/* UefiDiagnostics.hpp
    UEFI 阶段的失败诊断编号：本包各处共用的稳定编号与文本

    16..30、32、55 与 58 与 BaleenCore 同号异前缀（同一含义、同一序号、同一句话，只换
    阶段前缀），76 起是 UEFI 侧
    独有的检查，不参与同号异前缀
    共享实现（SelectSet / ManifestVerify / LoadKernel）自带前缀宏，本头只管本包自己判定的那些
*/

#pragma once
#include <Diagnostics.hpp>

namespace Baleen {
    namespace Uefi {
        // —— 与 BaleenCore 同号异前缀：同一含义的检查，序号两侧一致 ——
        constexpr Diagnostic kFailBindDisk{"E-UEFI-16", "Bind the disk service"};
        constexpr Diagnostic kFailReadLayout{"E-UEFI-17", "Read the layout descriptor"};
        constexpr Diagnostic kFailOpenVolume{"E-UEFI-28", "Open the system volume view"};
        constexpr Diagnostic kFailMountVolume{"E-UEFI-29", "Mount the system volume"};
        constexpr Diagnostic kFailVolumeUuid{"E-UEFI-30", "The system volume identifier doesn't match the layout descriptor"};
        constexpr Diagnostic kWarnKernelMissing{"E-UEFI-32", "The kernel image isn't in the system volume: the volume has no kernel yet"};
        constexpr Diagnostic kWarnBootBudget{"E-UEFI-55", "The boot time is beyond the budget"};
        // 系统卷里没有内核：本阶段到此为止，没有可交出去的东西（与 E-CORE-58 同号异前缀）
        constexpr Diagnostic kWarnNoKernel{"E-UEFI-58", "The UEFI stage ends here: there is no kernel to hand over"};

        // —— UEFI 侧独有：BaleenCore 没有对应检查 ——
        constexpr Diagnostic kFailNoBootServices{"E-UEFI-76", "The firmware boot services aren't available"};
        constexpr Diagnostic kFailOpenSetDirectory{"E-UEFI-77", "Open the kernel set directory"};
        constexpr Diagnostic kFailListSetDirectory{"E-UEFI-78", "Enumerate the kernel set directory"};
        constexpr Diagnostic kFailMemoryMapInit{"E-UEFI-79", "Allocate the memory map buffer"};
        constexpr Diagnostic kFailMemoryMapRead{"E-UEFI-80", "Capture the firmware memory map"};
        constexpr Diagnostic kFailMemoryMapFormat{"E-UEFI-81", "The firmware memory map format isn't supported"};
        constexpr Diagnostic kFailExitBootServices{"E-UEFI-82", "Exit the firmware boot services"};
        constexpr Diagnostic kWarnDeviceClueTooLong{"E-UEFI-83", "The boot device path is too long to pass on"};
        // 内核镜像先读进固件分配的缓冲；退出启动服务后固定装载跨度必须归系统才算可写
        constexpr Diagnostic kFailKernelMemory{"E-UEFI-86", "Allocate the kernel memory"};
        // 引导器占用区间必须在退出启动服务之前从已加载映像协议取出并保存
        constexpr Diagnostic kFailLoadedImageFacts{"E-UEFI-87", "The loaded image doesn't report its load span"};
        // 退出启动服务之后：固定装载跨度不在最终内存图的可用区间里，不能拷入内核
        constexpr Diagnostic kFailKernelSpanAfterExit{"E-UEFI-88", "The kernel load span isn't available after the boot services exit"};
    }
}
