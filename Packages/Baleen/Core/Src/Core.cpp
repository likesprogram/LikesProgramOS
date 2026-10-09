/* Core.cpp
    BaleenCore 引导阶段的主流程：装配自己的控制台并打印横幅，核对 Stub 交权块与自身镜像事实，
    初始化 CPU 快照，建立运行期段表与中断表，播报环境事实，读取并校验内核镜像，
    装载到装载地址、构造 BootInfo 与恒等映射页表，切长模式进入内核

    按 Stub 的分工约定，本文件承担全部启动流程与全部诊断输出：步骤顺序、进度行、
    失败原因与停机都集中在这里；Console.cpp 只做控制台装配，不打印也不停机
*/

#include <stdint.h>

#include <BiosBlockDevice.hpp>
#include <BootClock.hpp>
#include <BootInfo.hpp>
#include <BootMemory.hpp>
#include <CoreHandoff.hpp>
#include <Diagnostics.hpp>
#include <Ext4.hpp>
#include <ImageHeader.hpp>
#include <LayoutDescriptor.hpp>
#include <Libc.hpp>
#include <LoadKernel.hpp>
#include <Platform/Cpu.hpp>
#include <Platform/Descriptor.hpp>
#include <Print.hpp>
#include <Print/SerialTarget.hpp>
#include <PrintTarget.hpp>
#include <ReadFailure.hpp>
#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/SubDevice.hpp>

// 控制台装配入口在 Console.cpp
extern "C" void _Core_Console_Install();
// 异常入口桩地址表，由 Core.asm 提供，索引即向量号
extern "C" const uintptr_t _Core_Exception_Stubs[32];
// 交接入口：由 Core.asm 提供，切长模式并按恒等映射开分页后跳到内核入口，不返回
extern "C" [[noreturn]] void _Core_Enter_Kernel(uint32_t entry, uint32_t bootInfo, uint32_t pageTables);
// 链接脚本给出的镜像与未初始化区边界，用于与镜像头字段核对
extern "C" const uint8_t __image_start[];
extern "C" const uint8_t __image_end[];
extern "C" const uint8_t __bss_end[];

namespace {
    // 选择子：与 Stub 交权时的段表同号，BaleenCore 的运行期段表沿用这套编号
    constexpr uint16_t kSelectorCode32 = 0x08;   // 32 位代码段
    constexpr uint16_t kSelectorData32 = 0x10;   // 32 位数据段
    // CPU 异常向量数量：0 至 31，不含外部中断
    constexpr uint32_t kExceptionVectors = 32;
    // 布局描述的读取缓冲：一个 4096 字节块
    constexpr uint32_t kLayoutBlockBytes = 4096;
    // 交接状态：内存图副本的条目上限与 Stub 的内存图缓冲一致
    constexpr uint32_t kMemoryMapEntries = 64;
    // 恒等映射页表：4 级表、2MiB 大页覆盖低 4GiB，页目录数量按每张覆盖 1GiB 取
    constexpr uint32_t kPageBytes = 4096;
    constexpr uint64_t kHugePageBytes = 0x200000;
    constexpr uint32_t kIdentityDirectories = 4;
    constexpr uint64_t kPagePresent = 1ull << 0;      // 存在位
    constexpr uint64_t kPageReadWrite = 1ull << 1;    // 可写位
    constexpr uint64_t kPageHuge = 1ull << 7;         // 大页位：页目录项直接映射 2MiB
    constexpr uint32_t kEntriesPerTable = 512;

