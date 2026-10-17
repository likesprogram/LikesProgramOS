/* Memory.cpp
    物理页帧分配器的实现：位图标记与连续页分配

    页大小是 2 的幂，页号与对齐换算全部用移位与掩码，不牵入 64 位除法辅助函数
    分配用首次适配：从低页号线性扫描，找到第一段满足长度的连续空闲页；
    引导阶段分配次数很少，不为此维护空闲链表
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
        const uint64_t first = AlignUpPage(begin) >> kPageShift;
        const uint64_t last = AlignDownPage(end) >> kPageShift;
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

    uint64_t PageAllocator::Initialize(const Boot::Info& info, uint64_t kernelBegin, uint64_t kernelEnd) {
        memset(m_bitmap, 0, sizeof(m_bitmap));   // 全部占用，随后只放行内存图里的可用区间
        m_freePages = 0;
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
}
