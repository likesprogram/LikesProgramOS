/* Descriptor.cpp
    GDT 与 IDT 的表项编码与表存储

    表项与表寄存器的字节布局依据 Intel SDM 第三卷的段描述符、门描述符与 GDTR/IDTR 定义
    纯汇编原语在 Descriptor.asm：表寄存器装载与段寄存器重载，位宽随目标模式
*/

#include <Platform/Descriptor.hpp>

// Descriptor.asm 提供的纯汇编原语，出入口约定随模式（32 位 cdecl，64 位 System V）
extern "C" void _Gdt_LoadPointer(const void* tablePointer);   // 装入 GDTR
extern "C" void _Idt_LoadPointer(const void* tablePointer);   // 装入 IDTR
extern "C" void _Gdt_ReloadSegments(uint16_t dataSelector);   // 重载数据段寄存器
extern "C" void _Gdt_ReloadCodeSegment(uint16_t codeSelector);   // 用远返回把 CS 换到新代码段
extern "C" void _Gdt_LoadTaskRegister(uint16_t selector);     // 装入任务寄存器

namespace {
    // —— 段描述符位域：SDM 的 8 字节段描述符按小端打包进一个 64 位槽位 ——
    constexpr uint32_t kSegmentBaseLowShift = 16;   // 基址 15:0
    constexpr uint32_t kSegmentBaseMidShift = 32;   // 基址 23:16
    constexpr uint32_t kSegmentAccessShift = 40;    // 访问权限字节：P、DPL、S 与类型
    constexpr uint32_t kSegmentFlagsShift = 48;     // 标志字节：G、D/B、L、AVL 与限长 19:16
    constexpr uint32_t kSegmentBaseHighShift = 56;  // 基址 31:24

    // —— 段描述符取值 ——
    constexpr uint8_t kAccessCode = 0x9A;         // P=1、DPL=0、S=1、E=1 可执行、RW=1 可读
    constexpr uint8_t kAccessData = 0x92;         // P=1、DPL=0、S=1、E=0、RW=1 可写
    constexpr uint8_t kAccessTss = 0x89;          // P=1、DPL=0、S=0、类型 9 可用的 TSS
    constexpr uint8_t kAccessDplShift = 5;        // DPL 在访问权限字节中的位移
    constexpr uint8_t kFlagGranularity = 0x80;    // G：限长按 4KiB 计
    constexpr uint8_t kFlagDefault32 = 0x40;      // D/B：32 位代码段与 32 位栈指针宽度
    constexpr uint8_t kFlagLong = 0x20;           // L：长模式 64 位代码段
    constexpr uint32_t kLimit16 = 0xFFFF;         // 16 位段限长，字节粒度下的 64KiB
    constexpr uint32_t kFlatLimit = 0xFFFFF;      // 平坦段限长，4KiB 粒度下的 4GiB，也是 20 位限长字段的上限
    constexpr uint8_t kMaxDpl = 3;                // DPL 字段两位

    // —— 门描述符取值 ——
    constexpr uint8_t kGatePresent = 0x80;        // P：门存在
    constexpr uint8_t kGateDplShift = 5;          // DPL 在类型属性字节中的位移
    constexpr uint8_t kGateInterrupt = 0x0E;      // 中断门类型值，32 位与长模式相同
    constexpr uint8_t kGateTrap = 0x0F;           // 陷阱门类型值，32 位与长模式相同
    constexpr uint8_t kMaxIst = 7;                // IST 字段三位

    // 一个门描述符占的 64 位槽位数
    constexpr uint32_t kGateWords = Platform::Idt::kEntryBytes / sizeof(uint64_t);

    // 表寄存器内容：32 位保护模式是 16 位限长加 32 位基址，长模式是 16 位限长加 64 位基址
    struct TablePointer {
        uint16_t limit;   // 表限长，字节数减一
#if defined(__x86_64__)
        uint64_t base;    // 表线性基址，长模式下为 64 位
#else
        uint32_t base;    // 表线性基址
#endif
    } __attribute__((packed));

    // 把 DPL 并入访问权限字节
    uint8_t AccessByte(uint8_t typeAccess, uint8_t dpl) { return static_cast<uint8_t>(typeAccess | (dpl << kAccessDplShift)); }

    // 打包平坦段描述符：基址字段全 0，限长与标志按段宽度给出
    uint64_t PackFlatSegment(uint8_t access, uint32_t limit, uint8_t flags) {
        const uint8_t limitHigh = static_cast<uint8_t>((limit >> 16) & 0x0F);   // 限长 19:16 与标志同处字节 6
        return (static_cast<uint64_t>(limit) & 0xFFFF)
            | (static_cast<uint64_t>(access) << kSegmentAccessShift)
            | (static_cast<uint64_t>(static_cast<uint8_t>(flags | limitHigh)) << kSegmentFlagsShift);
    }

