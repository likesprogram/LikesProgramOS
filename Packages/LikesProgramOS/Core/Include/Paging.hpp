/* Paging.hpp
    内核引导页表与物理内存直映射：4 级页表，恒等映射与直映射窗口各一份

    入口时分页已由引导器打开（长模式的前提）：内核映像按物理地址恒等映射，因此入口代码、
    栈与 BootInfo 都可按物理地址访问；KernelMain 建立自己的页表并切换 CR3，此后访问任意
    物理内存经 PhysToVirt 的直映射窗口，内核映像自身继续按恒等映射执行
    引导页表用 2MiB 大页覆盖低 4GiB，不按内存图做精细映射，也不区分缓存策略：
    精细映射与 MMIO 策略随设备阶段引入，当前只保证内核映像、引导数据与内存图区间可达
*/

#pragma once
#include <stdint.h>

namespace Kernel {
    // 物理内存直映射窗口的虚拟基址：基址加物理地址即该物理地址的虚拟地址
    constexpr uintptr_t kPhysMapBase = 0xFFFF800000000000ull;
    // 引导页表覆盖的物理范围与页粒度：低 4GiB，2MiB 大页
    constexpr uint64_t kMapLimitBytes = 0x100000000ull;
    constexpr uint64_t kLargePageBytes = 0x200000ull;

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
