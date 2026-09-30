/* Core.cpp
    Core 引导阶段的主流程：装配自己的控制台并打印横幅，核对 Stub 交权块与自身镜像事实，
    初始化 CPU 快照，建立运行期段表与中断表，播报环境事实，最后停在明确的阶段边界上

    按 Stub 的分工约定，本文件承担全部启动流程与全部诊断输出：步骤顺序、进度行、
    失败原因与停机都集中在这里；Console.cpp 只做控制台装配，不打印也不停机
*/

#include <stdint.h>

#include <BootInfo.hpp>
#include <CoreHandoff.hpp>
#include <ImageHeader.hpp>
#include <Platform/Cpu.hpp>
#include <Platform/Descriptor.hpp>
#include <Print.hpp>
#include <PrintTarget.hpp>

// 控制台装配入口在 Console.cpp
extern "C" void _Core_Console_Install();
// 异常入口桩地址表，由 Core.asm 提供，索引即向量号
extern "C" const uintptr_t _Core_Exception_Stubs[32];
// 链接脚本给出的镜像与未初始化区边界，用于与镜像头字段核对
extern "C" const uint8_t __image_start[];
extern "C" const uint8_t __image_end[];
extern "C" const uint8_t __bss_end[];

namespace {
    // 选择子：与 Stub 交权时的段表同号，Core 的运行期段表沿用这套编号
    constexpr uint16_t kSelectorCode32 = 0x08;   // 32 位代码段
    constexpr uint16_t kSelectorData32 = 0x10;   // 32 位数据段
    // CPU 异常向量数量：0 至 31，不含外部中断
    constexpr uint32_t kExceptionVectors = 32;

    // CPU 事实快照
    Platform::Cpu s_cpu;
    // Core 自己的运行期段表与中断表：成员初值都是常量，不依赖启动运行库
    Platform::Gdt s_gdt;
    Platform::Idt s_idt;

    // 停机并停留，供自动化读取诊断；失败路径与阶段边界共用
    [[noreturn]] void Stop() {
        for (;;) asm volatile("CLI; HLT");
    }

    // 打印 FATAL 原因后停机
    [[noreturn]] void Fail(const char* reason) {
        Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Fatal, reason);
        Stop();
    }

    // 该向量是否由处理器压入错误码；其余向量的栈帧里没有错误码
    bool HasErrorCode(uint32_t vector) {
        return vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21 || vector == 29 || vector == 30;
    }

    // 建立运行期段表与中断表并装载；调用前中断须已关闭
    // 异常门全部指向 Core 的停机诊断入口：出错时屏上留下向量与出错位置，而不是三重故障
    void InstallDescriptorTables() {
        s_gdt.Clear();
        if (!s_gdt.SetCode(1, 0, Platform::CodeWidth::Bits32) || !s_gdt.SetData(2, 0, Platform::DataWidth::Bits32)) Fail("Install gdt fail");
        s_gdt.Load(kSelectorCode32, kSelectorData32);

        s_idt.Clear();
        for (uint32_t vector = 0; vector < kExceptionVectors; ++vector) {
            if (!s_idt.SetGate(vector, _Core_Exception_Stubs[vector], kSelectorCode32, Platform::GateType::Interrupt, 0, 0)) Fail("Install idt fail");
        }
        s_idt.Load();
    }

    // 内存图里可用区间的总字节数，按 type==1 累加；打印时右移成 MiB，不做 64 位除法
    uint64_t UsableMemoryBytes(const Baleen::CoreHandoff& handoff) {
        if (handoff.memoryMap == nullptr) return 0;
        uint64_t total = 0;
        for (uint32_t i = 0; i < handoff.memoryMapCount; ++i) {
            const Boot::MemoryMapEntry& entry = handoff.memoryMap[i];
            if (entry.type == 1) total += entry.length;
        }
        return total;
    }
}

