/* Memory.cpp
    物理页帧分配器的实现：位图标记、连续页分配与交接区间的预留

    页大小是 2 的幂，页号与对齐换算全部用移位与掩码，不牵入 64 位除法辅助函数
    分配用首次适配：从低页号线性扫描，找到第一段满足长度的连续空闲页；
    引导阶段分配次数很少，不为此维护空闲链表
    预留只维护一张固定大小的表：Reserve 校验通过后在位图上标占并登记，ReleaseReserved
    只接受起址与长度都相同的登记项——内核、引导器与 BootInfo 跨度没有登记项，释放它们
    只会得到 NotFound；失败路径不改变位图与预留表，调用方可以报告原因后继续
*/

#include <Memory.hpp>

#include <Libc.hpp>

#include <Paging.hpp>

namespace Kernel {
    namespace {
        // 页大小的移位量
        constexpr uint32_t kPageShift = 12;
        static_assert((1ull << kPageShift) == kPageBytes, "页粒度与移位量不一致");

        // 地址按页上取整 / 下取整
        uint64_t AlignUpPage(uint64_t address) { return (address + kPageBytes - 1) >> kPageShift << kPageShift; }
        uint64_t AlignDownPage(uint64_t address) { return address >> kPageShift << kPageShift; }
    }

    void PageAllocator::MarkRange(uint64_t begin, uint64_t end, bool free) {
        if (begin >= kManagedLimitBytes) return;
        if (end > kManagedLimitBytes) end = kManagedLimitBytes;
        // 取整方向由语义决定：标占用必须盖住区间碰到的每一页（起址下取整、终址上取整），
        // 放行只能放整页（起址上取整、终址下取整）；反过来会把半页错误地交出去或放出去
        const uint64_t first = (free ? AlignUpPage(begin) : AlignDownPage(begin)) >> kPageShift;
        const uint64_t last = (free ? AlignDownPage(end) : AlignUpPage(end)) >> kPageShift;
        for (uint64_t page = first; page < last; ++page) {
            const uint64_t word = page >> 6;
            const uint64_t bit = 1ull << (page & 63);
            if (free) {
                if ((m_bitmap[word] & bit) == 0) {
                    m_bitmap[word] |= bit;
                    ++m_freePages;
                }
            } else if ((m_bitmap[word] & bit) != 0) {
                m_bitmap[word] &= ~bit;
                --m_freePages;
            }
        }
    }

    void PageAllocator::MarkUsed(uint64_t physical, uint64_t bytes) { MarkRange(physical, physical + bytes, false); }

    void PageAllocator::MarkFree(uint64_t physical, uint64_t bytes) { MarkRange(physical, physical + bytes, true); }

    bool PageAllocator::IsFree(uint64_t page) const { return ((m_bitmap[page >> 6] >> (page & 63)) & 1) != 0; }

    bool PageAllocator::RangeFree(uint64_t physical, uint64_t bytes) const {
        const uint64_t first = physical >> kPageShift;
        const uint64_t pages = bytes >> kPageShift;
        for (uint64_t index = 0; index < pages; ++index) {
            if (!IsFree(first + index)) return false;
        }
        return true;
    }

    bool PageAllocator::OverlapsReservation(uint64_t physical, uint64_t bytes) const {
        const uint64_t end = physical + bytes;
        for (uint32_t index = 0; index < m_reservationCount; ++index) {
            const Reservation& entry = m_reservations[index];
            if (physical < entry.physical + entry.bytes && entry.physical < end) return true;
        }
        return false;
    }

    uint32_t PageAllocator::FindReservation(uint64_t physical, uint64_t bytes) const {
        for (uint32_t index = 0; index < m_reservationCount; ++index) {
            if (m_reservations[index].physical == physical && m_reservations[index].bytes == bytes) return index;
        }
        return kMaxReservations;
    }

