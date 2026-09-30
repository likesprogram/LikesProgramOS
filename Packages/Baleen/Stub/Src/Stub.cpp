/* Stub.cpp
    Stub：低内存固定桩，自检自身完整性头与摘要、开 A20、建立运行期段表与中断表、校验 BaleenCore 的
    描述符与镜像头、把 BaleenCore 读到高位、校验其摘要并跳转；自身须冻结且整体留在低 64KiB，
    所有 BIOS 服务经这里暴露给 BootCore
*/

#include <new>
#include <stdint.h>

#include <Bios.hpp>
#include <BootInfo.hpp>
#include <LoadCore.hpp>
#include <Print.hpp>
#include <Platform/Cpu.hpp>
#include <Platform/Descriptor.hpp>
#include <Print/VgaTextTarget.hpp>
#include <PrintTarget.hpp>
#include <SelfCheck.hpp>

// IPL 传入，Stub.asm 保存
extern "C" uint8_t _Boot_Drive;
extern "C" uint8_t _Boot_Media;
extern "C" uint16_t _Boot_Sector_Bytes;
// 异常入口桩地址表，由 Stub.asm 提供，索引即向量号
extern "C" const uintptr_t _Stub_Exception_Stubs[32];

namespace {
    // 运行期状态对象一律落在未初始化区：镜像自检要求装入的字节保持原样，
    // 带虚表或非零初值的对象若作为普通全局量会进落盘数据区，装配控制台就改写了镜像
    // 这里只留存储，对象由 InitConsole 就地构造
    alignas(Baleen::Devices::Vga) uint8_t s_vgaStorage[sizeof(Baleen::Devices::Vga)];
    Baleen::Devices::Vga& s_vga = *reinterpret_cast<Baleen::Devices::Vga*>(s_vgaStorage);
    alignas(Baleen::PrintTargets::BiosConsole) uint8_t s_consoleStorage[sizeof(Baleen::PrintTargets::BiosConsole)];
    Baleen::PrintTargets::BiosConsole& s_console = *reinterpret_cast<Baleen::PrintTargets::BiosConsole*>(s_consoleStorage);
    // CPU 设备
    Platform::Cpu s_cpu;
    // 运行期段描述符表：入口汇编只带进入保护模式所需的最小段表，其余描述符在这里建立
    Platform::Gdt s_gdt;
    // 运行期中断描述符表：CPU 异常统一进停机诊断入口，运行期不开放中断
    Platform::Idt s_idt;

    // 选择子：与入口汇编的引导表同号；16 位段留给将来回实模式的 BIOS 路径
    constexpr uint16_t kSelectorCode32 = 0x08;   // 32 位代码段
    constexpr uint16_t kSelectorData32 = 0x10;   // 32 位数据段
    [[maybe_unused]] constexpr uint16_t kSelectorCode16 = 0x18;   // 16 位代码段，当前只服务汇编侧语义
    [[maybe_unused]] constexpr uint16_t kSelectorData16 = 0x20;   // 16 位数据段，当前只服务汇编侧语义
    // CPU 异常向量数量：0 至 31，不含外部中断
    constexpr uint32_t kExceptionVectors = 32;

