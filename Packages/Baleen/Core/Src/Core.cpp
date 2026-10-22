/* Core.cpp
    BaleenCore 引导阶段的主流程：装配自己的控制台并打印横幅，核对 Stub 交权块与自身镜像事实，
    初始化 CPU 快照，建立运行期段表与中断表，播报环境事实，选版并读取、校验内核镜像，
    按 Modules.list 预装引导闭包，装载到装载地址、构造 BootInfo 与恒等映射页表，切长模式进入内核

    只有内核镜像、清单副本、模块记录与模块字节全部就绪才交权：装载或预装失败都按两阶段选版
    回退（候选集失败改选已知良好集，已知良好集失败终止），不交出残缺的交接现场

    按 Stub 的分工约定，本文件承担全部启动流程与全部诊断输出：步骤顺序、进度行、
    失败原因与停机都集中在这里；Console.cpp 只做控制台装配，不打印也不停机
*/

#include <stdint.h>

#include <DiskFlush.hpp>
#include <BiosBlockDevice.hpp>
#include <BootClock.hpp>
#include <BootClosure.hpp>
#include <BootInfo.hpp>
#include <BootMemory.hpp>
#include <CoreHandoff.hpp>
#include <Diagnostics.hpp>
#include <Ext4.hpp>
#include <ImageHeader.hpp>
#include <LayoutCheck.hpp>
#include <LayoutDescriptor.hpp>
#include <Libc.hpp>
#include <LoadKernel.hpp>
#include <Platform/Cpu.hpp>
#include <Platform/Descriptor.hpp>
#include <Print.hpp>
#include <Print/SerialTarget.hpp>
#include <PrintTarget.hpp>
#include <ReadFailure.hpp>
#include <ScreenFacts.hpp>
#include <SelectSet.hpp>
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
    // 写缓存刷新服务：把实际路径（AHCI 或传统 IDE）的结论转成块设备的刷新结果绑给介质
    // 没有可刷新的端口（光盘、USB、NVMe）报 Unsupported，命令出错或超时报 Failed
    Baleen::Core::FlushOutcome FlushMedium() {
        switch (Baleen::Core::DiskFlush::Flush()) {
            case Baleen::Core::DiskFlush::Result::Succeeded: return Baleen::Core::FlushOutcome::Flushed;
            case Baleen::Core::DiskFlush::Result::Failed: return Baleen::Core::FlushOutcome::Failed;
            default: return Baleen::Core::FlushOutcome::Unsupported;
        }
    }

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
    constexpr Baleen::Diagnostic kFailOpenVolume{"E-CORE-28", "Open the system volume view"};
    constexpr Baleen::Diagnostic kFailMountVolume{"E-CORE-29", "Mount the system volume"};
    constexpr Baleen::Diagnostic kFailVolumeUuid{"E-CORE-30", "The system volume identifier doesn't match the layout descriptor"};
    constexpr Baleen::Diagnostic kException{"E-CORE-31", "Exception"};
    constexpr Baleen::Diagnostic kWarnBootBudget{"E-CORE-55", "The boot time is beyond the budget"};
    // 引导时间预算：从 Stub 进入算起，内核入口之前的毫秒数
    constexpr uint32_t kBootBudgetMs = 3000;
    constexpr Baleen::Diagnostic kWarnKernelMissing{"E-CORE-32", "The kernel image isn't in the system volume: the volume has no kernel yet"};
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
    // 内核路径与 BIOS 驱动器号：都放在静态存储里，交接后由内核按物理地址读取
    // 内核路径原本是 _Core_Main 的局部量，交接时把栈地址交给内核太脆；驱动器号只对固件环境有意义，供诊断
    char s_kernelPath[Baleen::LayoutDescriptor::kKernelPathMaxBytes + 1] = {};
    uint8_t s_biosDrive = 0;
    // 刷新结论：可刷新的端口是不是启动介质所在的设备（读首块后经 AHCI 直读核验）
    bool s_flushCovers = false;
    // 启动介质的第 0 号块：身份核验的比对基准（光盘与硬盘在这一块上必然不同，布局描述块则可能相同）
    uint8_t s_firstBlock[kLayoutBlockBytes] = {};
    // 选版结果：目标集路径、集目录与集身份
    Baleen::Core::SelectedSet s_selected;
    // 预装：清单条目到预装条目的视图缓冲、来源与计划；机制本身不动态分配，这些都放静态存储
    Baleen::Core::PreloadArtifact s_listedArtifactViews[Baleen::Core::kPreloadMaxEntries] = {};
    Baleen::Core::PreloadSource s_preloadSource;
    Baleen::Core::PreloadPlan s_preloadPlan;
    // 预装结果：清单副本、列表副本与记录数组的物理地址与长度，随 BootInfo 交给内核
    Baleen::Core::PreloadResult s_preloadResult;
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

    // 校验布局描述：判据、编号与 UEFI 侧共用 LayoutCheck 的一份实现
    Baleen::Diagnostic CheckLayoutBlock(const uint8_t* block) {
        return Baleen::Core::LayoutCheck::Check(block);
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

    // 把清单事实里的列表产物视图搭成预装机制的条目视图：两者字段同义、布局不同，
    // 这里只搭视图，路径与摘要仍指向清单副本里的字节（本阶段只读）；非闭包条目也在视图里，
    // PlanPreload 按引导闭包期望集筛出要预装的成员
    uint32_t BuildListedArtifacts(const Baleen::Core::ManifestFacts& manifest) {
        uint32_t count = manifest.listedArtifactCount;
        if (count > Baleen::Core::kPreloadMaxEntries) count = Baleen::Core::kPreloadMaxEntries;
        for (uint32_t i = 0; i < count; ++i) {
            const Baleen::Core::ListedArtifact& entry = manifest.listedArtifacts[i];
            s_listedArtifactViews[i].artifactIndex = entry.artifactIndex;
            s_listedArtifactViews[i].pathBytes = static_cast<uint16_t>(entry.pathBytes);
            s_listedArtifactViews[i].dataBytes = entry.dataBytes;
            s_listedArtifactViews[i].path = entry.path;
            s_listedArtifactViews[i].digest = entry.digest;
        }
        return count;
    }

    // 预装引导闭包：解析 Modules.list、按期望集筛出闭包成员并逐个核对与读入，产出三类交接缓冲事实
    // 落点在内存图里找低 4 GiB 的连续可用区间，避开低 1 MiB、引导器映像与内核装载跨度；
    // 失败返回带编号的原因且不留下半套记录，调用方按两阶段选版回退处理
    Baleen::Diagnostic PreloadBootClosure(const Baleen::CoreHandoff& handoff, const Baleen::Core::KernelFacts& kernelFacts) {
        const Baleen::Core::ManifestFacts& manifest = s_selected.handoff.manifest;
        s_preloadSource = Baleen::Core::PreloadSource{};
        s_preloadSource.setDirectory = s_selected.setDirectory;
        s_preloadSource.manifest = manifest.manifestCopy;
        s_preloadSource.manifestBytes = manifest.manifestBytes;
        s_preloadSource.list = manifest.bootListCopy;
        s_preloadSource.listBytes = manifest.bootListBytes;
        s_preloadSource.moduleMajor = manifest.moduleMajor;
        s_preloadSource.artifacts = s_listedArtifactViews;
        s_preloadSource.artifactCount = BuildListedArtifacts(manifest);
        if (const Baleen::Diagnostic reason = Baleen::Core::PlanPreload(s_preloadSource, s_preloadPlan); reason.code != nullptr) return reason;
        Baleen::Core::MemoryMapAllocator allocator(handoff.memoryMap, handoff.memoryMapCount,
            reinterpret_cast<uintptr_t>(__image_start), static_cast<uint64_t>(__bss_end - __image_start),
            kernelFacts.loadAddress, kernelFacts.memoryBytes);
        Baleen::Core::StoragePreloadSink sink(s_ext4, allocator);
        return Baleen::Core::PreloadModules(s_preloadPlan, sink, s_preloadResult);
    }

    // 写十进制无符号数：预装摘要行的 count 与 bytes 用十进制，只做 32 位除法，
    // 避免 32 位目标上把 64 位除法与运行库带进镜像
    void WriteDecimal(uint32_t value) {
        char text[11] = {};
        uint32_t digits = 0;
        do {
            text[digits++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && digits < sizeof(text) - 1);
        for (uint32_t i = 0; i < digits / 2; ++i) {
            const char swap = text[i];
            text[i] = text[digits - 1 - i];
            text[digits - 1 - i] = swap;
        }
        text[digits] = '\0';
        Print::Write(text);
    }

    // 预装摘要行：G2/G5 断言用，count 与记录数一致、bytes 是全部模块字节之和、bad 恒为 0
    // 预装失败不落成半套，也不走本行；每模块的 idx/base/len 只在 DEBUG 等级输出，本阶段不输出
    void ReportPreloadSummary() {
        Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
        Print::Write("boot closure preloaded: count=");
        WriteDecimal(s_preloadResult.recordCount);
        Print::Write(" bytes=");
        WriteDecimal(static_cast<uint32_t>(s_preloadResult.moduleBytes));
        Print::Write(" bad=0\r\n");
    }

    // 播报本次选定的目标集：集标识按字节顺序的 64 位小写十六进制，与集目录名同一串字符
    // 回退改选之后同样播报一次，日志里能看出交接用的是哪一份
    void ReportSelectedSet() {
        Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
        Print::Write("Set=");
        static const char kHexDigits[] = "0123456789abcdef";
        for (uint32_t i = 0; i < 32; ++i) {
            const char text[3] = { kHexDigits[s_selected.buildId[i] >> 4], kHexDigits[s_selected.buildId[i] & 0x0f], '\0' };
            Print::Write(text);
        }
        Print::Write("\r\n");
        Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
        Print::Write("Attempt=");
        Print::WriteHex64(s_selected.attemptId);
        Print::Write(" Rev=");
        Print::WriteHex64(s_selected.moduleRevision);
        Print::Write("\r\n");
    }

    // 测试用故障注入：只在 FAULT_INJECT 非零的测试构建里生效（发布构建编译为空）
    // 预装、验签与选版都已完成，注入点只改“交给内核的那一份事实”，因此断言落在内核侧 E-KRN-5x：
    //   1 交接包缺失（清单副本指针清零）      → 内核 E-KRN-50
    //   2 清单副本魔数损坏                    → 内核 E-KRN-52
    //   3 记录数组头计数损坏                  → 内核 E-KRN-53
    //   4 清单副本 BuildId 损坏               → 内核 E-KRN-56
    //   5 列表副本主版本损坏                  → 内核 E-KRN-56
    //   6 首个模块字节损坏（摘要不符）        → 内核 E-KRN-58
    // 注入都不改引导器自己的判定与日志：引导器仍认为交接包完整，失败可见性由内核侧给出
#if FAULT_INJECT
    void ApplyFaultInjection() {
        uint8_t* const manifest = reinterpret_cast<uint8_t*>(s_bootInfo.kernelSetManifest);
        // 记录数组：8 字节头（计数）后接 16 字节记录；记录里的 phys 指向模块字节
        uint8_t* const records = reinterpret_cast<uint8_t*>(s_bootInfo.bootModules);
        switch (FAULT_INJECT) {
            case 1: s_bootInfo.kernelSetManifest = 0; break;
            case 2: if (manifest != nullptr) manifest[0] = static_cast<uint8_t>(~manifest[0]); break;
            case 3: if (records != nullptr) { records[0] = 0xFF; records[1] = 0xFF; } break;
            case 4: if (manifest != nullptr) manifest[0x10] = static_cast<uint8_t>(~manifest[0x10]); break;
            case 5: {
                // 列表副本的第三行是 major 行：把版本数字改成 2，与 .os 的 major 1 不等
                uint8_t* const list = reinterpret_cast<uint8_t*>(s_bootInfo.bootList);
                const uint32_t bytes = s_bootInfo.bootListBytes;
                for (uint32_t index = 0; index + 7 < bytes; ++index) {
                    if (list[index] == 'm' && list[index + 1] == 'a' && list[index + 2] == 'j' && list[index + 3] == 'o' && list[index + 4] == 'r' && list[index + 5] == ' ') {
                        list[index + 6] = '2';
                        break;
                    }
                }
                break;
            }
            case 6: if (records != nullptr) {
                // 记录布局：8 字节头 + 每条 {phys u64, bytes u32, artifactIndex u16, reserved u16}
                // 改模块文件的最后一个字节：容器解析不读段文件内容，但摘要必然不符 → 内核装载第 1 步 E-KRN-58
                uint64_t modulePhys = 0;
                uint32_t moduleBytes = 0;
                for (uint32_t index = 0; index < 8; ++index) modulePhys |= static_cast<uint64_t>(records[8 + index]) << (8 * index);
                for (uint32_t index = 0; index < 4; ++index) moduleBytes |= static_cast<uint32_t>(records[16 + index]) << (8 * index);
                uint8_t* const module = reinterpret_cast<uint8_t*>(modulePhys);
                if (module != nullptr && moduleBytes != 0) module[moduleBytes - 1] = static_cast<uint8_t>(~module[moduleBytes - 1]);
            } break;
            default: break;
        }
    }
#endif

    // 构造交给内核的 BootInfo：字段取自交权块与布局描述，内存图复制到本阶段的静态存储
    // 固件状态填 None：BIOS 路径进入长模式后没有可调用的固件服务，内核按这个事实固定运行模式
    // 输出通道按事实给出：调试口无条件可用，串口以存在性探测为准；内核按这些位装配自己的控制台
    // 启动来源取自布局描述：内核与运行期更新据此定位启动介质、系统卷、状态区域与内核路径
    // 引导闭包交接包与屏幕事实由预装与采集步骤给出，本函数只按字段一一填入
    void BuildBootInfo(const Baleen::CoreHandoff& handoff, const Baleen::Core::SelectedSet& selected, const Baleen::Core::PreloadResult& preloads, const Baleen::Core::ScreenFacts& screen) {
        using Baleen::LayoutDescriptor;
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
        // FATAL 等待按构建配置给出：0 = 内核按默认 10 秒；屏幕事实按引导期采集到的形态填，
        // 采集不成立时是 None，内核只走流式通道，不改启动判定
        s_bootInfo.fatalWaitMs = Baleen::Core::kFatalWaitMs;
        s_bootInfo.screenAddress = screen.address;
        s_bootInfo.screenWidth = screen.width;
        s_bootInfo.screenHeight = screen.height;
        s_bootInfo.screenStride = screen.stride;
        s_bootInfo.screenBytes = screen.bytes;
        s_bootInfo.screenKind = static_cast<uint32_t>(screen.kind);
        s_bootInfo.screenFormat = static_cast<uint32_t>(screen.format);
        s_bootInfo.memoryMap = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s_bootMemoryMap));
        // 内核集身份：选版成功的集标识与试启动身份；选版成功时集一定经过清单验签
        memcpy(s_bootInfo.kernelSetBuildId, selected.buildId, Boot::InfoLayout::kKernelSetBuildIdBytes);
        s_bootInfo.attemptId = selected.attemptId;
        // —— 启动来源：布局描述给出的位置事实，加固件环境自己的介质事实 ——
        s_bootInfo.layoutOffset = LayoutDescriptor::kOffset;
        s_bootInfo.volumeOffset = Get64(s_layoutBlock + LayoutDescriptor::kFieldVolumeOffset);
        s_bootInfo.volumeBytes = Get64(s_layoutBlock + LayoutDescriptor::kFieldVolumeBytes);
        s_bootInfo.stateOffset = Get64(s_layoutBlock + LayoutDescriptor::kFieldStateOffset);
        s_bootInfo.stateBytes = Get32(s_layoutBlock + LayoutDescriptor::kFieldStateBytes);
        // 交权块没有介质容量这个事实：如实填 0（未提供），不按 LBA 上限猜
        s_bootInfo.mediumBytes = 0;
        s_bootInfo.kernelPathBytes = Get16(s_layoutBlock + LayoutDescriptor::kFieldKernelPathBytes);
        s_bootInfo.kernelPath = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s_kernelPath));
        for (uint32_t i = 0; i < Boot::InfoLayout::kVolumeUuidBytes; ++i)
            s_bootInfo.volumeUuid[i] = s_layoutBlock[LayoutDescriptor::kFieldVolumeUuid + i];
        // 设备线索：IPL 交权时的驱动器号，只对固件环境有意义，给内核用于诊断与人工定位
        s_bootInfo.devicePath = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&s_biosDrive));
        s_bootInfo.devicePathBytes = 1;
        s_bootInfo.deviceKind = static_cast<uint32_t>(Boot::DeviceKind::BiosDrive);
        // 引导器自身占用区间：内核不得把 BaleenCore 的残留当可用内存
        s_bootInfo.loaderBegin = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__image_start));
        s_bootInfo.loaderBytes = static_cast<uint64_t>(__bss_end - __image_start);
        // 引导闭包交接包：清单副本、列表副本与预装记录数组都由预装步骤给出，
        // 三者都是验签/摘要通过的那一份字节或指向实际装载的模块字节
        s_bootInfo.kernelSetManifest = preloads.manifestPhys;
        s_bootInfo.bootList = preloads.listPhys;
        s_bootInfo.bootModules = preloads.recordsPhys;
        s_bootInfo.kernelSetManifestBytes = preloads.manifestBytes;
        s_bootInfo.bootListBytes = preloads.listBytes;
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
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Nx=");
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
    // 写能力：引导状态区的写入经 Stub 的写盘服务执行；光盘等只读介质不绑定写服务——
    // 交权块里的写服务入口存在不代表介质可写，直接绑定会让 Writable() 谎报可写，
    // 选择器在空状态/回退路径上白试一次写并报 E-CORE-70；只读介质按“无记录”直接启动
    if (handoff->media != static_cast<uint32_t>(Boot::Media::Cdrom)) s_medium.BindWrite(handoff->writeSectors);
    if (s_medium.BlockBytes() != handoff->sectorBytes || s_medium.BlockBytes() == 0) Fail(kFailBindDisk);
    if (const Storage::Error error = s_medium.Read(Baleen::LayoutDescriptor::kOffset, s_layoutBlock, sizeof(s_layoutBlock)); error != Storage::Error::Ok)
        Fail(kFailReadLayout, Storage::ErrorMessage(error));
    // 刷新能力：先看平台有没有可刷新的端口，再用"同一段字节经两种路径读出的内容"核验覆盖关系
    // 比对基准取**第 0 号块**而不是布局描述块：同一批载荷装出来的光盘与硬盘，布局描述块逐字节相同，
    // 拿它比对会把"刷别人"判成"刷自己"；第 0 号块带分区表或 ISO 结构，两者必然不同
    // 只在有端口时做这次核验（读一个空闲命令槽、只读不写）；核验不成立就按未覆盖处理
    // 第 3 个参数既是比较长度，也是 AHCI 的 LBA 与扇区计数单位：ATA 按逻辑扇区计，
    // 4Kn 介质上一次读 4096 字节只有 1 个逻辑扇区（换算在 DiskFlush 与 AhciCommand 里）
    if (Baleen::Core::DiskFlush::Probe() != Baleen::Core::DiskFlush::Path::None &&
        s_medium.Read(0, s_firstBlock, s_medium.BlockBytes()) == Storage::Error::Ok) {
        s_flushCovers = Baleen::Core::DiskFlush::CoversMedium(0, s_firstBlock, s_medium.BlockBytes());
    }
    s_medium.BindFlush(&FlushMedium, s_flushCovers);
    if (const Baleen::Diagnostic reason = CheckLayoutBlock(s_layoutBlock); reason.code != nullptr) Fail(reason);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The layout descriptor is valid");
    const uint64_t volumeOffset = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeOffset);
    const uint64_t volumeBytes = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeBytes);
    const uint64_t stateOffset = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldStateOffset);
    const uint32_t stateBytes = Get32(s_layoutBlock + Baleen::LayoutDescriptor::kFieldStateBytes);
    const uint32_t kernelPathBytes = Get16(s_layoutBlock + Baleen::LayoutDescriptor::kFieldKernelPathBytes);
    memcpy(s_kernelPath, s_layoutBlock + Baleen::LayoutDescriptor::kFieldKernelPath, kernelPathBytes);
    // 设备线索：IPL 交权时的驱动器号，随 BootInfo 交给内核用于诊断与人工定位
    s_biosDrive = static_cast<uint8_t>(handoff->drive);
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Volume=");
    Print::WriteHex64(volumeOffset);
    Print::Write(" Bytes=");
    Print::WriteHex64(volumeBytes);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("State=");
    Print::WriteHex64(stateOffset);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Kernel=");
    Print::Write(s_kernelPath);
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

    // —— 写缓存刷新能力：交权块没有刷新原语，写缓存由本阶段直接刷新（AHCI 优先，传统 IDE 兜底）——
    // 没有可刷新的端口是正常形态（光盘、USB、NVMe），不是失败；有端口时给出路径与结论
    // 有端口还不够：还要核验那个端口上挂的是不是启动介质——刷别人不算持久（s_flushCovers）
    {
        const Baleen::Core::DiskFlush::Path path = Baleen::Core::DiskFlush::Probe();
        const bool flushable = path != Baleen::Core::DiskFlush::Path::None;
        Baleen::PrintTargets::WriteLine(flushable ? Baleen::PrintTargets::Tag::Ok : Baleen::PrintTargets::Tag::Info, flushable ? "The write cache can be flushed on this medium" : "No write cache flush primitive is available on this medium");
        if (flushable) {
            Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
            Print::Write("Flush=");
            Print::Write(Baleen::Core::DiskFlush::PathText(path));
            Print::Write("\r\n");
        }
        // 覆盖结论单独一行：只有它成立，提交才敢声称持久；不成立时状态写入会报 E-CORE-71
        if (flushable && !s_flushCovers)
            Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "The flushable port isn't this medium's device; the boot state write has no durable guarantee");
    }

    // —— 选版（第一阶段）：读引导状态记录，定下目标集并验证清单与签名 ——
    // 选版落实引导状态记录的状态机与写入边界，并验证清单与信任锚；
    // 只读介质与写失败按降级路径启动缺省集，
    // 缺省集与其它集一样先验证清单与签名（没有未验证的直启回退）
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Selecting the kernel set");
    if (const Baleen::Diagnostic reason = Baleen::Core::BeginSetSelection(s_ext4, s_medium, stateOffset, stateBytes, s_kernelPath, Get32(s_layoutBlock + Baleen::LayoutDescriptor::kFieldFlags), s_selected); reason.code != nullptr)
        Fail(reason);
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The kernel set is verified");
    ReportSelectedSet();

    // —— 内核镜像与引导闭包：装载或预装失败都按两阶段选版回退 ——
    // 候选集失败时由选择器写 Failed 并改选已知良好集或上一修订；已知良好集（含缺省集）失败终止
    // 只有内核镜像、清单副本、列表副本、预装记录与模块字节全部就绪才往下走，不交出残缺的交接现场
    Baleen::Core::KernelFacts kernelFacts;
    for (;;) {
        // —— 内核镜像：按选中集的路径定位、读取并校验数据与整幅摘要 ——
        Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Locating and checking the kernel image");
        bool kernelMissing = false;
        const Baleen::Diagnostic kernelReason = Baleen::Core::CheckKernelImage(s_ext4, s_selected.kernelPath, s_selected.buildId, handoff->memoryMap, handoff->memoryMapCount, reinterpret_cast<uintptr_t>(__image_start), static_cast<uint64_t>(__bss_end - __image_start), kernelFacts, kernelMissing);
        Baleen::Diagnostic failure{};
        bool preloadFailed = false;
        if (kernelMissing) {
            Baleen::WriteDiagnostic(Baleen::PrintTargets::Tag::Warn, kWarnKernelMissing);
            failure = kWarnKernelMissing;
        } else if (kernelReason.code != nullptr) {
            failure = kernelReason;
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

            // —— 装载：整幅读进装载地址，按静态内存跨度清零未落盘尾部 ——
            // 读盘按扇区上取整，末尾填充也被写进内存，清零覆盖文件末尾之后的所有字节
            Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Loading the kernel image");
            if (const Baleen::Diagnostic reason = Baleen::Core::LoadKernelImage(s_ext4, s_selected.kernelPath, kernelFacts.loadAddress, kernelFacts.imageBytes); reason.code != nullptr) {
                failure = reason;
            } else {
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

                // —— 预装：按 Modules.list 把引导闭包读进低 4 GiB 并填记录 ——
                Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Preloading the boot closure");
                const Baleen::Diagnostic preloadReason = PreloadBootClosure(*handoff, kernelFacts);
                if (preloadReason.code == nullptr) {
                    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "The boot closure is preloaded");
                    ReportPreloadSummary();
                    break;
                }
                failure = preloadReason;
                preloadFailed = true;
            }
        }

        // —— 回退：候选集的装载 / 预装失败由选择器改选已知良好集或上一修订 ——
        // 候选集失败是 WARN 加回退；已知良好集（含缺省集）失败原样返回原因，终止启动
        Baleen::Core::SelectedSet fallback;
        const Baleen::Diagnostic next = Baleen::Core::SelectAfterLoadFailure(s_ext4, s_medium, stateOffset, stateBytes, s_kernelPath, Get32(s_layoutBlock + Baleen::LayoutDescriptor::kFieldFlags), s_selected, preloadFailed, failure, fallback);
        if (next.code != nullptr) {
            // 没有内核镜像可交且没有可回退的目标：停在可观测的边界上而不是空转，照常报出耗时
            if (!preloadFailed && kernelMissing && next.code == kWarnKernelMissing.code) {
                Baleen::WriteDiagnostic(Baleen::PrintTargets::Tag::Warn, kWarnNoKernel);
                ReportBootTime(*handoff);
                Stop();
            }
            Fail(next);
        }
        s_selected = fallback;
        Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Warn, "The selected set failed to load; falling back to the recorded set");
        ReportSelectedSet();
    }

    // —— 交接状态：BootInfo 与恒等映射页表，都在本阶段的静态存储里 ——
    // 屏幕事实在这里采集：只读 BDA，不成立时给 None，不影响启动判定
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Info, "Building the handover state");
    const Baleen::Core::ScreenFacts screen = Baleen::Core::CollectScreenFacts();
    BuildBootInfo(*handoff, s_selected, s_preloadResult, screen);
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
    // 启动来源：内核与运行期更新据此知道“从哪个卷、哪条路径起来的”
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Boot=Volume=");
    Print::WriteHex64(s_bootInfo.volumeOffset);
    Print::Write(" Bytes=");
    Print::WriteHex64(s_bootInfo.volumeBytes);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("State=");
    Print::WriteHex64(s_bootInfo.stateOffset);
    Print::Write(" Kind=");
    Print::WriteHex(s_bootInfo.deviceKind);
    Print::Write(" Drive=");
    Print::WriteHex(s_biosDrive);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Path=");
    Print::Write(s_kernelPath);
    Print::Write("\r\n");
    // 引导闭包交接包与屏幕事实：内核入口按这些地址读三类缓冲，屏幕只报形态与等待配置
    // 地址都在低 4GiB，长度与记录数按预装结果给出；屏幕事实不成立时 Kind=0
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Closure=Manifest=");
    Print::WriteHex(static_cast<uint32_t>(s_bootInfo.kernelSetManifest));
    Print::Write("/");
    Print::WriteHex(s_bootInfo.kernelSetManifestBytes);
    Print::Write(" List=");
    Print::WriteHex(static_cast<uint32_t>(s_bootInfo.bootList));
    Print::Write("/");
    Print::WriteHex(s_bootInfo.bootListBytes);
    Print::Write(" Records=");
    Print::WriteHex(static_cast<uint32_t>(s_bootInfo.bootModules));
    Print::Write(" Count=");
    WriteDecimal(s_preloadResult.recordCount);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Baleen::PrintTargets::Tag::Continue);
    Print::Write("Screen=Kind=");
    Print::WriteHex(s_bootInfo.screenKind);
    Print::Write(" Addr=");
    Print::WriteHex(static_cast<uint32_t>(s_bootInfo.screenAddress));
    Print::Write(" Geo=");
    Print::WriteHex(s_bootInfo.screenWidth);
    Print::Write("x");
    Print::WriteHex(s_bootInfo.screenHeight);
    Print::Write(" Stride=");
    Print::WriteHex(s_bootInfo.screenStride);
    Print::Write(" Bytes=");
    Print::WriteHex(s_bootInfo.screenBytes);
    Print::Write(" Format=");
    Print::WriteHex(s_bootInfo.screenFormat);
    Print::Write(" WaitMs=");
    Print::WriteHex(s_bootInfo.fatalWaitMs);
    Print::Write("\r\n");

    // 引导耗时：从 Stub 进入起算到交接完成的总耗时与本阶段耗时，预算判定同行的 Verdict 给出
    // 报在装载与交接状态之后：装载是读盘的主要项，预算终点按契约取内核入口
    ReportBootTime(*handoff);

#if FAULT_INJECT
    // 测试构建：在交给内核之前弄坏一处交接事实（发布构建没有这段）
    ApplyFaultInjection();
#endif

    // —— 进入内核：切长模式与恒等映射分页，跳到装载地址加入口偏移 ——
    // 交权后 BaleenCore 冻结常驻；内核会重建自己的段表、中断表与页表，并保留这份 BootInfo
    Baleen::PrintTargets::WriteLine(Baleen::PrintTargets::Tag::Ok, "Entering the kernel");
    _Core_Enter_Kernel(kernelFacts.loadAddress + kernelFacts.entryOffset, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_bootInfo)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_pml4)));
    Stop();
}
