/* MmioMap.cpp
    MMIO 映射窗口实现：静态 PDPT + 静态 PD 表 + 按需 PT，4 KiB PTE 逐页填

    窗口基址按 1 GiB 对齐，因此 PDPT 下标与 PD 下标都能用「相对窗口基址的偏移」直接算：
    offset >> 30 是 PDPT 下标（每项 1 GiB）、(offset >> 21) & 0x1FF 是 PD 下标（每项 2 MiB）、
    (offset >> 12) & 0x1FF 是 PT 下标（每项 4 KiB）
    表页与 PTE 都按物理地址写：内核低 4 GiB 有恒等映射，页帧分配器返回的物理地址可以直接
    当线性地址用，不需要经直映射窗口
    虚拟地址分配是单向 bump：窗口 4 GiB、单条上限 64 MiB，64 条映射刚好放得下；Unmap
    不回收虚拟区间（窗口容量按一次性分配设计）
*/

#include <MmioMap.hpp>

#include <Paging.hpp>

namespace {
    // 页表项标志：存在可写；设备 MMIO 额外置 PWT 与 PCD，不做 NX 处理
    constexpr uint64_t kPtePresentRw = 0x03;
    constexpr uint64_t kPteWriteThrough = 1ull << 3;
    constexpr uint64_t kPteCacheDisable = 1ull << 4;
    constexpr uint64_t kMmioPteFlags = kPtePresentRw | kPteWriteThrough | kPteCacheDisable;
    // 页表项里的物理地址掩码：低 12 位是标志
    constexpr uint64_t kPteAddressMask = ~0xFFFull;
    // PD 表数量：窗口 4 GiB，每张 PD 覆盖 512 × 2 MiB = 1 GiB
    constexpr uint32_t kPdTableCount = 4;
    constexpr uint64_t kPdTableBytes = 512ull * Kernel::kLargePageBytes;
    constexpr uint64_t kPdEntries = 512;
    constexpr uint64_t kPtEntries = 512;

    static_assert(Kernel::kMmioWindowSpanBytes <= kPdTableCount * kPdTableBytes,
        "MMIO 窗口跨度超过静态 PD 表覆盖范围：需要增加 kPdTableCount");
    static_assert((Kernel::kMmioWindowBase & (kPdTableBytes - 1)) == 0,
        "MMIO 窗口基址必须按 1 GiB 对齐，PDPT / PD 下标才能按相对偏移计算");
    static_assert(Kernel::MmioWindow::kMaxMappingBytes % Kernel::kPageBytes == 0,
        "单条映射上限必须是 4 KiB 的整数倍");
    static_assert(Kernel::MmioWindow::kMaxMappings * Kernel::MmioWindow::kMaxMappingBytes
        <= Kernel::kMmioWindowSpanBytes,
        "映射上限与单条上限的乘积不得超过窗口跨度");

    // 窗口自己的 PDPT 与 PD 表：静态 .bss，恒等映射下按地址直接写
    alignas(4096) uint64_t s_pdpt[512];
    alignas(4096) uint64_t s_pdTables[kPdTableCount][512];

    // 绑定的页帧分配器与窗口状态
    Kernel::PageAllocator* s_pages = nullptr;
    uint32_t s_windowInstalled = 0;
    uint32_t s_count = 0;
    uint64_t s_nextOffset = 0;

    // 一条窗口映射：窗口内偏移、物理起址与按页上取整后的长度
    struct Mapping {
        uint64_t offset;    // 相对窗口基址的偏移，4 KiB 对齐
        uint64_t physical;  // 物理起址
        uint64_t bytes;     // 映射长度，4 KiB 整数倍
    };
    Mapping s_mappings[Kernel::MmioWindow::kMaxMappings];