    // —— 本阶段失败原因的编号：编号是稳定标识，文本可以润色 ——
    constexpr Baleen::Diagnostic kFailHandoffNull{"E-CORE-01", "The handoff block pointer is null"};
    constexpr Baleen::Diagnostic kFailHandoffMagic{"E-CORE-02", "The handoff block magic doesn't match"};
    constexpr Baleen::Diagnostic kFailHandoffVersion{"E-CORE-03", "The handoff block version isn't supported"};
    constexpr Baleen::Diagnostic kFailHandoffBytes{"E-CORE-04", "The handoff block length isn't supported"};
    constexpr Baleen::Diagnostic kFailImageBytes{"E-CORE-05", "The image header size doesn't match the linked image"};
    constexpr Baleen::Diagnostic kFailImageMemory{"E-CORE-06", "The image header memory size doesn't match the linked image"};
    constexpr Baleen::Diagnostic kFailLoadAddress{"E-CORE-07", "Where BaleenCore runs doesn't match the handoff block load address"};
    constexpr Baleen::Diagnostic kFailHandoffImage{"E-CORE-08", "The handoff block image size doesn't match the image header"};
    constexpr Baleen::Diagnostic kFailCpuMode{"E-CORE-09", "The CPU mode doesn't match, it needs 32-bit protected mode"};
    constexpr Baleen::Diagnostic kFailCpuPae{"E-CORE-10", "The CPU doesn't support PAE, which the long-mode handover requires"};
    constexpr Baleen::Diagnostic kFailCpuLongMode{"E-CORE-11", "The CPU doesn't support long mode, which the kernel handover requires"};
    constexpr Baleen::Diagnostic kFailCpuNx{"E-CORE-12", "The CPU doesn't support NX, which the kernel memory protection requires"};
    constexpr Baleen::Diagnostic kFailGdt{"E-CORE-13", "Install gdt fail"};
    constexpr Baleen::Diagnostic kFailIdt{"E-CORE-14", "Install idt fail"};
    constexpr Baleen::Diagnostic kFailBounceWindow{"E-CORE-15", "Find a bounce window below 1MiB"};
    constexpr Baleen::Diagnostic kFailBindDisk{"E-CORE-16", "Bind the disk service"};
    constexpr Baleen::Diagnostic kFailReadLayout{"E-CORE-17", "Read the layout descriptor"};
    constexpr Baleen::Diagnostic kFailLayoutMagic{"E-CORE-18", "The layout descriptor magic doesn't match"};
    constexpr Baleen::Diagnostic kFailLayoutVersion{"E-CORE-19", "The layout descriptor version isn't supported"};
    constexpr Baleen::Diagnostic kFailLayoutHead{"E-CORE-20", "The layout descriptor header length isn't supported"};
    constexpr Baleen::Diagnostic kFailLayoutBytes{"E-CORE-21", "The layout descriptor length doesn't match"};
    constexpr Baleen::Diagnostic kFailLayoutFlags{"E-CORE-22", "The layout descriptor flags aren't supported"};
    constexpr Baleen::Diagnostic kFailLayoutReserved{"E-CORE-23", "The layout descriptor reserved field isn't zero"};
    constexpr Baleen::Diagnostic kFailLayoutChecksum{"E-CORE-24", "The layout descriptor checksum doesn't match"};
    constexpr Baleen::Diagnostic kFailLayoutPathBytes{"E-CORE-25", "The layout descriptor kernel path length is invalid"};
    constexpr Baleen::Diagnostic kFailLayoutPathAbsolute{"E-CORE-26", "The layout descriptor kernel path isn't absolute"};
    constexpr Baleen::Diagnostic kFailLayoutVolumeBytes{"E-CORE-27", "The layout descriptor volume length is zero"};
    constexpr Baleen::Diagnostic kFailOpenVolume{"E-CORE-28", "Open the system volume view"};
    constexpr Baleen::Diagnostic kFailMountVolume{"E-CORE-29", "Mount the system volume"};
    constexpr Baleen::Diagnostic kFailVolumeUuid{"E-CORE-30", "The system volume identifier doesn't match the layout descriptor"};
    constexpr Baleen::Diagnostic kException{"E-CORE-31", "Exception"};
    constexpr Baleen::Diagnostic kWarnBootBudget{"E-CORE-55", "The boot time is beyond the budget"};
    // 引导时间预算：从 Stub 进入算起，内核入口之前的毫秒数；取值依据见一级引导契约第七节
    constexpr uint32_t kBootBudgetMs = 3000;
    constexpr Baleen::Diagnostic kWarnKernelMissing{"E-CORE-32", "The kernel image isn't in the system volume: the volume has no kernel yet"};
    // 选版与签名：记录格式与信任锚由选版规格定义，当前直接启动布局描述指定的已确认镜像
    constexpr Baleen::Diagnostic kWarnSelectSkip{"E-CORE-56", "The kernel set selection and signature check aren't implemented: starting the image the layout names"};
    // 没有内核镜像可交：停在可观测的边界上，不再往装载与交接走
    constexpr Baleen::Diagnostic kWarnNoKernel{"E-CORE-58", "The BaleenCore stage ends here: there is no kernel to hand over"};