    // 打包系统段描述符的低 8 字节：基址 32 位、字节粒度限长，标志字节只放限长高位
    uint64_t PackSystemSegment(uint8_t access, uint32_t base, uint32_t limit) {
        return (static_cast<uint64_t>(limit) & 0xFFFF)
            | (static_cast<uint64_t>(base & 0xFFFF) << kSegmentBaseLowShift)
            | (static_cast<uint64_t>((base >> 16) & 0xFF) << kSegmentBaseMidShift)
            | (static_cast<uint64_t>(access) << kSegmentAccessShift)
            | (static_cast<uint64_t>((limit >> 16) & 0x0F) << kSegmentFlagsShift)
            | (static_cast<uint64_t>(base >> 24) << kSegmentBaseHighShift);
    }

}

namespace Platform {
    void Gdt::Clear() {
        for (uint32_t i = 0; i < kMaxEntries; ++i) m_entries[i] = 0;
    }

    bool Gdt::SetCode(uint32_t index, uint8_t dpl, CodeWidth width) {
        if (index >= kMaxEntries || dpl > kMaxDpl) return false;
        switch (width) {
            case CodeWidth::Bits16: m_entries[index] = PackFlatSegment(AccessByte(kAccessCode, dpl), kLimit16, 0); break;
            case CodeWidth::Bits32: m_entries[index] = PackFlatSegment(AccessByte(kAccessCode, dpl), kFlatLimit, kFlagGranularity | kFlagDefault32); break;
            case CodeWidth::Bits64: m_entries[index] = PackFlatSegment(AccessByte(kAccessCode, dpl), kFlatLimit, kFlagGranularity | kFlagLong); break;
        }
        return true;
    }

    bool Gdt::SetData(uint32_t index, uint8_t dpl, DataWidth width) {
        if (index >= kMaxEntries || dpl > kMaxDpl) return false;
        switch (width) {
            case DataWidth::Bits16: m_entries[index] = PackFlatSegment(AccessByte(kAccessData, dpl), kLimit16, 0); break;
            case DataWidth::Bits32: m_entries[index] = PackFlatSegment(AccessByte(kAccessData, dpl), kFlatLimit, kFlagGranularity | kFlagDefault32); break;
        }
        return true;
    }

    bool Gdt::SetTss(uint32_t index, uintptr_t base, uint32_t limit) {
        if (index >= kMaxEntries || limit > kFlatLimit) return false;
#if defined(__x86_64__)
        if (index + 1 >= kMaxEntries) return false;   // 长模式系统描述符占两个相邻槽位
        m_entries[index] = PackSystemSegment(kAccessTss, static_cast<uint32_t>(base), limit);
        m_entries[index + 1] = static_cast<uint64_t>(base) >> 32;   // 上位槽位只放基址 63:32
#else
        m_entries[index] = PackSystemSegment(kAccessTss, static_cast<uint32_t>(base), limit);
#endif
        return true;
    }

    void Gdt::Load(uint16_t codeSelector, uint16_t dataSelector) {
        const TablePointer pointer{ static_cast<uint16_t>(Limit()), reinterpret_cast<uintptr_t>(Base()) };
        _Gdt_LoadPointer(&pointer);
        _Gdt_ReloadSegments(dataSelector);
        _Gdt_ReloadCodeSegment(codeSelector);
    }

    void Idt::Clear() {
        for (uint32_t i = 0; i < kVectorCount * kGateWords; ++i) m_entries[i] = 0;
    }

    bool Idt::SetGate(uint32_t vector, uintptr_t handler, uint16_t selector, GateType type, uint8_t dpl, uint8_t ist) {
        if (vector >= kVectorCount || selector == 0 || dpl > kMaxDpl || ist > kMaxIst) return false;
#if !defined(__x86_64__)
        if (ist != 0) return false;   // 32 位保护模式的门没有 IST 字段，字节 4 保留为 0
#endif
        const uint8_t typeAttribute = static_cast<uint8_t>(kGatePresent | (dpl << kGateDplShift) | (type == GateType::Trap ? kGateTrap : kGateInterrupt));
        const uint64_t offset = static_cast<uint64_t>(handler);
        uint64_t* entry = &m_entries[vector * kGateWords];
        entry[0] = (offset & 0xFFFF)
            | (static_cast<uint64_t>(selector) << 16)
            | (static_cast<uint64_t>(ist) << 32)
            | (static_cast<uint64_t>(typeAttribute) << 40)
            | (((offset >> 16) & 0xFFFF) << 48);
#if defined(__x86_64__)
        entry[1] = offset >> 32;   // 上位 8 字节只放偏移 63:32，其余保留为 0
#endif
        return true;
    }

    void Idt::Load() const {
        const TablePointer pointer{ static_cast<uint16_t>(Limit()), reinterpret_cast<uintptr_t>(Base()) };
        _Idt_LoadPointer(&pointer);
    }

    void LoadTaskRegister(uint16_t selector) {
        _Gdt_LoadTaskRegister(selector);
    }
}
