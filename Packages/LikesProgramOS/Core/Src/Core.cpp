/* Core.cpp
    内核侧 Core 的主流程：装配自己的早期控制台，核对内核镜像数据、BootInfo 与启动来源，
    让固件启动服务失效，完成平台初始化与内存层，最后停在明确的阶段边界上

    按与 BaleenCore 相同的分工：本文件承担全部启动流程与全部诊断输出——步骤顺序、
    进度行、失败原因与停机都集中在这里；Console、Tables、Paging 与 Memory 只提供机制，
    不打印也不停机，失败只回报原因文本
*/

#include <stdint.h>

#include <BootInfo.hpp>
#include <KernelImage.hpp>

#include <Cpu.hpp>
#include <Memory.hpp>
#include <Paging.hpp>
#include <Tables.hpp>
#include <Tags.hpp>

#include <Print.hpp>

// 控制台装配入口在 Console.cpp
extern "C" void _Kernel_Console_Install(const Boot::Info* info);
// Core.asm 提供的入口；异常桩地址表由 Tables.cpp 引用
extern "C" void _Kernel_Start();
// 链接脚本给出的镜像与未初始化区边界，用于与内核镜像数据核对
extern "C" const uint8_t __image_start[];
extern "C" const uint8_t __image_end[];
extern "C" const uint8_t __bss_end[];

namespace {
    // CPU 事实快照与内存层实例：静态存储，不依赖启动运行库
    Kernel::Cpu s_cpu;
    Kernel::PageAllocator s_pages;

    // 停机并停留，供自动化读取诊断；失败路径与阶段边界共用
    [[noreturn]] void Stop() {
        for (;;) asm volatile("CLI; HLT");
    }