    // CPU 事实快照
    Platform::Cpu s_cpu;
    // 引导时钟：入口取时间戳并校准；总耗时用交权块里 Stub 的校准值换算
    Baleen::BootClock s_clock;
    // BaleenCore 自己的运行期段表与中断表：成员初值都是常量，不依赖启动运行库
    // 中断表只覆盖处理器异常向量（0..31）：运行期关中断，表限长不必覆盖 256 个向量
    Platform::Gdt s_gdt;
    Platform::IdtTable<kExceptionVectors> s_idt;
    // 介质块设备、系统卷偏移视图与 Ext4 实例：静态存储，引导期不做动态分配
    Baleen::Core::BiosBlockDevice s_medium;
    Storage::SubDevice s_systemVolume;
    Baleen::Core::Ext4FileSystem s_ext4;
    // 布局描述原字节
    uint8_t s_layoutBlock[kLayoutBlockBytes] = {};
    // 交权块：固定字段校验通过后常驻，Fail 路径经它取读盘失败记录
    const Baleen::CoreHandoff* s_handoff = nullptr;
    // 交接状态：BootInfo、内存图副本与恒等映射页表，都在未初始化区
    // 内核入口按物理地址读取这些事实，并把它们所在的内存标为已占用而保留
    Boot::Info s_bootInfo;
    Boot::MemoryMapEntry s_bootMemoryMap[kMemoryMapEntries];
    alignas(kPageBytes) uint64_t s_pml4[kEntriesPerTable];
    alignas(kPageBytes) uint64_t s_pdpt[kEntriesPerTable];
    alignas(kPageBytes) uint64_t s_pd[kIdentityDirectories][kEntriesPerTable];

    // 按小端读取 16 位
    uint16_t Get16(const uint8_t* field) {
        return static_cast<uint16_t>(field[0] | (field[1] << 8));
    }

