/* Memory.hpp
    物理页帧分配器：按 BootInfo 的内存图管理可用物理页，位图存放在未初始化区

    页面分配返回物理地址；访问内容经 Paging 的直映射窗口换算。分配器不知道虚拟地址映射，
    调用方需要线性地址时自行换算，DMA 与页表继续使用物理地址
    保留区间按保守口径处理：内核映像起点以下的全部地址（含固件、Stub 与 BaleenCore 的
    常驻区间）、内核映像与静态内存跨度自身、BootInfo 结构与其内存图数组
    引导器的所有权清单随 BootInfo 扩展，当前不细分谁占用了低地址

    交接区间（预装模块字节、清单与列表副本、预装记录数组）由调用方在 Initialize 之后用
    Reserve 逐段保留，Initialize 不接收区间参数：交接区间的段数与来源会随实现演进，一段
    一个 Reserve 比每轮改 Initialize 签名更稳，也不逼既有调用点同步改
    顺序约定是 Initialize → 各段 Reserve → 首次 Allocate；Allocate 从不返回已保留页
    Initialize 自己标记的跨度（内核、引导器、BootInfo）不是预留项，ReleaseReserved 不释放
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
    // 预留表规模：交接包的清单/列表/记录缓冲与预装区间各一项，16 项留余量；表满返回 NoRoom，不动态增长
    constexpr uint32_t kMaxReservations = 16;

    // 预留与释放操作的结果：Ok 为成功，失败值可区分原因；诊断编号由调用方按失败点选，本层不产生编号
    enum class ReserveResult : uint32_t {
        Ok = 0,        // 成功
        ZeroLength,    // 区间长度为 0
        Unaligned,     // 起址或长度未按 4 KiB 页对齐
        OutOfRange,    // 区间超出管理的低 4GiB
        Overflow,      // 起址加长度溢出 64 位
        Occupied,      // 区间内有页已被占用、已分配或已保留
        NoRoom,        // 预留表已满
        NotFound,      // ReleaseReserved 找不到与入参完全相同的预留项
    };

    // 物理页帧分配器：位图固定在未初始化区，不做动态增长
    class PageAllocator {
    public:
        // 按内存图初始化：位图先全部标记占用，再放行可用区间，最后保留内核与交接事实
        // kernelBegin 与 kernelEnd 是内核映像与静态内存的物理跨度；可重复调用，返回放行的页数
        // 重复调用会清空预留表；交接区间在它之后用 Reserve 保留
        uint64_t Initialize(const Boot::Info& info, uint64_t kernelBegin, uint64_t kernelEnd);
        // 分配 count 个连续页帧，返回起始物理地址；不足时返回 0
        uint64_t Allocate(uint32_t count);
        // 归还 count 个连续页帧；入参须来自 Allocate 且尚未归还
        void Free(uint64_t physical, uint32_t count);
        // 预留一段物理区间：起址与长度按页对齐、整段在管理范围内、所有页当前空闲才成功
        // 成功后 Allocate 不再返回这些页；失败不改变位图与预留表
        ReserveResult Reserve(uint64_t physical, uint64_t bytes);
        // 释放 Reserve 建立的预留：入参须与预留项的起址和长度完全相同
        // 释放后这些页重新可分配；Initialize 标记的跨度与已分配页不是预留项，返回 NotFound
        ReserveResult ReleaseReserved(uint64_t physical, uint64_t bytes);
        // 管理的页帧总数
        uint64_t ManagedPages() const { return kManagedPages; }
        // 当前空闲页帧数
        uint64_t FreePages() const { return m_freePages; }

    private:
        // 预留项：起址与长度，页对齐；只由 Reserve / ReleaseReserved 维护
        struct Reservation {
            uint64_t physical;   // 预留区间起址，页对齐
            uint64_t bytes;      // 预留区间长度，页对齐
        };
        // 把一段页帧标记为占用或空闲；地址越出管理范围的部分被截断
        // 占用方向覆盖区间碰到的每一页，放行方向只放整页（取整方向见 MarkRange）
        void MarkUsed(uint64_t physical, uint64_t bytes);
        void MarkFree(uint64_t physical, uint64_t bytes);
        // 一页是否空闲
        bool IsFree(uint64_t page) const;
        // 把一段地址范围标记为空闲/占用：占用方向起址下取整、终址上取整，空闲方向相反
        void MarkRange(uint64_t begin, uint64_t end, bool free);
        // 区间内是否所有页都空闲；入参已保证按页对齐且在管理范围内
        bool RangeFree(uint64_t physical, uint64_t bytes) const;
        // 区间是否与任一预留项重叠；入参已保证不溢出
        bool OverlapsReservation(uint64_t physical, uint64_t bytes) const;
        // 查找与入参完全相同的预留项下标；找不到返回 kMaxReservations
        uint32_t FindReservation(uint64_t physical, uint64_t bytes) const;
        uint64_t m_bitmap[kManagedPages / 64] = {};         // 位图：1 为空闲，0 为占用
        uint64_t m_freePages = 0;                           // 空闲页帧数
        uint32_t m_reservationCount = 0;                    // 预留表已用项数
        Reservation m_reservations[kMaxReservations] = {};  // 预留表：前 m_reservationCount 项有效
    };
}
