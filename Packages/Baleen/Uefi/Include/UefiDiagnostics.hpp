/* UefiDiagnostics.hpp
    UEFI 阶段的失败诊断编号：本包各处共用的稳定编号与文本

    编号登记与含义见 Docs/Specs/Baleen/失败诊断编号.md 第四节；16..30、32 与 BaleenCore
    同号异前缀（同一含义、同一序号、同一句话，只换阶段前缀），76 起是 UEFI 侧独有的检查，
    不参与同号异前缀
    共享实现（SelectSet / ManifestVerify / LoadKernel / ImageHeader）自带前缀宏，
    本头只管本包自己判定的那些
*/

#pragma once
#include <Diagnostics.hpp>

namespace Baleen {
    namespace Uefi {
        // —— 与 BaleenCore 同号异前缀：同一含义的检查，序号两侧一致 ——
        constexpr Diagnostic kFailBindDisk{"E-UEFI-16", "Bind the disk service"};
        constexpr Diagnostic kFailReadLayout{"E-UEFI-17", "Read the layout descriptor"};
        constexpr Diagnostic kFailLayoutMagic{"E-UEFI-18", "The layout descriptor magic doesn't match"};
        constexpr Diagnostic kFailLayoutVersion{"E-UEFI-19", "The layout descriptor version isn't supported"};
        constexpr Diagnostic kFailLayoutHead{"E-UEFI-20", "The layout descriptor header length isn't supported"};
        constexpr Diagnostic kFailLayoutBytes{"E-UEFI-21", "The layout descriptor length doesn't match"};
        constexpr Diagnostic kFailLayoutFlags{"E-UEFI-22", "The layout descriptor flags aren't supported"};
        constexpr Diagnostic kFailLayoutReserved{"E-UEFI-23", "The layout descriptor reserved field isn't zero"};
        constexpr Diagnostic kFailLayoutChecksum{"E-UEFI-24", "The layout descriptor checksum doesn't match"};
        constexpr Diagnostic kFailLayoutPathBytes{"E-UEFI-25", "The layout descriptor kernel path length is invalid"};
        constexpr Diagnostic kFailLayoutPathAbsolute{"E-UEFI-26", "The layout descriptor kernel path isn't absolute"};
        constexpr Diagnostic kFailLayoutVolumeBytes{"E-UEFI-27", "The layout descriptor volume length is zero"};
        constexpr Diagnostic kFailOpenVolume{"E-UEFI-28", "Open the system volume view"};
        constexpr Diagnostic kFailMountVolume{"E-UEFI-29", "Mount the system volume"};
        constexpr Diagnostic kFailVolumeUuid{"E-UEFI-30", "The system volume identifier doesn't match the layout descriptor"};
        constexpr Diagnostic kWarnKernelMissing{"E-UEFI-32", "The kernel image isn't in the system volume: the volume has no kernel yet"};
        constexpr Diagnostic kWarnBootBudget{"E-UEFI-55", "The boot time is beyond the budget"};

        // —— UEFI 侧独有：BaleenCore 没有对应检查 ——
        constexpr Diagnostic kFailNoBootServices{"E-UEFI-76", "The firmware boot services aren't available"};
        constexpr Diagnostic kFailOpenSetDirectory{"E-UEFI-77", "Open the kernel set directory"};
        constexpr Diagnostic kFailListSetDirectory{"E-UEFI-78", "Enumerate the kernel set directory"};
        constexpr Diagnostic kFailMemoryMapInit{"E-UEFI-79", "Allocate the memory map buffer"};
        constexpr Diagnostic kFailMemoryMapRead{"E-UEFI-80", "Capture the firmware memory map"};
        constexpr Diagnostic kFailMemoryMapFormat{"E-UEFI-81", "The firmware memory map format isn't supported"};
        constexpr Diagnostic kFailExitBootServices{"E-UEFI-82", "Exit the firmware boot services"};
        constexpr Diagnostic kWarnDeviceClueTooLong{"E-UEFI-83", "The boot device path is too long to pass on"};
    }
}
