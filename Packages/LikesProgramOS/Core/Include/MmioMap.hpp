/* MmioMap.hpp
    内核侧高地址 MMIO 映射窗口：把任意物理区间映射到固定的高地址虚拟窗口

    窗口基址与跨度来自 Paging.hpp 的 kMmioWindowBase / kMmioWindowSpanBytes；窗口自己的
    静态 PDPT 与四张 PD 表在本实现内，2 MiB 区段的 PT 页帧从绑定的页帧分配器按需申请，
    每个 4 KiB 页一条 PTE（设备 MMIO 必须 4 KiB 粒度，不用大页），物理地址允许超过 4 GiB
    安装窗口走 Paging 的 InstallVirtualWindow，在第一次 Map 时惰性完成；Unmap 与 Map 结束
    都经 FlushTlb 重载 CR3，避免 TLB 里残留旧翻译
    PT 页帧不做回收（窗口映射上限 64 条），Unmap 只清 PTE；同一 2 MiB 区段
    重复使用时复用已分配的 PT
    失败只返回 nullptr / false，不打印、不停机：调用方（Core.Mmio 槽）把失败翻译成
    模块侧的 not mappable
*/

#pragma once
#include <stdint.h>

#include <Memory.hpp>

namespace Kernel {
    namespace MmioWindow {
        // 同时存在的窗口映射上限
        constexpr uint32_t kMaxMappings = 64;
        // 单条映射上限：bytes 按 4 KiB 上取整后不得超过它
        constexpr uint64_t kMaxMappingBytes = 64 * 1024 * 1024;

        // 绑定页帧分配器：由 ModuleRegistry 的 BindCoreServices 调用一次
        // 传空指针解除绑定并清空映射表；未绑定时 Map 返回 nullptr
        void Bind(PageAllocator* pages);

        // 把物理区间 [physical, physical+bytes) 映射进窗口，返回虚拟基址；失败返回 nullptr
        // physical 必须按 4 KiB 对齐；cachePolicy 取 Core.Mmio 槽 2 的参数，当前只实现
        // 设备 MMIO（0 与其他值都按 PCD|PWT 处理）
        void* Map(uint64_t physical, uint64_t bytes, uint32_t cachePolicy);

        // 解除一条映射：base 必须是 Map 的返回值且落在窗口内；只清 PTE，不回收 PT 页帧
        bool Unmap(void* base);

        // 窗口反查：窗口内虚拟地址 → 物理地址与该条映射的总长度；不在窗口或未映射返回 false
        bool Lookup(const void* address, uint64_t* physical, uint64_t* bytes);
    }
}