    // 打印 FATAL 原因后停机
    [[noreturn]] void Fail(const char* reason) {
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Fatal, reason);
        Stop();
    }

    // 校验后的内核事实写进一行续行：字节数、装载地址、入口与 BuildId
    void ReportKernelFacts(const Kernel::ImageFacts& facts) {
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Bytes=");
        Print::WriteHex(facts.imageBytes);
        Print::Write(" Load=");
        Print::WriteHex(facts.loadAddress);
        Print::Write(" Entry=");
        Print::WriteHex(facts.entryOffset);
        Print::Write(" Build=");
        Print::WriteHex(facts.buildId);
        Print::Write("\r\n");
    }

    // 设备线索最长读入的字节数：UEFI 设备路径可能很长，日志只取开头若干字节用于人工定位
    constexpr uint32_t kCluePrintBytes = 16;
    // 实际会读的设备线索字节数；0 表示没有线索
    uint32_t ClueBytes(const Boot::Info* info) { return info->devicePathBytes < kCluePrintBytes ? info->devicePathBytes : kCluePrintBytes; }

    // 物理区间是否整体落在内核直映射窗口内：只有这样的地址才能按 PhysToVirt 解引用
    // 引导器给的地址出错时必然越界，硬读会缺页并把内核停在异常处理里；
    // 这类字段按“未提供”处理，核对处另行报 WARN，不拿一次缺页换一条诊断
    bool IsMappedRange(uint64_t physical, uint64_t bytes) {
        if (physical == 0 || physical >= Kernel::kMapLimitBytes) return false;
        return Kernel::kMapLimitBytes - physical >= bytes;
    }

    // 打印一个字节的两位小写十六进制：卷标识与设备线索按字节顺序原样报出
    void WriteByteHex(uint8_t value) {
        constexpr char kDigits[] = "0123456789abcdef";
        Print::PutChar(kDigits[value >> 4]);
        Print::PutChar(kDigits[value & 0xF]);
    }

    // 报一条启动来源不一致的 WARN 前缀；具体字段值由调用处紧跟其后打印
    void WarnInconsistent(const char* field) {
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Warn);
        Print::Write("The BootInfo ");
        Print::Write(field);
        Print::Write(" is inconsistent: ");
    }

    // 核对启动来源字段之间的一致性：任何一条不成立只报 WARN，该项按“未提供”处理，不判死
    // 第三方引导器可能给不全同一份 BootInfo，因此每条都以字段自身非 0 为前提；
    // 全部一致返回 true，出现不一致返回 false，调用处据此决定是否报 OK
    bool CheckBootSource(const Boot::Info* info) {
        bool consistent = true;
        // 内核路径：给了长度就必须给出可读的地址，且长度不超上限
        if (info->kernelPathBytes != 0 && (info->kernelPathBytes > Boot::kKernelPathMaxBytes || !IsMappedRange(info->kernelPath, info->kernelPathBytes))) {
            consistent = false;
            WarnInconsistent("kernel path");
            Print::Write("Bytes=");
            Print::WriteHex(info->kernelPathBytes);
            Print::Write(" Path=");
            Print::WriteHex64(info->kernelPath);
            Print::Write("\r\n");
        }
        // 设备线索：给了长度就必须给出可读的地址，且长度不超契约上限；只要求要读的前若干字节可读
        if (info->devicePathBytes != 0 && (info->devicePathBytes > Boot::kDeviceClueMaxBytes || info->devicePath == 0 || !IsMappedRange(info->devicePath, ClueBytes(info)))) {
            consistent = false;
            WarnInconsistent("device path");
            Print::Write("Bytes=");
            Print::WriteHex(info->devicePathBytes);
            Print::Write(" Path=");
            Print::WriteHex64(info->devicePath);
            Print::Write("\r\n");
        }
        // 系统卷：位置与长度要么都给，要么都不给；只给一个是不一致（0 表示未提供）
        if ((info->volumeBytes != 0) != (info->volumeOffset != 0)) {
            consistent = false;
            WarnInconsistent("volume");
            Print::Write("Layout=");
            Print::WriteHex64(info->layoutOffset);
            Print::Write(" Offset=");
            Print::WriteHex64(info->volumeOffset);
            Print::Write(" Bytes=");
            Print::WriteHex64(info->volumeBytes);
            Print::Write("\r\n");
        }
        // 引导状态区域：位置与长度同样要么都给，要么都不给（偏移 0 表示该介质没有状态区域）
        if ((info->stateOffset != 0) != (info->stateBytes != 0)) {
            consistent = false;
            WarnInconsistent("state area");
            Print::Write("Offset=");
            Print::WriteHex64(info->stateOffset);
            Print::Write(" Bytes=");
            Print::WriteHex(info->stateBytes);
            Print::Write("\r\n");
        }
        return consistent;
    }

    // 播报内核路径：按 kernelPathBytes 个字节原样打印，不越读也不依赖结尾空字符
    // 长度超上限或地址不可读时不读，用 "-" 占位：不一致已由 CheckBootSource 报过
    void ReportKernelPath(const Boot::Info* info) {
        if (info->kernelPathBytes == 0 || info->kernelPathBytes > Boot::kKernelPathMaxBytes || !IsMappedRange(info->kernelPath, info->kernelPathBytes)) { Print::Write("-"); return; }
        const char* path = static_cast<const char*>(Kernel::PhysToVirt(info->kernelPath));
        for (uint32_t i = 0; i < info->kernelPathBytes; ++i) Print::PutChar(path[i]);
    }

    // 播报启动设备线索：按 kCluePrintBytes 截断后逐字节打印，地址不可读时用 "-" 占位
    // 解释方式看 deviceKind：UEFI 设备路径是字节串，BIOS 驱动器号是一个字节
    void ReportDeviceClue(const Boot::Info* info) {
        const uint32_t bytes = ClueBytes(info);
        if (bytes == 0 || !IsMappedRange(info->devicePath, bytes)) { Print::Write("-"); return; }
        const uint8_t* clue = static_cast<const uint8_t*>(Kernel::PhysToVirt(info->devicePath));
        for (uint32_t i = 0; i < bytes; ++i) WriteByteHex(clue[i]);
    }

    // 播报启动来源：介质与卷的位置事实、引导器占用区间、卷标识与内核路径
    // 未提供的字段按 0 原样报出，字符串与线索缺失或不可读时用 "-" 占位
    void ReportBootSource(const Boot::Info* info) {
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Boot=Kind=");
        Print::WriteHex(info->deviceKind);
        Print::Write(" Layout=");
        Print::WriteHex64(info->layoutOffset);
        Print::Write(" Volume=");
        Print::WriteHex64(info->volumeOffset);
        Print::Write("/");
        Print::WriteHex64(info->volumeBytes);
        Print::Write(" State=");
        Print::WriteHex64(info->stateOffset);
        Print::Write("/");
        Print::WriteHex64(info->stateBytes);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Medium=");
        Print::WriteHex64(info->mediumBytes);
        Print::Write(" Loader=");
        Print::WriteHex64(info->loaderBegin);
        Print::Write("/");
        Print::WriteHex64(info->loaderBytes);
        Print::Write(" Uuid=");
        for (uint32_t i = 0; i < Boot::InfoLayout::kVolumeUuidBytes; ++i) WriteByteHex(info->volumeUuid[i]);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("ClueBytes=");
        Print::WriteHex(info->devicePathBytes);
        Print::Write(" Device=");
        ReportDeviceClue(info);
        Print::Write(" Path=");
        ReportKernelPath(info);
        Print::Write("\r\n");
    }
}

