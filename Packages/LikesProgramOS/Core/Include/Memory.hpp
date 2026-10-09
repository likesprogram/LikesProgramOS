/* Memory.hpp
    物理页帧分配器：按 BootInfo 的内存图管理可用物理页，位图存放在未初始化区

    页面分配返回物理地址；访问内容经 Paging 的直映射窗口换算。分配器不知道虚拟地址映射，
    调用方需要线性地址时自行换算，DMA 与页表继续使用物理地址
    保留区间按保守口径处理：内核映像起点以下的全部地址（含固件、Stub 与 BaleenCore 的
    常驻区间）、内核映像与静态内存跨度自身、BootInfo 结构与其内存图数组
    引导器的所有权清单随 BootInfo 扩展，当前不细分谁占用了低地址
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>

namespace Kernel {
    // 页粒度与管理范围上限：页 4KiB，管理低 4GiB
    constexpr uint64_t kPageBytes = 0x1000;
    constexpr uint64_t kManagedLimitBytes = 0x100000000ull;
    // 位图规模：一位一页，1 表示空闲
    constexpr uint64_t kManagedPages = kManagedLimitBytes / kPageBytes;

    // 物理页帧分配器：位图固定在未初始化区，不做动态增长
    class PageAllocator {
    public:
        // 按内存图初始化：位图先全部标记占用，再放行可用区间，最后保留内核与交接事实
        // kernelBegin 与 kernelEnd 是内核映像与静态内存的物理跨度；可重复调用，返回放行的页数
        uint64_t Initialize(const Boot::Info& info, uint64_t kernelBegin, uint64_t kernelEnd);
        // 分配 count 个连续页帧，返回起始物理地址；不足时返回 0
        uint64_t Allocate(uint32_t count);
        // 归还 count 个连续页帧；入参须来自 Allocate 且尚未归还
        void Free(uint64_t physical, uint32_t count);
        // 管理的页帧总数
        uint64_t ManagedPages() const { return kManagedPages; }
        // 当前空闲页帧数
        uint64_t FreePages() const { return m_freePages; }

    private:
        // 把一段页帧标记为占用或空闲；地址越出管理范围的部分被截断
        void MarkUsed(uint64_t physical, uint64_t bytes);
        void MarkFree(uint64_t physical, uint64_t bytes);
        // 一页是否空闲
        bool IsFree(uint64_t page) const;
        // 把一段地址范围标记为空闲/占用，内部按页对齐后逐页处理
        void MarkRange(uint64_t begin, uint64_t end, bool free);
        uint64_t m_bitmap[kManagedPages / 64] = {};   // 位图：1 为空闲，0 为占用
        uint64_t m_freePages = 0;                     // 空闲页帧数
    };
}