    // 按小端读取 32 位
    uint32_t Get32(const uint8_t* field) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
        return value;
    }

    // 按小端读取 64 位
    uint64_t Get64(const uint8_t* field) {
        return static_cast<uint64_t>(Get32(field)) | (static_cast<uint64_t>(Get32(field + 4)) << 32);
    }

    // 挑低地址弹跳窗口：判定与 Stub 共用 Baleen::FindBounceWindow，相邻可用条目可以合并覆盖
    // 返回窗口起址；没有可用窗口返回空
    uint8_t* FindBounceWindow(const Baleen::CoreHandoff& handoff) {
        const uint32_t base = Baleen::FindBounceWindow(handoff.memoryMap, handoff.memoryMapCount, Baleen::kBounceBegin, Baleen::kBounceEnd, Baleen::kBounceBytes);
        return base == 0 ? nullptr : reinterpret_cast<uint8_t*>(base);
    }

    // 校验布局描述的固定字段与校验和：不通过时返回带编号的原因
    Baleen::Diagnostic CheckLayoutBlock(const uint8_t* block) {
        using Baleen::LayoutDescriptor;
        if (Get32(block + LayoutDescriptor::kFieldMagic) != LayoutDescriptor::kMagic) return kFailLayoutMagic;
        if (Get16(block + LayoutDescriptor::kFieldVersion) != LayoutDescriptor::kVersion) return kFailLayoutVersion;
        if (Get16(block + LayoutDescriptor::kFieldHeaderBytes) != LayoutDescriptor::kHeaderBytes) return kFailLayoutHead;
        if (Get32(block + LayoutDescriptor::kFieldLayoutBytes) != LayoutDescriptor::kLayoutBytes) return kFailLayoutBytes;
        if (Get32(block + LayoutDescriptor::kFieldFlags) != 0) return kFailLayoutFlags;
        if (Get16(block + LayoutDescriptor::kFieldReserved) != 0) return kFailLayoutReserved;
        if (Get32(block + LayoutDescriptor::kFieldChecksum) != LayoutDescriptor::Checksum(block)) return kFailLayoutChecksum;
        const uint32_t pathBytes = Get16(block + LayoutDescriptor::kFieldKernelPathBytes);
        if (pathBytes == 0 || pathBytes > LayoutDescriptor::kKernelPathMaxBytes) return kFailLayoutPathBytes;
        if (block[LayoutDescriptor::kFieldKernelPath] != '/') return kFailLayoutPathAbsolute;
        if (Get64(block + LayoutDescriptor::kFieldVolumeBytes) == 0) return kFailLayoutVolumeBytes;
        return Baleen::Diagnostic{};
    }

    // 引导耗时：从 Stub 进入起算，换算用 Stub 校准的每毫秒计数；没有计时事实时只报本阶段
    // 超出预算时以 WARN 与编号 E-CORE-55 报出，未超出是续行
    void ReportBootTime(const Baleen::CoreHandoff& handoff) {
        const uint32_t stageMs = s_clock.MillisBetween(s_clock.EntryStamp(), s_clock.Stamp());
        const uint64_t bootStamp = (static_cast<uint64_t>(handoff.bootTscHigh) << 32) | handoff.bootTscLow;
        const uint32_t totalMs = Baleen::MillisBetween(bootStamp, s_clock.Stamp(), handoff.ticksPerMs);
        const bool over = handoff.ticksPerMs != 0 && totalMs > kBootBudgetMs;
        Baleen::PrintTargets::WriteTag(over ? Baleen::PrintTargets::Tag::Warn : Baleen::PrintTargets::Tag::Continue);
        if (over) {
            Print::Write(kWarnBootBudget.code);
            Print::Write(" ");
        }
        Print::Write("BootMs=");
        Print::WriteHex(totalMs);
        Print::Write(" StageMs=");
        Print::WriteHex(stageMs);
        Print::Write(" BudgetMs=");
        Print::WriteHex(kBootBudgetMs);
        Print::Write(" Verdict=");
        Print::Write(handoff.ticksPerMs == 0 ? "NoClock" : (over ? "Over" : "Ok"));
        Print::Write("\r\n");
    }

    // 停机并停留，供自动化读取诊断；失败路径与阶段边界共用
    [[noreturn]] void Stop() {
        for (;;) asm volatile("CLI; HLT");
    }

    // 打印 FATAL 原因后停机；最后一次调用读盘服务失败时补一行现场
    // detail 非空时补在同一行末尾（存储层错误名一类），记录由 Stub 的读盘服务维护，
    // 读盘成功的调用会清空它，非读盘失败因此不会多出这一行
    [[noreturn]] void Fail(const Baleen::Diagnostic& diagnostic, const char* detail = nullptr) {
        Baleen::WriteDiagnostic(Baleen::PrintTargets::Tag::Fatal, diagnostic, detail);
        if (s_handoff != nullptr && s_handoff->lastReadFailure != nullptr && s_handoff->lastReadFailure->phase != 0)
            Baleen::WriteReadFailureLine(*s_handoff->lastReadFailure);
        Stop();
    }

    // 该向量是否由处理器压入错误码；其余向量的栈帧里没有错误码
    bool HasErrorCode(uint32_t vector) {
        return vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21 || vector == 29 || vector == 30;
    }

    // 建立运行期段表与中断表并装载；调用前中断须已关闭
    // 异常门全部指向 BaleenCore 的停机诊断入口：出错时屏上留下向量与出错位置，而不是三重故障
    void InstallDescriptorTables() {
        s_gdt.Clear();
        if (!s_gdt.SetCode(1, 0, Platform::CodeWidth::Bits32) || !s_gdt.SetData(2, 0, Platform::DataWidth::Bits32)) Fail(kFailGdt);
        s_gdt.Load(kSelectorCode32, kSelectorData32);

        s_idt.Clear();
        for (uint32_t vector = 0; vector < kExceptionVectors; ++vector) {
            if (!s_idt.SetGate(vector, _Core_Exception_Stubs[vector], kSelectorCode32, Platform::GateType::Interrupt, 0, 0)) Fail(kFailIdt);
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

    // 构造交给内核的 BootInfo：字段取自交权块，内存图复制到本阶段的静态存储
    // 固件状态填 None：BIOS 路径进入长模式后没有可调用的固件服务，内核按这个事实固定运行模式
    // 输出通道按事实给出：调试口无条件可用，串口以存在性探测为准；内核按这些位装配自己的控制台
    void BuildBootInfo(const Baleen::CoreHandoff& handoff) {
        uint32_t count = handoff.memoryMapCount;
        if (count > kMemoryMapEntries) count = kMemoryMapEntries;
        for (uint32_t i = 0; i < count; ++i) s_bootMemoryMap[i] = handoff.memoryMap[i];
        s_bootInfo.bytes = Boot::InfoLayout::kBytes;
        s_bootInfo.version = Boot::InfoLayout::kVersion;
        s_bootInfo.flags = 0;
        s_bootInfo.firmware = static_cast<uint32_t>(Boot::Firmware::None);
        s_bootInfo.media = handoff.media;
        s_bootInfo.sectorBytes = handoff.sectorBytes;
        uint32_t channels = static_cast<uint32_t>(Boot::Channel::DebugPort);
        if (Print::SerialTarget::Probe(Print::SerialTarget::kDefaultBase)) channels |= static_cast<uint32_t>(Boot::Channel::Serial);
        s_bootInfo.channels = channels;
        s_bootInfo.memoryMapCount = count;
        s_bootInfo.memoryMapTruncated = handoff.memoryMapTruncated;
        s_bootInfo.reserved0 = 0;
        for (uint32_t i = 0; i < 4; ++i) s_bootInfo.reserved[i] = 0;
        s_bootInfo.memoryMap = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s_bootMemoryMap));
    }

    // 建恒等映射页表：PML4[0] 指向 PDPT，PDPT 的 4 项各指向一张页目录，2MiB 大页覆盖低 4GiB
    // 这是内核入口到内核换上自己页表之间的过渡映射；直映射窗口与更细的页粒度由内核自己建
    // 表内其余项保持未初始化区的零值：Stub 交权前按 MemoryBytes 清零，这里不重复清零
    void BuildIdentityPageTables() {
        s_pml4[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s_pdpt)) | kPagePresent | kPageReadWrite;
        for (uint32_t i = 0; i < kIdentityDirectories; ++i) {
            s_pdpt[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s_pd[i])) | kPagePresent | kPageReadWrite;
            for (uint32_t j = 0; j < kEntriesPerTable; ++j) {
                const uint64_t frame = (static_cast<uint64_t>(i) * kEntriesPerTable + j) * kHugePageBytes;
                s_pd[i][j] = frame | kPagePresent | kPageReadWrite | kPageHuge;
            }
        }
    }
}