// CPU 异常入口的公共目标：打印向量与出错位置后停机，不返回；帧由 Core.asm 的入口桩按处理器压栈顺序排列
extern "C" void _Core_Exception_Handler(const uint32_t* frame) {
    const uint32_t vector = frame[0];
    const uint32_t* tail = frame + 1;
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Fatal);
    Print::Write("Exception ");
    Print::WriteHex(vector);
    if (HasErrorCode(vector)) { Print::Write(" Err="); Print::WriteHex(tail[0]); ++tail; }   // 有错误码的向量由处理器先压错误码
    Print::Write(" Eip=");
    Print::WriteHex(tail[0]);
    Print::Write("\r\n");
    for (;;) asm volatile("CLI; HLT");
}

// BaleenCore 主流程：由 Core.asm 的入口以 ESI 里的交权块指针调用，不返回
extern "C" void _Core_Main(const Baleen::CoreHandoff* handoff) {
    // 装配 Core 自己的控制台：横幅与后面每一步的结论都从这里出去
    _Core_Console_Install();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Core, "LikesProgramOS BaleenCore");

    // 交权块：三项固定字段先行，通过之后其余字段才是可解释的事实
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Checking the handoff block");
    if (handoff == nullptr) Fail("The handoff block pointer is null");
    if (handoff->magic != Baleen::CoreHandoffLayout::kMagic) Fail("The handoff block magic doesn't match");
    if (handoff->version != Baleen::CoreHandoffLayout::kVersion) Fail("The handoff block version isn't supported");
    if (handoff->bytes != Baleen::CoreHandoffLayout::kBytes) Fail("The handoff block length isn't 64 bytes");
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The handoff block is valid");

    // 自身镜像事实：摘要已由 Stub 在交权前比对，这里不重复取；核对头字段与链接布局、装载地址
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Checking the core image against the handoff block");
    Baleen::ImageFacts facts;
    if (const char* reason = Baleen::CheckImageHeader(__image_start, Baleen::kCoreImageMagic, facts)) Fail(reason);
    if (facts.imageBytes != static_cast<uint32_t>(__image_end - __image_start)) Fail("The image header size doesn't match the linked image");
    if (facts.memoryBytes != static_cast<uint32_t>(__bss_end - __image_start)) Fail("The image header memory size doesn't match the linked image");
    const uint32_t imageLoad = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__image_start));
    if (handoff->coreLoad != imageLoad) Fail("Where the core runs doesn't match the handoff block load address");
    if (handoff->coreBytes != facts.imageBytes) Fail("The handoff block image size doesn't match the image header");
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The core image matches the handoff block");

    // CPU 事实快照：模式不符时后续步骤没有意义
    s_cpu.Install();
    if (s_cpu.Mode() != Platform::CpuMode::Protected32) Fail("The CPU mode doesn't match, it needs 32-bit protected mode");
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Initializing the CPU");

    // 运行期段表与中断表：此后 CPU 异常停在诊断行上，而不是无输出的三重故障
    InstallDescriptorTables();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Installing descriptor tables");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Cpu=");
    Print::Write(s_cpu.HasCpuid() ? s_cpu.Vendor() : "no-cpuid");
    Print::Write(" Mode=");
    Print::WriteHex(static_cast<uint32_t>(s_cpu.Mode()));
    Print::Write(" Build=");
    Print::WriteHex(facts.buildId);
    Print::Write("\r\n");

    // 环境事实：介质、Core 装载与内存图都来自交权块，后续阶段按这些事实定位系统卷
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Reporting the machine facts");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Media=");
    Print::WriteHex(handoff->media);
    Print::Write(" Sect=");
    Print::WriteHex(handoff->sectorBytes);
    Print::Write(" Load=");
    Print::WriteHex(handoff->coreLoad);
    Print::Write(" Bytes=");
    Print::WriteHex(handoff->coreBytes);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Mmap=");
    Print::WriteHex(handoff->memoryMapCount);
    Print::Write(" Trunc=");
    Print::WriteHex(handoff->memoryMapTruncated);
    Print::Write(" UsableMiB=");
    Print::WriteHex(static_cast<uint32_t>(UsableMemoryBytes(*handoff) >> 20));
    Print::Write("\r\n");

    // 阶段边界：布局描述、卷访问与内核装载尚未实现，停在可观测的状态上而不是跑进未定义代码
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Warn, "The core stage ends here: the layout, volume and kernel stages aren't implemented yet");
    Stop();
}
