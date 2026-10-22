/* Paging.hpp
    内核引导页表与物理内存直映射：4 级页表，恒等映射与直映射窗口各一份

    入口时分页已由引导器打开（长模式的前提）：内核映像按物理地址恒等映射，因此入口代码、
    栈与 BootInfo 都可按物理地址访问；KernelMain 建立自己的页表并切换 CR3，此后访问任意
    物理内存经 PhysToVirt 的直映射窗口，内核映像自身继续按恒等映射执行
    引导页表用 2MiB 大页覆盖低 4GiB，不按内存图做精细映射，也不区分缓存策略：
    精细映射与 MMIO 策略随设备阶段引入，当前只保证内核映像、引导数据与内存图区间可达
    当前只补了高地址 MMIO 的 4 KiB 映射窗口（见 kMmioWindowBase 与 MmioMap）：
    **模块映像仍按物理地址恒等映射**，没有按段设置 RX/RW/NX，不宣称 W^X——
    模块映像的 4 KiB 权限映射与 NX 是已知限制
*/

#pragma once
#include <stdint.h>

namespace Kernel {
    // 物理内存直映射窗口的虚拟基址：基址加物理地址即该物理地址的虚拟地址
    constexpr uintptr_t kPhysMapBase = 0xFFFF800000000000ull;
    // 引导页表覆盖的物理范围与页粒度：低 4GiB，2MiB 大页
    constexpr uint64_t kMapLimitBytes = 0x100000000ull;
    constexpr uint64_t kLargePageBytes = 0x200000ull;

    // 高地址 MMIO 映射窗口（4 KiB 映射最小面）：4 KiB PTE 按需建立，物理地址可在 4GiB 以上
    // 窗口落在 PML4 的一个独立槽位，与恒等映射、直映射窗口互不相交；容量与单条上限见 MmioMap.hpp
    constexpr uintptr_t kMmioWindowBase = 0xFFFF900000000000ull;
    constexpr uint64_t kMmioWindowSpanBytes = 0x100000000ull;
    constexpr uint32_t kMmioWindowPml4Index = static_cast<uint32_t>((kMmioWindowBase >> 39) & 0x1FF);

    // 把一个页表子树挂进内核 PML4 的窗口槽位并重载 CR3（TLB 全失效）
    // virtualBase 必须等于 kMmioWindowBase；成功返回空指针，失败返回原因文本
    const char* InstallVirtualWindow(uintptr_t virtualBase, uintptr_t pdptPhysical);

    // 重载 CR3 让页表修改生效：窗口映射的 Unmap 之后调用
    void FlushTlb();

    // 建立引导页表并装载 CR3：恒等映射保持映像、栈与引导数据的物理地址可用，
    // 直映射窗口供内核访问已映射的物理地址；切换前后执行的代码与数据都落在同一批物理页上
    // 成功返回空指针，失败返回原因文本；调用前页表存储已在未初始化区就绪
    const char* InstallBootPaging();

    // 物理地址到直映射窗口虚拟地址的换算；仅对引导页表覆盖范围内的地址有效
    inline void* PhysToVirt(uint64_t physical) {
        return reinterpret_cast<void*>(kPhysMapBase + physical);
    }

    // 直映射窗口虚拟地址到物理地址的换算；只用于 PhysToVirt 得到的地址
    inline uint64_t VirtToPhys(const void* address) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address)) - kPhysMapBase;
    }
}
