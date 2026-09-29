/* Stub.cpp
    Stub：低内存固定桩，开 A20、建立运行期段表与中断表、校验 BaleenCore 落点、把 BaleenCore 读到高位并跳转；自身须冻结且整体留在低 64KiB，所有 BIOS 服务经这里暴露给 BootCore
*/

#include <stdint.h>

#include <Bios.hpp>
#include <BootInfo.hpp>
#include <LoadCore.hpp>
#include <Print.hpp>
#include <Platform/Cpu.hpp>
#include <Platform/Descriptor.hpp>
#include <Print/VgaTextTarget.hpp>
#include <PrintTarget.hpp>

// IPL 传入，Stub.asm 保存
extern "C" uint8_t _Boot_Drive;
extern "C" uint8_t _Boot_Media;
// 异常入口桩地址表，由 Stub.asm 提供，索引即向量号
extern "C" const uintptr_t _Stub_Exception_Stubs[32];

namespace {
    // 引导期控制台用的 VGA 文本设备；文本模式由 IPL 交权路径保证
    Baleen::Devices::Vga s_vga;
    // BIOS 路径引导期控制台：组合 VGA 文本、0xE9 与 COM1
    Baleen::PrintTargets::BiosConsole s_console;
    // CPU 设备
    Platform::Cpu s_cpu;
    // 运行期段描述符表：入口汇编只带进入保护模式所需的最小段表，其余描述符在这里建立
    Platform::Gdt s_gdt;
    // 运行期中断描述符表：CPU 异常统一进停机诊断入口，运行期不开放中断
    Platform::Idt s_idt;

    // 选择子：与入口汇编的引导表同号；16 位段留给将来回实模式的 BIOS 路径
    constexpr uint16_t kSelectorCode32 = 0x08;   // 32 位代码段
    constexpr uint16_t kSelectorData32 = 0x10;   // 32 位数据段
    constexpr uint16_t kSelectorCode16 = 0x18;   // 16 位代码段
    constexpr uint16_t kSelectorData16 = 0x20;   // 16 位数据段
    // CPU 异常向量数量：0 至 31，不含外部中断
    constexpr uint32_t kExceptionVectors = 32;

    // 停机并停留，供自动化读取诊断；本阶段的失败路径都走这里，打印集中在启动流程里
    [[noreturn]] void Fail(const char* reason) {
        Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Fatal, reason);
        for (;;) asm volatile("CLI; HLT");
    }

    // 该向量是否由处理器压入错误码；其余向量的栈帧里没有错误码
    bool HasErrorCode(uint32_t vector) {
        return vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21 || vector == 29 || vector == 30;
    }

    // 建立运行期段表与中断表并装载；调用前中断须已关闭，入口汇编的 CLI 之后不再开放
    // 段表内容集中在 C++ 侧，入口汇编只留进保护模式的最小表；异常门全部指向停机诊断入口
    void InstallDescriptorTables() {
        s_gdt.Clear();
        if (!s_gdt.SetCode(1, 0, Platform::CodeWidth::Bits32) || !s_gdt.SetData(2, 0, Platform::DataWidth::Bits32)
            || !s_gdt.SetCode(3, 0, Platform::CodeWidth::Bits16) || !s_gdt.SetData(4, 0, Platform::DataWidth::Bits16)) Fail("Install gdt fail");
        s_gdt.Load(kSelectorCode32, kSelectorData32);

        s_idt.Clear();
        for (uint32_t vector = 0; vector < kExceptionVectors; ++vector) {
            if (!s_idt.SetGate(vector, _Stub_Exception_Stubs[vector], kSelectorCode32, Platform::GateType::Interrupt, 0, 0)) Fail("Install idt fail");
        }
        s_idt.Load();
    }

    // 探测结果缓存：AH=48 是一条真机读命令，屏上诊断行再取一次没有意义
    uint32_t s_probed_sector = 0;

    // 探测设备扇区大小：光盘按 El Torito 固定 2048，其它取 AH=48，失败回落 512
    uint32_t ProbeSectorSize(uint32_t media) {
        if (media == static_cast<uint32_t>(Boot::Media::Cdrom)) return 2048;
        if (s_probed_sector != 0) return s_probed_sector;
        const uint32_t probed = _Bios_Sector_Size();
        s_probed_sector = (probed == 512 || probed == 2048 || probed == 4096) ? probed : 512;
        return s_probed_sector;
    }
}

// 交权块暴露给 Core 的控制台入口：转发到 Stub 装配的 Print 目标
// Core 在保护模式下调它，不必自己初始化 VGA、串口与调试口；它与其其他三个 BIOS 服务一起构成交权块的全部服务
extern "C" void _Stub_Write(const char* text) {
    Print::Write(text);
}