// CPU 异常入口的公共目标：打印向量与出错位置后停机，不返回；帧由 Core.asm 的入口桩按处理器压栈顺序排列
extern "C" void _Core_Exception_Handler(const uint32_t* frame) {
    const uint32_t vector = frame[0];
    const uint32_t* tail = frame + 1;
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Fatal);
    Print::Write(kException.code);
    Print::Write(" Exception ");
    Print::WriteHex(vector);
    if (HasErrorCode(vector)) { Print::Write(" Err="); Print::WriteHex(tail[0]); ++tail; }   // 有错误码的向量由处理器先压错误码
    Print::Write(" Eip=");
    Print::WriteHex(tail[0]);
    Print::Write("\r\n");
    for (;;) asm volatile("CLI; HLT");
}

// BaleenCore 主流程：由 Core.asm 的入口以 ESI 里的交权块指针调用，不返回
extern "C" void _Core_Main(const Baleen::CoreHandoff* handoff) {
    // 引导计时先于一切：时间戳在 Install 入口取，控制台装配与校准的耗时都算在本阶段里
    s_clock.Install();
    // 装配 BaleenCore 自己的控制台：横幅与后面每一步的结论都从这里出去
    _Core_Console_Install();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::BaleenCore, "LikesProgramOS BaleenCore");

    // 交权块：三项固定字段先行，通过之后其余字段才是可解释的事实
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Checking the handoff block");
    if (handoff == nullptr) Fail(kFailHandoffNull);
    if (handoff->magic != Baleen::CoreHandoffLayout::kMagic) Fail(kFailHandoffMagic);
    if (handoff->version != Baleen::CoreHandoffLayout::kVersion) Fail(kFailHandoffVersion);
    if (handoff->bytes != Baleen::CoreHandoffLayout::kBytes) Fail(kFailHandoffBytes);
    // 固定字段已通过：从这里起 Fail 路径可以取用块里的读盘失败记录
    s_handoff = handoff;
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The handoff block is valid");

    // 自身镜像事实：摘要已由 Stub 在交权前比对，这里不重复取；核对头字段与链接布局、装载地址
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Checking the BaleenCore image against the handoff block");
    Baleen::ImageFacts facts;
    if (const Baleen::Diagnostic reason = Baleen::CheckImageHeader(__image_start, Baleen::kCoreImageMagic, facts, Baleen::ImageDiagStage::Core); reason.code != nullptr) Fail(reason);
    if (facts.imageBytes != static_cast<uint32_t>(__image_end - __image_start)) Fail(kFailImageBytes);
    if (facts.memoryBytes != static_cast<uint32_t>(__bss_end - __image_start)) Fail(kFailImageMemory);
    const uint32_t imageLoad = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__image_start));
    if (handoff->coreLoad != imageLoad) Fail(kFailLoadAddress);
    if (handoff->coreBytes != facts.imageBytes) Fail(kFailHandoffImage);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The BaleenCore image matches the handoff block");

    // CPU 事实快照：模式不符时后续步骤没有意义
    s_cpu.Install();
    if (s_cpu.Mode() != Platform::CpuMode::Protected32) Fail(kFailCpuMode);
    // 交接阶段的前置能力：内核按 64 位构建，切长模式需要 PAE 与长模式，页保护需要 NX
    // 缺哪一项都在这里明确失败，而不是在切模式或进入内核时崩掉
    if (!s_cpu.HasPae()) Fail(kFailCpuPae);
    if (!s_cpu.HasLongMode()) Fail(kFailCpuLongMode);
    if (!s_cpu.HasNx()) Fail(kFailCpuNx);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Initializing the CPU");

    // 运行期段表与中断表：此后 CPU 异常停在诊断行上，而不是无输出的三重故障
    InstallDescriptorTables();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Installing descriptor tables");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Cpu=");
    Print::Write(s_cpu.HasCpuid() ? s_cpu.Vendor() : "no-cpuid");
    Print::Write(" Mode=");
    Print::WriteHex(static_cast<uint32_t>(s_cpu.Mode()));
    Print::Write(" Pae=");
    Print::WriteHex(s_cpu.HasPae() ? 1 : 0);
    Print::Write(" Long=");
    Print::WriteHex(s_cpu.HasLongMode() ? 1 : 0);
    Print::Write(" Nx=");
    Print::WriteHex(s_cpu.HasNx() ? 1 : 0);
    Print::Write(" Build=");
    Print::WriteHex(facts.buildId);
    Print::Write("\r\n");

    // 环境事实：介质、BaleenCore 装载与内存图都来自交权块，后续阶段按这些事实定位系统卷
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

    // —— 布局描述：读固定位置的一个 4096 字节块，校验固定字段与校验和 ——
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Reading the layout descriptor");
    uint8_t* const bounce = FindBounceWindow(*handoff);
    if (bounce == nullptr) Fail(kFailBounceWindow);
    // 介质块设备：读盘服务只接受低 1MiB 目的地，窗口在低地址里挑；容量按 LBA 的 32 位上限声明
    s_medium.Bind(handoff->readSectors, handoff->sectorBytes, bounce, Baleen::kBounceBytes, static_cast<uint64_t>(handoff->sectorBytes) << 32);
    if (s_medium.BlockBytes() != handoff->sectorBytes || s_medium.BlockBytes() == 0) Fail(kFailBindDisk);
    if (const Storage::Error error = s_medium.Read(Baleen::LayoutDescriptor::kOffset, s_layoutBlock, sizeof(s_layoutBlock)); error != Storage::Error::Ok)
        Fail(kFailReadLayout, Storage::ErrorMessage(error));
    if (const Baleen::Diagnostic reason = CheckLayoutBlock(s_layoutBlock); reason.code != nullptr) Fail(reason);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The layout descriptor is valid");
    const uint64_t volumeOffset = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeOffset);
    const uint64_t volumeBytes = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeBytes);
    const uint64_t stateOffset = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldStateOffset);
    const uint32_t kernelPathBytes = Get16(s_layoutBlock + Baleen::LayoutDescriptor::kFieldKernelPathBytes);
    char kernelPath[Baleen::LayoutDescriptor::kKernelPathMaxBytes + 1] = {};
    memcpy(kernelPath, s_layoutBlock + Baleen::LayoutDescriptor::kFieldKernelPath, kernelPathBytes);
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Volume=");
    Print::WriteHex64(volumeOffset);
    Print::Write(" Bytes=");
    Print::WriteHex64(volumeBytes);
    Print::Write(" State=");
    Print::WriteHex64(stateOffset);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Kernel=");
    Print::Write(kernelPath);
    Print::Write("\r\n");

    // —— 系统卷：在介质设备上开偏移视图，挂 Ext4 并核对卷标识 ——
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Mounting the system volume");
    if (const Storage::Error error = Storage::SubDevice::Open(s_medium, volumeOffset, volumeBytes, s_systemVolume); error != Storage::Error::Ok)
        Fail(kFailOpenVolume, Storage::ErrorMessage(error));
    if (const Storage::Error error = s_ext4.Mount(s_systemVolume, Storage::MountMode::ReadOnly); error != Storage::Error::Ok)
        Fail(kFailMountVolume, Storage::ErrorMessage(error));
    if (memcmp(s_ext4.Uuid(), s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeUuid, 16) != 0)
        Fail(kFailVolumeUuid);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The system volume matches the layout descriptor");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Ext4=");
    Print::WriteHex(s_ext4.BlockBytes());
    Print::Write(" Inodes=");
    Print::WriteHex(s_ext4.InodesCount());
    Print::Write(" Blocks=");
    Print::WriteHex64(s_ext4.BlocksCount());
    Print::Write("\r\n");

    // —— 内核镜像：按布局描述给的路径定位、读取并校验数据与整幅摘要 ——
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Locating and checking the kernel image");
    Baleen::Core::KernelFacts kernelFacts;
    bool kernelMissing = false;
    const Baleen::Diagnostic kernelReason = Baleen::Core::CheckKernelImage(s_ext4, kernelPath, handoff->memoryMap, handoff->memoryMapCount, kernelFacts, kernelMissing);
    if (kernelMissing) {
        Baleen::WriteDiagnostic(Baleen::PrintTargets::Tag::Warn, kWarnKernelMissing);
    } else if (kernelReason.code != nullptr) {
        Fail(kernelReason);
    } else {
        Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The kernel image is valid");
        Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
        Print::Write("Bytes=");
        Print::WriteHex(kernelFacts.imageBytes);
        Print::Write(" Load=");
        Print::WriteHex(kernelFacts.loadAddress);
        Print::Write(" Entry=");
        Print::WriteHex(kernelFacts.entryOffset);
        Print::Write("\r\n");
        Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
        Print::Write("BootInfo=");
        Print::WriteHex(kernelFacts.bootInfoVersion);
        Print::Write(" Build=");
        Print::WriteHex(kernelFacts.buildId);
        Print::Write("\r\n");
    }

    // 没有内核镜像可交：WARN 已在上一步给出，停在可观测的边界上而不是空转
    // 停在这里也是本阶段的终点，照常报出耗时与预算判定
    if (kernelMissing) {
        Baleen::WriteDiagnostic(Baleen::PrintTargets::Tag::Warn, kWarnNoKernel);
        ReportBootTime(*handoff);
        Stop();
    }

    // 选版与签名：记录格式与信任锚由选版规格定义，当前直接启动布局描述指定的已确认镜像
    Baleen::WriteDiagnostic(Baleen::PrintTargets::Tag::Warn, kWarnSelectSkip);

    // —— 装载：整幅读进装载地址，按静态内存跨度清零未落盘尾部 ——
    // 读盘按扇区上取整，末尾填充也被写进内存，清零覆盖文件末尾之后的所有字节
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Loading the kernel image");
    if (const Baleen::Diagnostic reason = Baleen::Core::LoadKernelImage(s_ext4, kernelPath, kernelFacts.loadAddress, kernelFacts.imageBytes); reason.code != nullptr)
        Fail(reason);
    uint8_t* const kernelImage = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kernelFacts.loadAddress));
    if (kernelFacts.memoryBytes > kernelFacts.imageBytes)
        memset(kernelImage + kernelFacts.imageBytes, 0, kernelFacts.memoryBytes - kernelFacts.imageBytes);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The kernel image is loaded");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Load=");
    Print::WriteHex(kernelFacts.loadAddress);
    Print::Write(" Bytes=");
    Print::WriteHex(kernelFacts.imageBytes);
    Print::Write(" Span=");
    Print::WriteHex(kernelFacts.memoryBytes);
    Print::Write("\r\n");

    // —— 交接状态：BootInfo 与恒等映射页表，都在本阶段的静态存储里 ——
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Building the handover state");
    BuildBootInfo(*handoff);
    BuildIdentityPageTables();
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The handover state is ready");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("BootInfo=");
    Print::WriteHex(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_bootInfo)));
    Print::Write(" Mmap=");
    Print::WriteHex(s_bootInfo.memoryMapCount);
    Print::Write(" Channels=");
    Print::WriteHex(s_bootInfo.channels);
    Print::Write("\r\n");

    // 引导耗时：从 Stub 进入起算到交接完成的总耗时与本阶段耗时，预算判定同行的 Verdict 给出
    // 报在装载与交接状态之后：装载是读盘的主要项，预算终点按契约取内核入口
    ReportBootTime(*handoff);

    // —— 进入内核：切长模式与恒等映射分页，跳到装载地址加入口偏移 ——
    // 交权后 BaleenCore 冻结常驻；内核会重建自己的段表、中断表与页表，并保留这份 BootInfo
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Entering the kernel");
    _Core_Enter_Kernel(kernelFacts.loadAddress + kernelFacts.entryOffset, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_bootInfo)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_pml4)));
    Stop();
}