    // 页表页的写指针：物理地址即线性地址（低 4 GiB 恒等映射）
    uint64_t* TablePage(uint64_t physical) {
        return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(physical));
    }

    // 取一个对象的物理地址：静态表在恒等映射区
    uint64_t PhysicalOf(const void* address) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address));
    }

    // 惰性安装窗口：填 PDPT 的四条 PD 指针，再交给 Paging 挂进内核 PML4
    bool EnsureWindow() {
        if (s_windowInstalled != 0) return true;
        for (uint32_t index = 0; index < kPdTableCount; ++index) {
            s_pdpt[index] = PhysicalOf(s_pdTables[index]) | kPtePresentRw;
        }
        if (Kernel::InstallVirtualWindow(static_cast<uintptr_t>(Kernel::kMmioWindowBase),
                static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(s_pdpt))) != nullptr) {
            return false;
        }
        s_windowInstalled = 1;
        return true;
    }

    // 取（必要时分配并清零）某个 2 MiB 区段的 PT，返回可写指针；失败返回空
    uint64_t* PageTableFor(uint64_t offset) {
        const uint64_t pdptIndex = offset >> 30;
        const uint64_t pdIndex = (offset >> 21) & 0x1FF;
        if (pdptIndex >= kPdTableCount) return nullptr;
        const uint64_t entry = s_pdTables[pdptIndex][pdIndex];
        if ((entry & kPtePresentRw) != 0) return TablePage(entry & kPteAddressMask);
        if (s_pages == nullptr) return nullptr;
        const uint64_t physical = s_pages->Allocate(1);
        if (physical == 0) return nullptr;
        uint64_t* table = TablePage(physical);
        for (uint64_t index = 0; index < kPtEntries; ++index) table[index] = 0;
        s_pdTables[pdptIndex][pdIndex] = physical | kPtePresentRw;
        return table;
    }

    // 清一条映射覆盖的 PTE：PT 不存在时跳过
    void ClearMappingPages(uint64_t offset, uint64_t bytes) {
        for (uint64_t position = 0; position < bytes; position += Kernel::kPageBytes) {
            const uint64_t pageOffset = offset + position;
            const uint64_t pdptIndex = pageOffset >> 30;
            const uint64_t pdIndex = (pageOffset >> 21) & 0x1FF;
            if (pdptIndex >= kPdTableCount) continue;
            const uint64_t entry = s_pdTables[pdptIndex][pdIndex];
            if ((entry & kPtePresentRw) == 0) continue;
            uint64_t* table = TablePage(entry & kPteAddressMask);
            table[(pageOffset >> 12) & 0x1FF] = 0;
        }
    }
}

namespace Kernel {
    namespace MmioWindow {
        void Bind(PageAllocator* pages) {
            s_pages = pages;
            if (pages == nullptr) {
                for (uint32_t index = 0; index < kMaxMappings; ++index) s_mappings[index] = Mapping{};
                s_count = 0;
                s_nextOffset = 0;
            }
        }

        void* Map(uint64_t physical, uint64_t bytes, uint32_t cachePolicy) {
            // 当前只有设备 MMIO 一种策略：PCD|PWT 在 PTE 标志里固定，参数只为接口对齐
            (void)cachePolicy;
            if (s_pages == nullptr || bytes == 0) return nullptr;
            if ((physical % kPageBytes) != 0) return nullptr;
            if (bytes > kMaxMappingBytes) return nullptr;
            if (physical > ~0ull - bytes) return nullptr;
            const uint64_t rounded = (bytes + kPageBytes - 1) / kPageBytes * kPageBytes;
            if (s_count >= kMaxMappings) return nullptr;
            if (s_nextOffset > kMmioWindowSpanBytes || rounded > kMmioWindowSpanBytes - s_nextOffset) return nullptr;
            if (!EnsureWindow()) return nullptr;
            const uint64_t offset = s_nextOffset;
            // 逐页填 PTE：跨 2 MiB 边界时 PageTableFor 会自动取下一张 PT
            uint64_t position = 0;
            for (; position < rounded; position += kPageBytes) {
                uint64_t* table = PageTableFor(offset + position);
                if (table == nullptr) break;
                table[((offset + position) >> 12) & 0x1FF] = (physical + position) | kMmioPteFlags;
            }
            if (position < rounded) {
                // 中途失败：清掉已经写进去的 PTE，PT 页帧按不回收策略保留
                ClearMappingPages(offset, position);
                return nullptr;
            }
            s_mappings[s_count].offset = offset;
            s_mappings[s_count].physical = physical;
            s_mappings[s_count].bytes = rounded;
            ++s_count;
            s_nextOffset += rounded;
            FlushTlb();
            return reinterpret_cast<void*>(static_cast<uintptr_t>(kMmioWindowBase + offset));
        }

        bool Unmap(void* base) {
            if (base == nullptr) return false;
            const uint64_t virtualAddress = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(base));
            if (virtualAddress < kMmioWindowBase) return false;
            const uint64_t offset = virtualAddress - kMmioWindowBase;
            if (offset >= kMmioWindowSpanBytes) return false;
            for (uint32_t index = 0; index < s_count; ++index) {
                const Mapping& mapping = s_mappings[index];
                if (mapping.offset != offset) continue;
                ClearMappingPages(mapping.offset, mapping.bytes);
                for (uint32_t move = index + 1; move < s_count; ++move) s_mappings[move - 1] = s_mappings[move];
                --s_count;
                s_mappings[s_count] = Mapping{};
                FlushTlb();
                return true;
            }
            return false;
        }

        bool Lookup(const void* address, uint64_t* physical, uint64_t* bytes) {
            if (address == nullptr || physical == nullptr || bytes == nullptr) return false;
            const uint64_t virtualAddress = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address));
            if (virtualAddress < kMmioWindowBase) return false;
            const uint64_t offset = virtualAddress - kMmioWindowBase;
            if (offset >= kMmioWindowSpanBytes) return false;
            for (uint32_t index = 0; index < s_count; ++index) {
                const Mapping& mapping = s_mappings[index];
                if (offset < mapping.offset) continue;
                if (offset - mapping.offset >= mapping.bytes) continue;
                *physical = mapping.physical + (offset - mapping.offset);
                *bytes = mapping.bytes;
                return true;
            }
            return false;
        }
    }
}