// 内核异常入口的公共目标：打印向量、错误码与出错位置后停机，不返回
// 帧由 Core.asm 的入口桩整理，自低到高固定为：向量号、错误码、返回偏移、CS、RFLAGS
extern "C" void _Kernel_Exception_Handler(const uint64_t* frame) {
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Fatal);
    Print::Write("Exception ");
    Print::WriteHex(static_cast<uint32_t>(frame[0]));
    Print::Write(" Err=");
    Print::WriteHex(static_cast<uint32_t>(frame[1]));
    Print::Write(" Rip=");
    Print::WriteHex64(frame[2]);
    Print::Write(" Cs=");
    Print::WriteHex(static_cast<uint32_t>(frame[3]));
    Print::Write(" Rflags=");
    Print::WriteHex64(frame[4]);
    Print::Write("\r\n");
    for (;;) asm volatile("CLI; HLT");
}

// 内核主流程：由 Core.asm 的入口以 RDI 里的 BootInfo 指针调用，不返回
extern "C" void _Kernel_Main(const Boot::Info* info) {
    // 早期控制台：按 BootInfo 声明的通道装配，后面的每一步结论都从这里出去
    _Kernel_Console_Install(info);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Kernel, "LikesProgramOS kernel");

    // 内核镜像数据：先与本镜像的链接布局交叉核对，打包或装载错误在这里就停下
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Checking the kernel image data");
    const uint32_t linkedBytes = static_cast<uint32_t>(__image_end - __image_start);
    const uint32_t linkedSpan = static_cast<uint32_t>(__bss_end - __image_start);
    Kernel::ImageFacts facts;
    if (const char* reason = Kernel::CheckImageData(__image_start, linkedBytes, facts)) Fail(reason);
    if (facts.imageBytes != linkedBytes) Fail("The kernel image size doesn't match the linked image");
    if (facts.memoryBytes != linkedSpan) Fail("The kernel memory span doesn't match the linked image");
    if (facts.loadAddress != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__image_start))) Fail("Where the kernel runs doesn't match the image data load address");
    if (facts.entryOffset != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_Kernel_Start) - reinterpret_cast<uintptr_t>(__image_start))) Fail("The kernel entry doesn't match the image data entry offset");
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The kernel image data matches the linked image");
    ReportKernelFacts(facts);

    // BootInfo：尺寸与版本先行，通过之后其余字段才是可解释的事实
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Checking the BootInfo");
    if (info == nullptr) Fail("The BootInfo pointer is null");
    if (info->version != Boot::InfoLayout::kVersion) Fail("The BootInfo version isn't supported");
    if (info->bytes < Boot::InfoLayout::kBytes) Fail("The BootInfo is shorter than the fields the kernel reads");
    if (info->flags != 0) Fail("The BootInfo flags aren't supported");
    if (facts.bootInfoVersion != Boot::InfoLayout::kVersion) Fail("The kernel image asks for a BootInfo version this kernel doesn't build");
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The BootInfo matches the kernel");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("Bytes=");
    Print::WriteHex(info->bytes);
    Print::Write(" Media=");
    Print::WriteHex(info->media);
    Print::Write(" Sect=");
    Print::WriteHex(info->sectorBytes);
    Print::Write(" Mmap=");
    Print::WriteHex(info->memoryMapCount);
    Print::Write("\r\n");

    // 固件启动服务失效：这一步不是清理，而是固定运行模式，此后不存在回退到固件的分支
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Invalidating the firmware boot services");
    switch (static_cast<Boot::Firmware>(info->firmware)) {
        case Boot::Firmware::None:
            Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "No firmware boot services are usable in long mode");
            break;
        case Boot::Firmware::UefiExited:
            Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The UEFI boot services are already exited");
            break;
        case Boot::Firmware::UefiActive:
            Fail("The UEFI boot services are still active and exiting them isn't implemented");
        default:
            Fail("The firmware state isn't supported");
    }

    // CPU 事实：长模式是入口契约的前提，能力位只作记录
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Probing the CPU");
    if (!s_cpu.Install()) Fail("The CPU doesn't support CPUID");
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The CPU is probed");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("Cpu=");
    Print::Write(s_cpu.Vendor());
    Print::Write(" LongMode=");
    Print::WriteHex(s_cpu.HasLongMode() ? 1 : 0);
    Print::Write(" Tsc=");
    Print::WriteHex(s_cpu.HasTsc() ? 1 : 0);
    Print::Write(" Apic=");
    Print::WriteHex(s_cpu.HasApic() ? 1 : 0);
    Print::Write(" Nx=");
    Print::WriteHex(s_cpu.HasNx() ? 1 : 0);
    Print::Write("\r\n");

    // 运行期段表、任务寄存器与中断表：此后 CPU 异常停在诊断行上，而不是三重故障
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Installing descriptor tables");
    if (const char* reason = Kernel::InstallDescriptorTables()) Fail(reason);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The descriptor tables are installed");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("Gdt=0x08 Idt=256 Tss=0x28 Ist=1\r\n");

    // 引导页表：恒等映射保持映像与栈可用，直映射窗口供内核访问物理内存
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Installing the boot page tables");
    if (const char* reason = Kernel::InstallBootPaging()) Fail(reason);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The boot page tables are installed");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("PhysMap=");
    Print::WriteHex64(Kernel::kPhysMapBase);
    Print::Write(" MapMiB=");
    Print::WriteHex(static_cast<uint32_t>(Kernel::kMapLimitBytes >> 20));
    Print::Write("\r\n");

    // 启动来源：内核据此知道自己是哪个介质、哪个卷、哪条路径被引导起来的，运行期更新也按它回写
    // 核对与播报放在引导页表之后：路径与设备线索是引导器给的物理地址，只有直映射窗口就绪后
    // 才能按 PhysToVirt 读；字段给不全不是失败，逐项按“未提供”处理
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Checking the boot source");
    if (CheckBootSource(info)) Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The boot source is consistent");
    ReportBootSource(info);

    // 内存层：按内存图放行可用页，保留低地址与内核自身；分配返回物理地址
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Initializing the page allocator");
    const uint64_t kernelBegin = reinterpret_cast<uintptr_t>(__image_start);
    const uint64_t kernelEnd = reinterpret_cast<uintptr_t>(__bss_end);
    s_pages.Initialize(*info, kernelBegin, kernelEnd);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The page allocator is ready");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("ManagedMiB=");
    Print::WriteHex(static_cast<uint32_t>(Kernel::kManagedLimitBytes >> 20));
    Print::Write(" FreeMiB=");
    Print::WriteHex(static_cast<uint32_t>(s_pages.FreePages() >> 8));   // 1MiB = 256 页
    Print::Write("\r\n");

    // 阶段边界：设备模型、模块装载、调度与 Init 尚未实现，停在可观测的状态上而不是跑进未定义代码
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Warn, "The kernel core stage ends here: the device model, module loader, scheduler and Init aren't implemented yet");
    Stop();
}