    // 停机并停留，供自动化读取诊断；本阶段的失败路径都走这里，打印集中在启动流程里
    [[noreturn]] void Fail(const char* reason) {
        Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Fatal, reason);
        for (;;) asm volatile("CLI; HLT");
    }

    // 就地构造控制台对象：写入的只是未初始化区的存储，不改动镜像字节
    // VGA 设备与组合控制台都带虚表或非零初值，不能依赖静态初始化落盘
    void InitConsole() {
        new (s_vgaStorage) Baleen::Devices::Vga();
        new (s_consoleStorage) Baleen::PrintTargets::BiosConsole();
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

    // 扇区大小取自完成 IPL 装载的同一会话，不重新探测后猜测回落到另一种单位
    uint32_t HandoffSectorSize(uint32_t media) {
        const uint32_t sector = _Boot_Sector_Bytes;
        if (media == static_cast<uint32_t>(Boot::Media::Cdrom)) return sector == 2048 ? sector : 0;
        if (media == static_cast<uint32_t>(Boot::Media::Hdd) && (sector == 512 || sector == 4096)) return sector;
        return 0;
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
    InitConsole();
    s_console.Install(s_vga);
    // 接管控制台后清一次屏：VGA 上抹掉固件与上层阶段留下的输出，串口等流式通道无感
    Print::ClearScreen();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Stub, "LikesProgramOS BaleenStub");

    // 自身完整性自检：先核对头字段与链接布局，再比对整幅镜像的摘要
    // 摘要要遍历整个文件，先播报再进行，卡在这一步时屏上仍有线索
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Verifying the stub image");
    Baleen::Stub::SelfInfo self;
    if (const char* reason = Baleen::Stub::CheckSelf(self)) Fail(reason);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The stub image is intact");
    // 续行：镜像尺寸与构建标识，BuildId 只显示前 4 字节
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Image=");
    Print::WriteHex(self.imageBytes);
    Print::Write(" Mem=");
    Print::WriteHex(self.memoryBytes);
    Print::Write(" Build=");
    Print::WriteHex(self.buildId);
    Print::Write("\r\n");

    // 初始化 CPU
    s_cpu.Install();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Initializing the CPU");

    // 段与门都按 32 位保护模式建立，进入这个模式由入口汇编负责；模式不符时后续步骤没有意义
    if (s_cpu.Mode() != Platform::CpuMode::Protected32) Fail("The CPU mode doesn't match, it needs 32-bit protected mode");
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "The CPU mode is 32-bit protected mode");

    // 打开 A20。A20 测试要按平坦段访问 1MiB 以上的物理地址，此处的数据段已由入口汇编置为平坦段
    if (!s_cpu.EnableA20()) Fail("Enable A20");
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Enabling A20");

    // 运行期段表与中断表：此后 CPU 异常停在诊断行上，而不是无输出的三重故障
    InstallDescriptorTables();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Installing descriptor tables");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Cpu=");
    Print::Write(s_cpu.HasCpuid() ? s_cpu.Vendor() : "no-cpuid");
    Print::Write(" Mode=");
    Print::WriteHex(static_cast<uint32_t>(s_cpu.Mode()));
    Print::Write("\r\n");

    // 核对 IPL 交来的设备逻辑扇区大小
    const uint32_t media = _Boot_Media;
    const uint32_t sect = HandoffSectorSize(media);
    if (sect == 0) Fail("The IPL sector size isn't supported for this media");
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Checking the IPL sector size");
    // 进度行：实体机上只有屏能看到输出，此行之后若再无下文，说明卡在描述符读取、E820 或 BaleenCore 装载这些要走 BIOS 的步骤上，而不是没跑起来
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Media=");
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
    // 等级随自检结果走：两项都不阻断启动，任一项失败按非致命错误、内存图截断或签名不符按警告
    Baleen::PrintTargets::Tag probe_tag = Baleen::PrintTargets::Tag::Ok;
    if (read_ok == 0 || mmap_count == 0) probe_tag = Baleen::PrintTargets::Tag::Error;
    else if (truncated != 0 || (sect == 512 && !signature_ok)) probe_tag = Baleen::PrintTargets::Tag::Warn;
    Baleen::PrintTargets::WriteLine(probe_tag, "Probing BIOS services");
    // 读数另起一行
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Mmap=");
    Print::WriteHex(mmap_count);
    Print::Write(" Trunc=");
    Print::WriteHex(truncated);
    Print::Write(" Read0=");
    Print::WriteHex(read_ok);
    // 0x55AA 只是 512 字节介质上引导扇区的签名，光盘上没有这一项，不参与判定
    if (sect == 512) Print::Write(signature_ok ? " Sig=Ok" : " Sig=Bad");
    Print::Write("\r\n");

    // 从存储介质装载 BaleenCore：先核对描述符与镜像头，再读入高位，最后比对摘要，成功不返回
    // 校验、读盘、交权的顺序与每一步的诊断行都摆在这里：日志与流程集中在一处，出问题只看这一段
    Baleen::Stub::CoreLoadPlan plan;
    if (const char* reason = Baleen::Stub::PrepareCore(_Boot_Drive, media, sect, plan)) Fail(reason);
    // 装载计划已成：长度、入口与静态内存跨度都取自镜像头，且已与描述符核对
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Preparing the baleen core load");
    // 输出计划参数，屏上此行之后再无下文，说明卡在 Core 读盘上
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("At=");
    Print::WriteHex(plan.fileOffset);
    Print::Write(" Bytes=");
    Print::WriteHex(plan.imageBytes);
    Print::Write(" Load=");
    Print::WriteHex(plan.loadAddress);
    Print::Write("\r\n"); // 主动换行，防止溢出影响观察
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Entry=");
    Print::WriteHex(plan.loadAddress + plan.entryOffset);
    Print::Write(" Bounce=");
    Print::WriteHex(plan.bounceAddress);
    Print::Write("\r\n");
    if (const char* reason = Baleen::Stub::ReadCore(plan, sect)) Fail(reason);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Loading baleen core");

    // 摘要遍历整个文件，先播报再进行，卡在这一步时屏上仍有线索
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Verifying the baleen core image");
    if (const char* reason = Baleen::Stub::CheckCore(plan)) Fail(reason);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The baleen core image is intact");
    // 续行：头声明的静态内存跨度与构建标识，BuildId 只显示前 4 字节
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Mem=");
    Print::WriteHex(plan.memoryBytes);
    Print::Write(" Build=");
    Print::WriteHex(plan.buildId);
    Print::Write("\r\n");

    // 交权与跳转：跳转前只能播报，进入 Core 由 Core 自己的输出证明
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Entering baleen core");
    Baleen::Stub::EnterCore(plan);
}