    uint64_t PageAllocator::Initialize(const Boot::Info& info, uint64_t kernelBegin, uint64_t kernelEnd) {
        memset(m_bitmap, 0, sizeof(m_bitmap));   // 全部占用，随后只放行内存图里的可用区间
        m_freePages = 0;
        m_reservationCount = 0;   // 预留表随位图一起复位：交接区间按新的交接事实重新预留
        // 内存图条目已由 Stub 按属性位过滤：这里只按类型取可用区间，长度越界部分被截断
        if (info.memoryMap != 0) {
            const Boot::MemoryMapEntry* entries = static_cast<const Boot::MemoryMapEntry*>(PhysToVirt(info.memoryMap));
            for (uint32_t i = 0; i < info.memoryMapCount; ++i) {
                const Boot::MemoryMapEntry& entry = entries[i];
                if (entry.type != 1) continue;
                if (entry.base >= kManagedLimitBytes) continue;
                uint64_t end = kManagedLimitBytes;
                if (entry.length <= kManagedLimitBytes - entry.base) end = entry.base + entry.length;
                MarkFree(entry.base, end - entry.base);
            }
        }
        // 保留：内核映像起点以下的全部地址、内核跨度自身、BootInfo 与其内存图数组
        MarkRange(0, kernelBegin, false);
        MarkRange(kernelBegin, kernelEnd, false);
        const uint64_t infoBegin = reinterpret_cast<uintptr_t>(&info);
        MarkUsed(infoBegin, sizeof(info));
        if (info.memoryMap != 0) MarkUsed(info.memoryMap, static_cast<uint64_t>(info.memoryMapCount) * sizeof(Boot::MemoryMapEntry));
        // 引导器自身的残留不能当可用内存：它在交权后冻结常驻，放行给内核就是踩运行中的代码与数据
        if (info.loaderBytes != 0) MarkUsed(info.loaderBegin, info.loaderBytes);
        return m_freePages;
    }

    uint64_t PageAllocator::Allocate(uint32_t count) {
        if (count == 0 || count > m_freePages) return 0;
        uint64_t runStart = 0;   // 当前连续空闲段的起始页号
        uint32_t run = 0;        // 当前连续空闲段的长度
        for (uint64_t page = 0; page < kManagedPages; ++page) {
            if (!IsFree(page)) {
                run = 0;
                continue;
            }
            if (run == 0) runStart = page;
            ++run;
            if (run == count) {
                MarkUsed(runStart << kPageShift, static_cast<uint64_t>(count) << kPageShift);
                return runStart << kPageShift;
            }
        }
        return 0;
    }

    void PageAllocator::Free(uint64_t physical, uint32_t count) {
        if (count == 0) return;
        MarkFree(physical, static_cast<uint64_t>(count) << kPageShift);
    }

    ReserveResult PageAllocator::Reserve(uint64_t physical, uint64_t bytes) {
        if (bytes == 0) return ReserveResult::ZeroLength;
        // 位图按页管理，未对齐的起址或长度会落到半页上，直接拒绝而不是悄悄扩成整页
        if (((physical | bytes) & (kPageBytes - 1)) != 0) return ReserveResult::Unaligned;
        if (bytes > UINT64_MAX - physical) return ReserveResult::Overflow;
        if (physical >= kManagedLimitBytes || physical + bytes > kManagedLimitBytes) return ReserveResult::OutOfRange;
        // 位图与预留表都要查：位图回答“页是否空闲”，预留表保证登记项互不重叠
        if (OverlapsReservation(physical, bytes) || !RangeFree(physical, bytes)) return ReserveResult::Occupied;
        if (m_reservationCount >= kMaxReservations) return ReserveResult::NoRoom;
        m_reservations[m_reservationCount].physical = physical;
        m_reservations[m_reservationCount].bytes = bytes;
        ++m_reservationCount;
        MarkUsed(physical, bytes);
        return ReserveResult::Ok;
    }

    ReserveResult PageAllocator::ReleaseReserved(uint64_t physical, uint64_t bytes) {
        if (bytes == 0) return ReserveResult::ZeroLength;
        if (((physical | bytes) & (kPageBytes - 1)) != 0) return ReserveResult::Unaligned;
        if (bytes > UINT64_MAX - physical) return ReserveResult::Overflow;
        if (physical >= kManagedLimitBytes || physical + bytes > kManagedLimitBytes) return ReserveResult::OutOfRange;
        const uint32_t index = FindReservation(physical, bytes);
        if (index == kMaxReservations) return ReserveResult::NotFound;
        // 后面的项前移，保持预留表的顺序与预留先后一致；项数上限 16，移动代价可忽略
        for (uint32_t move = index + 1; move < m_reservationCount; ++move) m_reservations[move - 1] = m_reservations[move];
        --m_reservationCount;
        MarkFree(physical, bytes);
        return ReserveResult::Ok;
    }
}
