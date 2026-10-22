/* Tables.hpp
    内核描述符表：GDT、TSS 与 IDT 的建立与装载

    机制取自 Packages/Common 的 Platform::Gdt / Platform::Idt（按 64 位编译一份），
    策略由内核给出：内核与用户段、DPL、TSS 与 IST、异常门指向 Core.asm 的桩
*/

#pragma once
#include <stdint.h>

namespace Kernel {
    // 内核选择子：段表槽位编号与选择子布局
    constexpr uint16_t kSelectorKernelCode = 0x08;   // 内核 64 位代码段
    constexpr uint16_t kSelectorKernelData = 0x10;   // 内核数据段
    constexpr uint16_t kSelectorUserCode = 0x1B;     // 用户 64 位代码段，DPL 3
    constexpr uint16_t kSelectorUserData = 0x23;     // 用户数据段，DPL 3
    constexpr uint16_t kSelectorTss = 0x28;          // TSS，长模式下占两个槽位

    // 建立并装载内核的段表、任务寄存器与中断表：异常门全部指向 Core.asm 的桩
    // 双故障与 NMI 走 IST1 的应急栈，栈损坏时仍能留下诊断；调用前中断须关闭
    // 成功返回空指针，失败返回原因文本
    const char* InstallDescriptorTables();
}
