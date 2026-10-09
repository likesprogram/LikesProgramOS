/* Tables.cpp
    内核描述符表的建立与装载：段表、TSS 与中断表

    顺序固定：先段表与 TSS（含 LTR），再中断表；两次装载都在中断关闭时进行，
    新表的段基址都是 0，与入口时的平坦段一致，重载后指令流与栈落在同一地址
    异常桩的入口地址由 Core.asm 提供，这里只负责把它们写进门描述符
*/

#include <Tables.hpp>

#include <Libc.hpp>

#include <Platform/Descriptor.hpp>

#include <stddef.h>

namespace Kernel {
    // Core.asm 提供的入口：内核栈顶与异常桩地址表
    // extern "C" 声明放在匿名命名空间之外：匿名命名空间会给出内部链接，符号名对不上
    extern "C" const uint8_t _Kernel_Stack_Top[];
    extern "C" const uintptr_t _Kernel_Exception_Stubs[32];

    namespace {
        // 异常向量数量：0 至 31，不含外部中断
        constexpr uint32_t kExceptionVectors = 32;
        // 双故障与 NMI 使用的 IST 应急栈，在未初始化区
        constexpr uint32_t kIstStackBytes = 0x2000;

        // IA-32e 的 TSS：104 字节，字段偏移由 SDM 固定；按硬件布局打包，
        // 不套 C++ 的对齐规则（RSP0 落在偏移 4，未经填充）
        struct __attribute__((packed)) Tss {
            uint32_t reserved0;    // 保留
            uint64_t rsp0;         // 特权级 0 栈指针，从低特权级进入内核时装载
            uint64_t rsp1;         // 特权级 1 栈指针，未使用
            uint64_t rsp2;         // 特权级 2 栈指针，未使用
            uint64_t reserved1;    // 保留
            uint64_t ist[7];       // IST1 至 IST7 的栈指针
            uint64_t reserved2;    // 保留
            uint16_t reserved3;    // 保留
            uint16_t ioMapBase;    // I/O 许可位图基址，取 TSS 长度表示没有位图
        };
        static_assert(sizeof(Tss) == 104, "IA-32e 的 TSS 必须是 104 字节");
        static_assert(offsetof(Tss, ist) == 0x24, "IA-32e 的 IST 数组必须落在偏移 0x24");

        // 内核自己的表实例与应急栈：静态存储，不依赖启动运行库
        Platform::Gdt s_gdt;
        Platform::Idt s_idt;
        Tss s_tss;
        alignas(16) uint8_t s_istStack[kIstStackBytes];
    }

    const char* InstallDescriptorTables() {
        s_gdt.Clear();
        if (!s_gdt.SetCode(1, 0, Platform::CodeWidth::Bits64)) return "Set the kernel code segment";
        if (!s_gdt.SetData(2, 0, Platform::DataWidth::Bits32)) return "Set the kernel data segment";
        if (!s_gdt.SetCode(3, 3, Platform::CodeWidth::Bits64)) return "Set the user code segment";
        if (!s_gdt.SetData(4, 3, Platform::DataWidth::Bits32)) return "Set the user data segment";

        // TSS：RSP0 指向内核入口栈，IST1 指向应急栈；没有 I/O 许可位图
        memset(&s_tss, 0, sizeof(s_tss));
        s_tss.rsp0 = reinterpret_cast<uintptr_t>(_Kernel_Stack_Top);
        s_tss.ist[0] = reinterpret_cast<uintptr_t>(s_istStack) + kIstStackBytes;
        s_tss.ioMapBase = sizeof(Tss);
        if (!s_gdt.SetTss(5, reinterpret_cast<uintptr_t>(&s_tss), sizeof(Tss) - 1)) return "Set the TSS descriptor";
        s_gdt.Load(kSelectorKernelCode, kSelectorKernelData);
        Platform::LoadTaskRegister(kSelectorTss);

        s_idt.Clear();
        for (uint32_t vector = 0; vector < kExceptionVectors; ++vector) {
            // 双故障（8）与 NMI（2）走 IST1：当前栈不可信时仍能留下诊断
            const uint8_t ist = (vector == 2 || vector == 8) ? 1 : 0;
            if (!s_idt.SetGate(vector, _Kernel_Exception_Stubs[vector], kSelectorKernelCode, Platform::GateType::Interrupt, 0, ist))
                return "Set the exception gate";
        }
        s_idt.Load();
        return nullptr;
    }
}