// CPU 异常入口的公共目标：打印向量与出错位置后停机，不返回；帧由 Stub.asm 的入口桩按处理器压栈顺序排列
extern "C" void _Stub_Exception_Handler(const uint32_t* frame) {
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

// BaleenStub 主流程：成功跳到 BaleenCore，不返回
extern "C" void _Stub_Main() {
    // 初始化控制台
    s_console.Install(s_vga);
    // 接管控制台后清一次屏：VGA 上抹掉固件与上层阶段留下的输出，串口等流式通道无感
    Print::ClearScreen();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Stub, "LikesProgramOS BaleenStub");

    // 初始化 CPU
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Initializing the CPU");
    s_cpu.Install();
    // 段与门都按 32 位保护模式建立，进入这个模式由入口汇编负责；模式不符时后续步骤没有意义
    if (s_cpu.Mode() != Platform::CpuMode::Protected32) Fail("The CPU mode doesn't match, it needs 32-bit protected mode");

    // 打开 A20。A20 测试要按平坦段访问 1MiB 以上的物理地址，此处的数据段已由入口汇编置为平坦段
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Enabling A20");
    if (!s_cpu.EnableA20()) Fail("Enable A20");

    // 运行期段表与中断表：此后 CPU 异常停在诊断行上，而不是无输出的三重故障
    InstallDescriptorTables();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Install descriptor tables");

    const uint32_t media = _Boot_Media;
    const uint32_t sect = ProbeSectorSize(media);
    // 进度行：实体机上只有屏能看到输出，此行之后若再无下文，说明卡在描述符读取、E820 或 BaleenCore 装载这些要走 BIOS 的步骤上，而不是没跑起来
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Debug);
    Print::Write("Cpu=");
    Print::Write(s_cpu.HasCpuid() ? s_cpu.Vendor() : "no-cpuid");
    Print::Write(" Mode=");
    Print::WriteHex(static_cast<uint32_t>(s_cpu.Mode()));
    Print::Write(" Media=");
    Print::WriteHex(media);
    Print::Write(" Sect=");
    Print::WriteHex(sect);
    Print::Write("\r\n");

    // BIOS 服务自检：内存图与读盘都要经实模式弹跳走固件，这里各做一次并报告结果
    // 内存图读进静态缓冲而不是栈上局部量：Core 装载区的判定与交权块都要用它
    uint32_t truncated = 0;
    const uint32_t mmap_count = Baleen::Stub::QueryMemoryMap(truncated);

    // 读 0 号扇区并核对引导签名：读数正确说明弹跳、DAP 与分批口径都按预期工作
    // 缓冲区按最大扇区取，块长度用探测值：光盘的扇区是 2048，按 512 备缓冲会冲掉栈
    uint8_t probe_sector[4096] __attribute__((aligned(16)));
    const uint32_t read_ok = _Bios_Read_Sectors(0, 1, probe_sector, sect);
    const bool signature_ok = read_ok != 0 && probe_sector[510] == 0x55 && probe_sector[511] == 0xAA;
    // 本行等级随自检结果走：两项都不阻断启动，读盘失败按非致命错误标出，签名不符按警告标出
    Baleen::PrintTargets::Tag probe_tag = Baleen::PrintTargets::Tag::Debug;
    if (read_ok == 0) probe_tag = Baleen::PrintTargets::Tag::Error;
    else if (sect == 512 && !signature_ok) probe_tag = Baleen::PrintTargets::Tag::Warn;
    Baleen::PrintTargets::WriteTag(probe_tag);
    Print::Write("Mmap=");
    Print::WriteHex(mmap_count);
    Print::Write(" Trunc=");
    Print::WriteHex(truncated);
    Print::Write(" Read0=");
    Print::WriteHex(read_ok);
    // 0x55AA 只是 512 字节介质上引导扇区的签名，光盘上没有这一项，不参与判定
    if (sect == 512) Print::Write(signature_ok ? " Sig=Ok" : " Sig=Bad");
    Print::Write("\r\n");

    // 从存储介质装载 BaleenCore 并按交权块交给它，成功不返回
    // 校验、读盘、交权的顺序与每一步的诊断行都摆在这里：日志与流程集中在一处，出问题只看这一段
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Loading BaleenCore");
    Baleen::Stub::CoreLoadPlan plan;
    if (const char* reason = Baleen::Stub::PrepareCore(_Boot_Drive, media, sect, plan)) Fail(reason);
    // 进度行：实体机上只有屏能看到输出，此行之后若再无下文，说明卡在 Core 读盘上
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Debug);
    Print::Write("At=");
    Print::WriteHex(plan.fileOffset);
    Print::Write(" Bytes=");
    Print::WriteHex(plan.imageBytes);
    Print::Write(" Load=");
    Print::WriteHex(plan.loadAddress);
    Print::Write(" Bounce=");
    Print::WriteHex(plan.bounceAddress);
    Print::Write("\r\n");
    if (const char* reason = Baleen::Stub::ReadCore(plan, sect)) Fail(reason);

    // 交权与跳转：跳转前只能播报，进入 Core 由 Core 自己的输出证明
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Entering BaleenCore");
    Baleen::Stub::EnterCore();
}
