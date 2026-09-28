/* Descriptor.hpp
    GDT 与 IDT：表项编码、表存储与装载机制

    本文件只提供机制，不决定策略：描述符集、选择子布局、DPL、TSS 与 IST 的用法、处理器入口
    地址都由使用方给出。Baleen 与内核各持自己的表实例，差异落在填表内容上：Baleen 用平坦段
    加上停机诊断入口，内核用内核/用户段、TSS 与 IST 加上完整的异常与 IRQ 入口，机制本身共用
    表项形态按编译目标模式生成，同一份源码在两种模式下各自得到正确的字节布局：门描述符
    32 位保护模式 8 字节、IA-32e 长模式 16 字节，表寄存器基址 32 位、长模式 64 位。内核的
    64 位侧因此不能复用按 32 位编译的 Platform::Cpu（该文件对此有编译期拒绝），描述符表
    放在独立文件里以便两种模式分别编译
    模式提升与回退（实模式 → 保护模式 → 长模式）不在这里：目标代码段必须与当前模式同宽，
    带远跳转的切换留在各阶段汇编入口
*/
#pragma once
#include <stdint.h>

namespace Platform {
    // 代码段宽度：16 位、32 位与 IA-32e 长模式的 64 位
    enum class CodeWidth {
        Bits16,   // 16 位代码段，字节粒度限长
        Bits32,   // 32 位代码段，4KiB 粒度限长
        Bits64,   // 长模式 64 位代码段，置 L 位，限长被处理器忽略
    };

    // 数据段宽度：16 位与 32 位；长模式下数据段的 D/B 被忽略，按 32 位填写
    enum class DataWidth {
        Bits16,   // 16 位数据段，字节粒度限长
        Bits32,   // 32 位数据段，4KiB 粒度限长
    };

    // 门类型：中断门由处理器清 IF，陷阱门保留 IF
    enum class GateType {
        Interrupt,   // 中断门，类型值 0xE，32 位与长模式取值相同
        Trap,        // 陷阱门，类型值 0xF，32 位与长模式取值相同
    };

    // 全局描述符表：固定槽位的段描述符存储与装载入口
    // 只编码平坦基址 0 的段；基址非零的段、LDT、调用门与 16 位 TSS 不在当前范围内
    class Gdt {
    public:
        // 槽位数量上限；长模式下的系统描述符要占两个相邻槽位
        static constexpr uint32_t kMaxEntries = 32;

        // 清空全部槽位，得到一个只有空描述符的表；已装载的 GDTR 不受影响
        void Clear();
        // 写入代码段描述符：基址 0、可读、向上扩展；槽位越界或 DPL 大于 3 时返回 false
        bool SetCode(uint32_t index, uint8_t dpl, CodeWidth width);
        // 写入数据段描述符：基址 0、可写、向上扩展；槽位越界或 DPL 大于 3 时返回 false
        bool SetData(uint32_t index, uint8_t dpl, DataWidth width);
        // 写入 TSS 描述符：可用 TSS、DPL 0、字节粒度限长；长模式下占用 index 与 index+1 两个槽位，
        // 上一半放基址 63:32，槽位不足或限长超过 20 位编码范围时返回 false；
        // TSS 结构本身的布局与初始化由使用方负责
        bool SetTss(uint32_t index, uintptr_t base, uint32_t limit);
        // 装载 GDTR 并把 CS 与数据段寄存器重载到给定选择子
        // 调用前必须关中断；新表的段基址须与当前一致，否则重载后指令流与栈会指向别处
        void Load(uint16_t codeSelector, uint16_t dataSelector);

        // 表存储起始地址，可直接用于诊断输出
        const void* Base() const { return m_entries; }
        // 表限长，字节数减一
        uint32_t Limit() const { return sizeof(m_entries) - 1; }

    private:
        uint64_t m_entries[kMaxEntries] = {};   // 段描述符存储，每个槽位 8 字节，按小端连续排列
    };

    // 中断描述符表：256 个门槽位与装载入口
    // 处理器入口代码不在这里生成：Baleen 的停机诊断入口与内核的异常、IRQ 框架各自在自己的汇编里定义，
    // 本类只把入口地址写进门描述符
    class Idt {
    public:
        // 向量数量，由架构固定；表限长始终覆盖全部向量
        static constexpr uint32_t kVectorCount = 256;
#if defined(__x86_64__)
        // 门描述符宽度：IA-32e 长模式
        static constexpr uint32_t kEntryBytes = 16;
#else
        // 门描述符宽度：32 位保护模式
        static constexpr uint32_t kEntryBytes = 8;
#endif

        // 清空全部门，得到一个所有向量都不存在（不可用）的表；已装载的 IDTR 不受影响
        void Clear();
        // 设置一个门：向量越界、选择子为 0、DPL 大于 3、IST 越界或 32 位保护模式下 IST 非零时返回 false
        bool SetGate(uint32_t vector, uintptr_t handler, uint16_t selector, GateType type, uint8_t dpl, uint8_t ist);
        // 装载 IDTR；不改动任何段寄存器，也不改动中断标志
        void Load() const;

        // 表存储起始地址，可直接用于诊断输出
        const void* Base() const { return m_entries; }
        // 表限长，字节数减一
        uint32_t Limit() const { return sizeof(m_entries) - 1; }

    private:
        uint64_t m_entries[kVectorCount * (kEntryBytes / sizeof(uint64_t))] = {};   // 门存储，按小端连续排列
    };

    // 把 TSS 选择子装入任务寄存器（LTR）：仅特权级 0 可用，选择子须指向表内的可用 TSS 描述符
    // 长模式下处理器按 16 字节描述符取得 64 位基址；内核的 IST 与特权级栈切换依赖它，Baleen 不建立 TSS
    void LoadTaskRegister(uint16_t selector);
}
