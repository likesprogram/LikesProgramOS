/* Core.cpp
    内核侧 Core 的主流程：装配自己的早期控制台，核对内核镜像数据、BootInfo 与启动来源，
    让固件启动服务失效，完成平台初始化与内存层，核对内核集交接包并把预装字节接管到内核自有内存，
    最后停在明确的阶段边界上

    按与 BaleenCore 相同的分工：本文件承担全部启动流程与全部诊断输出——步骤顺序、
    进度行、失败原因与停机都集中在这里；Console、Tables、Paging、Memory 与 Handoff 只提供机制，
    不打印也不停机，失败只回报编号与原因文本。失败行与启动来源不一致的 WARN 行带 E-KRN 编号：
    编号与文本的单点定义在 Diagnostics.hpp
    FATAL 路径统一为：报编号 → 按 fatalWaitMs 等待（默认 10 秒）→ 请求平台复位 → 复位不生效则停机
*/

#include <stdint.h>

#include <BootInfo.hpp>
#include <KernelImage.hpp>
#include <Manifest.hpp>
#include <Sha256.hpp>

#include <Contract.hpp>
#include <Cpu.hpp>
#include <DependencyGraph.hpp>
#include <Diagnostics.hpp>
#include <Handoff.hpp>
#include <Libc.hpp>
#include <Memory.hpp>
#include <ModuleContainer.hpp>
#include <ModuleLoader.hpp>
#include <ModuleRegistry.hpp>
#include <Paging.hpp>
#include <Reset.hpp>
#include <Tables.hpp>
#include <Tags.hpp>

#include <Print.hpp>

// 诊断编号常量集中在 Diagnostics.hpp，本文件的失败点用短名引用
namespace Diagnostics = Kernel::Diagnostics;

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
    // 交接事实指针：Fail 路径按 fatalWaitMs 决定等待时长，BootInfo 校验通过后常驻
    const Boot::Info* s_info = nullptr;
    // 引导闭包装载：接口注册表、模块表与装载平台适配器；都是静态存储，不做动态分配
    Kernel::InterfaceRegistry s_registry;
    Kernel::ModuleRegistry s_modules;
    Kernel::KernelLoaderPlatform s_loaderPlatform;
    // 装载图与容器视图：容器视图约 9.6 KiB，内核栈只有 16 KiB，只能放静态存储
    Kernel::ContainerView s_closureView;
    Kernel::Dependency::ModuleNode s_closureNodes[Kernel::kHandoffMaxRecords];
    Kernel::Dependency::DependencyEntry s_closureDependencies[Kernel::kHandoffMaxRecords][Kernel::Dependency::kMaxDependencies];
    uint32_t s_closureRecords[Kernel::kHandoffMaxRecords] = {};
    // 系统卷挂载与清单读取：清单只读前 32 字节（头部与条目区开头），缓冲放静态存储；
    // 清单路径按内核路径上限加 "/Manifest.mft" 与结尾零，由集根推导后写入
    constexpr uint32_t kManifestPrefixBytes = 32;
    uint8_t s_manifestBytes[kManifestPrefixBytes];
    char s_manifestPath[Boot::kKernelPathMaxBytes + 1];
    // 运行期模块：Modules.list 里不属于引导闭包期望集的条目。集根由 BuildManifestPath 推出并留在这里，
    // 卷内路径 = 集根 + "/" + 清单条目路径；读缓冲按运行期模块上限给 256 KiB，静态存储（内核栈只有 16 KiB）
    constexpr uint32_t kRuntimeModuleMaxBytes = 256 * 1024;
    constexpr uint32_t kRuntimeModuleMaxCount = 16;
    // 设备枚举扫描上限：只防实现缺陷下的死循环（QEMU 的 PCI 设备远小于它）
    constexpr uint32_t kDeviceScanLimit = 256;
    // 夹具约定：Modules/ 下直接子项是必需运行期模块，Modules/Optional/ 子目录下的是可选运行期模块
    // 容器与清单条目都不带必需/可选标志位（清单只有 Type=2 的 .osm），正式清单 Type 语义留后续阶段
    constexpr char kOptionalRuntimePrefix[] = "Modules/Optional/";
    char s_setRootPath[Boot::kKernelPathMaxBytes + 1];
    uint32_t s_setRootBytes = 0;
    char s_runtimePath[Boot::kKernelPathMaxBytes * 2 + 2];
    uint8_t s_runtimeBytes[kRuntimeModuleMaxBytes];
    // ISO9660 第二来源核对：光盘档读一个 2048 字节扇区（PVD 或根目录）用
    constexpr uint32_t kIsoSectorBytes = 2048;
    uint8_t s_isoSector[kIsoSectorBytes];

    // 停机并停留，供自动化读取诊断；失败路径与阶段边界共用
    [[noreturn]] void Stop() {
        for (;;) asm volatile("CLI; HLT");
    }

    // 打印带编号的 FATAL，按 fatalWaitMs 等待（0 = 默认 10 秒，超上限也按默认），再请求平台复位
    // 等待只做了有界退避时报 WARN（E-KRN-90）；复位没生效时报 FATAL（E-KRN-91）后停机保留诊断
    [[noreturn]] void Fail(const Diagnostics::Diagnostic& diagnostic, const char* detail = nullptr) {
        Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Fatal, diagnostic, detail);
        const uint32_t waitMs = Kernel::Reset::FatalWaitMs(s_info != nullptr ? s_info->fatalWaitMs : 0);
        if (!_Kernel_WaitMs(waitMs)) Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Kernel::Reset::kWarnWaitFallback);
        if (!_Kernel_Reset()) Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Fatal, Kernel::Reset::kFailReset);
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

    // 打印十进制无符号数：清单读取标记按约定写 bytes=<十进制>，不是十六进制
    void WriteDecimal(uint32_t value) {
        char text[10];
        uint32_t length = 0;
        do {
            text[length++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (length > 0) Print::PutChar(text[--length]);
    }

    // 核对启动来源字段之间的一致性：任何一条不成立只报 WARN，该项按“未提供”处理，不判死
    // 第三方引导器可能给不全同一份 BootInfo，因此每条都以字段自身非 0 为前提；
    // 全部一致返回 true，出现不一致返回 false，调用处据此决定是否报 OK
    bool CheckBootSource(const Boot::Info* info) {
        bool consistent = true;
        // 内核路径：给了长度就必须给出可读的地址，且长度不超上限
        if (info->kernelPathBytes != 0 && (info->kernelPathBytes > Boot::kKernelPathMaxBytes || !IsMappedRange(info->kernelPath, info->kernelPathBytes))) {
            consistent = false;
            Diagnostics::WriteDiagnosticPrefix(Kernel::Tags::Tag::Warn, Diagnostics::kWarnBootInfoKernelPath);
            Print::Write(": Bytes=");
            Print::WriteHex(info->kernelPathBytes);
            Print::Write(" Path=");
            Print::WriteHex64(info->kernelPath);
            Print::Write("\r\n");
        }
        // 设备线索：给了长度就必须给出可读的地址，且长度不超契约上限；只要求要读的前若干字节可读
        if (info->devicePathBytes != 0 && (info->devicePathBytes > Boot::kDeviceClueMaxBytes || info->devicePath == 0 || !IsMappedRange(info->devicePath, ClueBytes(info)))) {
            consistent = false;
            Diagnostics::WriteDiagnosticPrefix(Kernel::Tags::Tag::Warn, Diagnostics::kWarnBootInfoDevicePath);
            Print::Write(": Bytes=");
            Print::WriteHex(info->devicePathBytes);
            Print::Write(" Path=");
            Print::WriteHex64(info->devicePath);
            Print::Write("\r\n");
        }
        // 系统卷：位置与长度要么都给，要么都不给；只给一个是不一致（0 表示未提供）
        if ((info->volumeBytes != 0) != (info->volumeOffset != 0)) {
            consistent = false;
            Diagnostics::WriteDiagnosticPrefix(Kernel::Tags::Tag::Warn, Diagnostics::kWarnBootInfoVolume);
            Print::Write(": Layout=");
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
            Diagnostics::WriteDiagnosticPrefix(Kernel::Tags::Tag::Warn, Diagnostics::kWarnBootInfoStateArea);
            Print::Write(": Offset=");
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
    // 字段按用途分组分行：位置事实、介质与占用区间、卷标识、线索与路径各自成行
    // 路径本身长度不定，单独占一行
    void ReportBootSource(const Boot::Info* info) {
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Boot=Kind=");
        Print::WriteHex(info->deviceKind);
        Print::Write(" Layout=");
        Print::WriteHex64(info->layoutOffset);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Volume=");
        Print::WriteHex64(info->volumeOffset);
        Print::Write("/");
        Print::WriteHex64(info->volumeBytes);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("State=");
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
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Uuid=");
        for (uint32_t i = 0; i < Boot::InfoLayout::kVolumeUuidBytes; ++i) WriteByteHex(info->volumeUuid[i]);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("ClueBytes=");
        Print::WriteHex(info->devicePathBytes);
        Print::Write(" Device=");
        ReportDeviceClue(info);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Path=");
        ReportKernelPath(info);
        Print::Write("\r\n");
    }

    // 播报交接包事实：三类缓冲区间、记录数与模块字节合计，以及集身份与修订
    // 构建标识固定 64 个十六进制字符，单独占一行；其余字段按用途分组
    void ReportHandoffFacts(const Kernel::HandoffFacts& handoff) {
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Handoff=Manifest=");
        Print::WriteHex64(handoff.manifestRange.phys);
        Print::Write("/");
        Print::WriteHex(handoff.manifestRange.bytes);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("List=");
        Print::WriteHex64(handoff.listRange.phys);
        Print::Write("/");
        Print::WriteHex(handoff.listRange.bytes);
        Print::Write(" Records=");
        Print::WriteHex(handoff.recordCount);
        Print::Write(" Bytes=");
        Print::WriteHex(static_cast<uint32_t>(handoff.moduleBytes));
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Set=");
        for (uint32_t i = 0; i < Boot::InfoLayout::kKernelSetBuildIdBytes; ++i) WriteByteHex(handoff.buildId[i]);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Major=");
        Print::WriteHex(handoff.moduleMajor);
        Print::Write(" Rev=");
        Print::WriteHex64(handoff.moduleRevision);
        Print::Write(" Entries=");
        Print::WriteHex(handoff.manifest.entryCount);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Modules=");
        Print::WriteHex(handoff.list.count);
        Print::Write(" Closure=");
        Print::WriteHex(handoff.closureCount);
        Print::Write(" Runtime=");
        Print::WriteHex(handoff.runtimeCount);
        Print::Write("\r\n");
    }

    // 保留一段交接区间：按页边界取整后交给分配器保留，已占用视为成功
    // 引导器常驻区（BaleenCore 映像）里的缓冲在 Initialize 时已被标占，Occupied 正说明这一点
    bool ReserveRange(const Kernel::HandoffRange& range) {
        if (range.phys == 0 || range.bytes == 0) return false;
        const uint64_t begin = range.phys & ~(Kernel::kPageBytes - 1);
        const uint64_t end = (range.phys + range.bytes + Kernel::kPageBytes - 1) & ~(Kernel::kPageBytes - 1);
        const Kernel::ReserveResult result = s_pages.Reserve(begin, end - begin);
        return result == Kernel::ReserveResult::Ok || result == Kernel::ReserveResult::Occupied;
    }

    // 标占三类缓冲与每个模块字节：首次 Allocate 之前完成，分配器此后不会把这些页交给别的用途
    bool ReserveHandoff(const Kernel::HandoffFacts& handoff) {
        if (!ReserveRange(handoff.manifestRange) || !ReserveRange(handoff.listRange) || !ReserveRange(handoff.recordsRange)) return false;
        for (uint32_t i = 0; i < handoff.recordCount; ++i) {
            if (!ReserveRange(handoff.moduleRanges[i])) return false;
        }
        return true;
    }

    // 复制接管：为一段交接区间分配内核自有页帧并整段拷贝，交出副本区间
    // 复制而不是接管：闭包合计约 160 KiB，峰值内存翻倍可控
    bool CopyRange(const Kernel::HandoffRange& range, Kernel::HandoffRange& copy) {
        const uint64_t pages = (static_cast<uint64_t>(range.bytes) + Kernel::kPageBytes - 1) / Kernel::kPageBytes;
        const uint64_t dest = s_pages.Allocate(static_cast<uint32_t>(pages));
        if (dest == 0) return false;
        memcpy(Kernel::PhysToVirt(dest), Kernel::PhysToVirt(range.phys), range.bytes);
        copy.phys = dest;
        copy.bytes = range.bytes;
        return true;
    }

    // 把三类缓冲与模块字节复制到内核自有 4 KiB 帧，并把清单 / 列表视图重新指向副本
    // 副本与交接字节逐字节相同，解析结果一致；失败返回 false，调用方按交接包不可用停机
    bool TakeOverHandoff(Kernel::HandoffFacts& handoff) {
        if (!CopyRange(handoff.manifestRange, handoff.manifestCopy)) return false;
        if (!CopyRange(handoff.listRange, handoff.listCopy)) return false;
        if (!CopyRange(handoff.recordsRange, handoff.recordsCopy)) return false;
        for (uint32_t i = 0; i < handoff.recordCount; ++i) {
            if (!CopyRange(handoff.moduleRanges[i], handoff.moduleCopies[i])) return false;
        }
        const uint8_t* const manifest = static_cast<const uint8_t*>(Kernel::PhysToVirt(handoff.manifestCopy.phys));
        const uint8_t* const list = static_cast<const uint8_t*>(Kernel::PhysToVirt(handoff.listCopy.phys));
        if (Kernel::ParseManifest(manifest, handoff.manifestCopy.bytes, handoff.manifest) != nullptr) return false;
        // 列表重解析会把分类结果清零：先把 9 步核对得到的清单下标与闭包/运行期分类保存下来，
        // 重解析后恢复；路径视图指向新副本，分类只依赖路径集合，因此可以原样搬过去
        uint16_t artifactIndex[Kernel::kHandoffMaxRecords] = {};
        uint16_t closureMember[Kernel::kHandoffMaxRecords] = {};
        for (uint32_t index = 0; index < handoff.list.count && index < Kernel::kHandoffMaxRecords; ++index) {
            artifactIndex[index] = handoff.list.artifactIndex[index];
            closureMember[index] = handoff.list.closureMember[index];
        }
        if (Kernel::ParseBootList(list, handoff.listCopy.bytes, handoff.list) != nullptr) return false;
        for (uint32_t index = 0; index < handoff.list.count && index < Kernel::kHandoffMaxRecords; ++index) {
            handoff.list.artifactIndex[index] = artifactIndex[index];
            handoff.list.closureMember[index] = closureMember[index];
        }
        return true;
    }

    // 播报接管结果：三类副本的物理地址与已复制的模块数
    void ReportHandoffCopies(const Kernel::HandoffFacts& handoff) {
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Copies=Manifest=");
        Print::WriteHex64(handoff.manifestCopy.phys);
        Print::Write(" List=");
        Print::WriteHex64(handoff.listCopy.phys);
        Print::Write("\r\n");

        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Records=");
        Print::WriteHex64(handoff.recordsCopy.phys);
        Print::Write(" Modules=");
        Print::WriteHex(handoff.recordCount);
        Print::Write("\r\n");
    }

    // 内核核心导出：绑定内核核心服务后登记 Core.* 十张槽表，并运行时核对 Core.Abi
    // 失败按 E-KRN-59 停机（编号与 detail 由 InitializeCoreExports 给出）
    void RegisterCoreExports(const Boot::Info* info) {
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Registering the kernel core exports");
        s_loaderPlatform.Bind(&s_pages);
        const char* reason = nullptr;
        if (const Diagnostics::Diagnostic diagnostic = Kernel::InitializeCoreExports(s_registry, &s_pages, info, reason); diagnostic.code != nullptr) Fail(diagnostic, reason);
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The kernel core exports are registered");
    }

    // 绑定状态约定（契约 v1 只给“状态位”字段，Dev.Model 与 DriverLoader 约定取值）：
    // 1 = 已绑定待启动，2 = 已启动（Started 之后 BindingFacts 的 ops 与 context 才有效）
    constexpr uint32_t kBindingStateBound = 1;

    // E-KRN-62 的 detail：绑定句柄 + 设备句柄 + Start 失败码，直接定位是哪条绑定起不来
    // 契约 v1 的 DriverDesc 没有模块标识字段，BindingFacts.driverModule 目前恒为零，所以不拿它当标识
    uint32_t AppendHexValue(char* text, uint32_t length, uint32_t value) {
        text[length++] = '0';
        text[length++] = 'x';
        constexpr char kDigits[] = "0123456789ABCDEF";
        for (uint32_t index = 0; index < 8; ++index) text[length++] = kDigits[(value >> (28 - 4 * index)) & 0xF];
        return length;
    }

    const char* BindingStartDetail(const BindingFacts& binding, KernelError error) {
        static char detail[96];
        uint32_t length = 0;
        constexpr char kBind[] = "bind=";
        for (const char* text = kBind; *text != '\0'; ++text) detail[length++] = *text;
        length = AppendHexValue(detail, length, binding.bindHandle);
        constexpr char kDevice[] = " device=";
        for (const char* text = kDevice; *text != '\0'; ++text) detail[length++] = *text;
        length = AppendHexValue(detail, length, binding.deviceHandle);
        constexpr char kMiddle[] = " start=";
        for (const char* text = kMiddle; *text != '\0'; ++text) detail[length++] = *text;
        for (const char* text = KernelErrorName(error); *text != '\0'; ++text) detail[length++] = *text;
        detail[length] = '\0';
        return detail;
    }

    // 驱动启动编排钩子：驱动 Init 里 RegisterDriver + Bind 之后、
    // Vfs Init 之前必须把绑定启动起来——BindingFacts 的 ops 与 context 只在 Start 成功后才有效
    // 每次模块 Init 成功调用一次：Dev.Model 与 Drv.Loader 尚未登记时静默返回（闭包前半段不是失败）
    // 设备按 cursor 从 0 枚举、kNotFound 结束；BindingFacts 零初始化即查询模式（bytes==0），
    // 未绑定返回 kNotFound；返回的 state 约定 1=Bound、2=Started，Started 的绑定不再调 Start
    // Start 失败按 E-KRN-62 停机（启动盘驱动不可用就别假装能挂卷），detail 带驱动模块标识
    void StartPendingBindings() {
        const Kernel::RegisteredExport* const deviceExport = s_registry.Find(kIfDevModel, 1);
        const Kernel::RegisteredExport* const driverExport = s_registry.Find(kIfDrvLoader, 1);
        if (deviceExport == nullptr || driverExport == nullptr) return;
        if (deviceExport->entry.slotCount != kDevModelSlotCountV1 || driverExport->entry.slotCount != kDrvLoaderSlotCountV1) return;
        if (deviceExport->entry.slots == nullptr || driverExport->entry.slots == nullptr) return;
        const DevModelSlotsV1* const device = static_cast<const DevModelSlotsV1*>(deviceExport->entry.slots);
        const DrvLoaderSlotsV1* const driver = static_cast<const DrvLoaderSlotsV1*>(driverExport->entry.slots);
        uint32_t cursor = 0;
        // 枚举上界 256 只防实现缺陷下的死循环；正常设备数远小于它
        for (uint32_t poll = 0; poll < 256; ++poll) {
            DeviceFacts facts{};
            const KernelError enumError = device->Enumerate(&cursor, &facts);
            if (enumError != KernelError::kOk) return;
            BindingFacts binding{};
            if (device->Binding(facts.deviceHandle, &binding) != KernelError::kOk) continue;
            if (binding.bindHandle == 0 || binding.state != kBindingStateBound) continue;
            if (const KernelError startError = driver->Start(binding.bindHandle); startError != KernelError::kOk)
                Fail(Diagnostics::kFailBootDeviceDriver, BindingStartDetail(binding, startError));
        }
    }

    // USB 枚举推进钩子：UsbCore 的 Usb.Host v1 导出出现后，每个模块 Init 成功时推进枚举与类绑定
    // 最多 16 轮、每轮预算 64；没有导出或 Poll 返回失败都直接停（HDD / ISO 档没有 USB 设备，不算错误）
    // 目的是在 Vfs Init（预装序 10）之前完成 USB 枚举、Dev.Model v2 登记与 Binding(Started) 写入
    void AdvanceUsbHost() {
        const Kernel::RegisteredExport* const exportEntry = s_registry.Find(kIfUsbHost, 1);
        if (exportEntry == nullptr || exportEntry->entry.slotCount != kUsbHostSlotCountV1 || exportEntry->entry.slots == nullptr) return;
        if (exportEntry->entry.slots == nullptr) return;
        const UsbHostSlotsV1* const host = static_cast<const UsbHostSlotsV1*>(exportEntry->entry.slots);
        if (host->Poll == nullptr) return;
        constexpr uint32_t kPollRounds = 16;
        constexpr uint32_t kPollBudget = 64;
        for (uint32_t round = 0; round < kPollRounds; ++round) if (host->Poll(kPollBudget) != KernelError::kOk) return;
    }

    // 装载引导闭包：按闭包成员在 Modules.list 中的顺序填依赖图（容器解析给出标识、版本与依赖表），
    // 用依赖图的拓扑排序
    // 定下顺序后逐条交给装载编排；Init 成功标记由装载器打印，失败按编号停机
    void LoadBootClosure(const Boot::Info* info, Kernel::HandoffFacts& handoff) {
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Loading the boot closure modules");
        Kernel::LoaderContext context;
        context.platform = &s_loaderPlatform;
        context.registry = &s_registry;
        context.modules = &s_modules;
        context.osModuleMajor = handoff.moduleMajor;
        context.bootInfo = info;

        const uint32_t closureCount = handoff.closureCount;
        for (uint32_t order = 0; order < closureCount; ++order) {
            const uint32_t recordIndex = handoff.closureRecordIndex[order];
            const Kernel::HandoffRange& copy = handoff.moduleCopies[recordIndex];
            const uint8_t* const bytes = static_cast<const uint8_t*>(Kernel::PhysToVirt(copy.phys));
            s_closureView = Kernel::ContainerView{};
            if (const Kernel::ContainerError error = Kernel::ParseModuleContainer(bytes, copy.bytes, s_closureView); error != Kernel::ContainerError::None)
                Fail(Diagnostics::kFailOsmContainer, Kernel::ContainerErrorText(error));
            Kernel::Dependency::ModuleNode& node = s_closureNodes[order];
            node = Kernel::Dependency::ModuleNode{};
            node.moduleId = s_closureView.moduleId;
            node.major = s_closureView.versionMajor;
            node.minor = s_closureView.versionMinor;
            node.patch = s_closureView.versionPatch;
            node.preloadOrder = order + 1;
            node.dependencyCount = s_closureView.dependencyCount;
            node.dependencies = s_closureView.dependencyCount != 0 ? s_closureDependencies[order] : nullptr;
            for (uint32_t index = 0; index < s_closureView.dependencyCount; ++index) {
                const Kernel::ContainerDependency& source = s_closureView.dependencies[index];
                Kernel::Dependency::DependencyEntry& target = s_closureDependencies[order][index];
                target = Kernel::Dependency::DependencyEntry{};
                target.moduleId = source.moduleId;
                target.minMajor = source.minMajor;
                target.minMinor = source.minMinor;
                target.minPatch = source.minPatch;
                target.maxMajor = source.maxMajor;
                target.maxMinor = source.maxMinor;
                target.maxPatch = source.maxPatch;
                target.flags = 0;   // 容器依赖表没有 Flags 字段，解析器已校验并丢弃
            }
            s_closureRecords[order] = recordIndex;
        }

        Kernel::Dependency::GraphInput graph;
        graph.modules = s_closureNodes;
        graph.moduleCount = closureCount;
        Kernel::Dependency::TopologyResult topology;
        const char* graphReason = nullptr;
        if (const Diagnostics::Diagnostic diagnostic = Kernel::SortModulesForLoad(graph, topology, graphReason); diagnostic.code != nullptr) Fail(diagnostic, graphReason);

        for (uint32_t index = 0; index < topology.count; ++index) {
            const uint32_t order = topology.index[index];
            const uint32_t recordIndex = s_closureRecords[order];
            const Kernel::HandoffRecord& record = handoff.records[recordIndex];
            Kernel::LoadRequest request;
            request.bytes = static_cast<const uint8_t*>(Kernel::PhysToVirt(handoff.moduleCopies[recordIndex].phys));
            request.bytesCount = handoff.moduleCopies[recordIndex].bytes;
            request.manifestEntry = &handoff.manifest.entries[record.artifactIndex];
            request.handoff = &handoff;
            request.closureModule = true;
            request.preloadOrder = order + 1;
            Kernel::LoadOutcome outcome;
            if (const Diagnostics::Diagnostic diagnostic = Kernel::LoadModule(context, request, outcome); diagnostic.code != nullptr) Fail(diagnostic, outcome.reason);
            // 驱动启动编排：该模块 Init 成功后把已绑定未启动的驱动启动起来，Vfs（预装序 10）Init 前完成
            StartPendingBindings();
            // USB 枚举推进：UsbCore 之后每次 Init 都推一轮，确保 Vfs Init 前 USB 类绑定与设备登记完成
            AdvanceUsbHost();
        }
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The boot closure modules are initialized");
    }

    // 播报已启动的块设备事实（诊断用，不参与判定）：Dev.Model 绑定事实里的 ops 与块大小/容量
    // 挂卷失败时据此区分“设备没起来”与“Boot.Volume 候选判定拒绝”
    void ReportBlockDevices() {
        const Kernel::RegisteredExport* const deviceExport = s_registry.Find(kIfDevModel, 1);
        if (deviceExport == nullptr || deviceExport->entry.slotCount != kDevModelSlotCountV1 || deviceExport->entry.slots == nullptr) return;
        const DevModelSlotsV1* const deviceModel = static_cast<const DevModelSlotsV1*>(deviceExport->entry.slots);
        uint32_t cursor = 0;
        for (uint32_t step = 0; step < kDeviceScanLimit; ++step) {
            DeviceFacts facts{};
            if (deviceModel->Enumerate(&cursor, &facts) != KernelError::kOk) return;
            BindingFacts binding{};
            if (deviceModel->Binding(facts.deviceHandle, &binding) != KernelError::kOk) continue;
            if (binding.ops == nullptr || binding.ops->BlockBytes == nullptr || binding.ops->CapacityBytes == nullptr) continue;
            Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
            Print::Write("BlockDevice handle=");
            Print::WriteHex(facts.deviceHandle);
            Print::Write(" bytes=");
            Print::WriteHex(binding.ops->BlockBytes(binding.context));
            Print::Write("\r\n");
            Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
            Print::Write("capacity=");
            Print::WriteHex64(binding.ops->CapacityBytes(binding.context));
            Print::Write("\r\n");
        }
    }

    // —— 系统卷挂载与清单读取：闭包全部 Init 之后由内核核心调用一次 ——

    // 把失败阶段与 Storage::Error 文本拼成 detail：静态缓冲，Fail 同步写完才进入等待
    const char* StorageDetail(const char* stage, Storage::Error error) {
        static char detail[96];
        uint32_t length = 0;
        for (const char* text = stage; *text != '\0' && length < sizeof(detail) - 2; ++text) detail[length++] = *text;
        detail[length++] = ':';
        detail[length++] = ' ';
        for (const char* text = Storage::ErrorMessage(error); *text != '\0' && length < sizeof(detail) - 1; ++text) detail[length++] = *text;
        detail[length] = '\0';
        return detail;
    }

    // 集根推导：BootInfo.kernelPath 去掉末尾 "/LikesProgram.os"，再拼 "/Manifest.mft"
    // 返回清单路径字节数；路径缺失、不可读、不以固定后缀结尾或超出缓冲时返回 0（不猜路径）
    // 交叉核对：集根最后一段必须是 kernelSetBuildId 的 64 位小写十六进制文本（两种事实必须同批）
    uint32_t BuildManifestPath(const Boot::Info* info) {
        constexpr char kKernelName[] = "/LikesProgram.os";
        constexpr char kManifestName[] = "/Manifest.mft";
        constexpr uint32_t kKernelNameBytes = sizeof(kKernelName) - 1;
        constexpr uint32_t kManifestNameBytes = sizeof(kManifestName) - 1;
        if (info->kernelPathBytes == 0 || info->kernelPathBytes > Boot::kKernelPathMaxBytes) return 0;
        if (!IsMappedRange(info->kernelPath, info->kernelPathBytes)) return 0;
        const char* const kernelPath = static_cast<const char*>(Kernel::PhysToVirt(info->kernelPath));
        if (info->kernelPathBytes <= kKernelNameBytes) return 0;
        const uint32_t rootBytes = info->kernelPathBytes - kKernelNameBytes;
        for (uint32_t index = 0; index < kKernelNameBytes; ++index) if (kernelPath[rootBytes + index] != kKernelName[index]) return 0;
        // 集身份交叉核对：kernelSetBuildId 全零表示未提供，跳过；非零时必须与目录名逐字符相等
        bool buildIdGiven = false;
        for (uint32_t index = 0; index < Boot::InfoLayout::kKernelSetBuildIdBytes; ++index) if (info->kernelSetBuildId[index] != 0) buildIdGiven = true;
        if (buildIdGiven) {
            uint32_t segmentBegin = rootBytes;
            while (segmentBegin > 0 && kernelPath[segmentBegin - 1] != '/') --segmentBegin;
            if (rootBytes - segmentBegin != Boot::InfoLayout::kKernelSetBuildIdBytes * 2) return 0;
            constexpr char kDigits[] = "0123456789abcdef";
            for (uint32_t index = 0; index < Boot::InfoLayout::kKernelSetBuildIdBytes; ++index) {
                const uint8_t byte = info->kernelSetBuildId[index];
                if (kernelPath[segmentBegin + 2 * index] != kDigits[byte >> 4]) return 0;
                if (kernelPath[segmentBegin + 2 * index + 1] != kDigits[byte & 0xF]) return 0;
            }
        }
        if (rootBytes + kManifestNameBytes > sizeof(s_manifestPath) - 1) return 0;
        if (rootBytes > sizeof(s_setRootPath) - 1) return 0;
        for (uint32_t index = 0; index < rootBytes; ++index) s_setRootPath[index] = kernelPath[index];
        s_setRootPath[rootBytes] = '\0';
        s_setRootBytes = rootBytes;
        for (uint32_t index = 0; index < rootBytes; ++index) s_manifestPath[index] = kernelPath[index];
        for (uint32_t index = 0; index < kManifestNameBytes; ++index) s_manifestPath[rootBytes + index] = kManifestName[index];
        s_manifestPath[rootBytes + kManifestNameBytes] = '\0';
        return rootBytes + kManifestNameBytes;
    }

    // 读盘清单前缀与交接副本逐字节比对：只比前 32 字节（头部与条目区开头；签名值在文件尾部，
    // 不参与本次比对），字节相等即摘要相等；不一致时经 offset 返回首个不一致偏移
    bool ManifestPrefixEqual(const Kernel::HandoffFacts& handoff, uint32_t readBytes, uint32_t& offset) {
        offset = 0;
        if (handoff.manifestCopy.phys == 0 || handoff.manifestCopy.bytes < readBytes) return false;
        const uint8_t* const copy = static_cast<const uint8_t*>(Kernel::PhysToVirt(handoff.manifestCopy.phys));
        if (copy == nullptr) return false;
        for (uint32_t index = 0; index < readBytes; ++index) {
            if (s_manifestBytes[index] != copy[index]) { offset = index; return false; }
        }
        return true;
    }

    // E-KRN-76 的 detail：两侧字节数与首个不一致偏移，固定宽度便于 grep 与离线核对
    uint32_t AppendHexField(char* text, uint32_t length, const char* name, uint32_t value) {
        for (const char* cursor = name; *cursor != '\0' && length < 90; ++cursor) text[length++] = *cursor;
        text[length++] = '=';
        text[length++] = '0';
        text[length++] = 'x';
        constexpr char kDigits[] = "0123456789ABCDEF";
        for (uint32_t index = 0; index < 8; ++index) text[length++] = kDigits[(value >> (28 - 4 * index)) & 0xF];
        text[length++] = ' ';
        return length;
    }

    const char* ManifestMismatchDetail(uint32_t readBytes, uint32_t copyBytes, uint32_t offset) {
        static char detail[96];
        uint32_t length = AppendHexField(detail, 0, "read", readBytes);
        length = AppendHexField(detail, length, "copy", copyBytes);
        length = AppendHexField(detail, length, "offset", offset);
        detail[length - 1] = '\0';   // 去掉最后一个字段后的分隔空格
        return detail;
    }

    // 从接口注册表取 Fs.Vfs v1 槽表：槽数必须与生成头一致，槽表由装载器物化成每项一个函数指针的数组
    // 缺失或形状不对按 E-KRN-70 停机；只有 Vfs Init 成功后注册表里才会有它
    const FsVfsSlotsV1* VfsSlots() {
        const Kernel::RegisteredExport* const vfsExport = s_registry.Find(kIfFsVfs, 1);
        if (vfsExport == nullptr || vfsExport->entry.slotCount != kFsVfsSlotCountV1 || vfsExport->entry.slots == nullptr)
            Fail(Diagnostics::kFailVolumeInterface, "the Fs.Vfs v1 export is missing or malformed");
        return static_cast<const FsVfsSlotsV1*>(vfsExport->entry.slots);
    }

    // 挂载系统卷并读清单：从接口注册表取 Fs.Vfs v1 与 Boot.Volume v1 的导出，按槽序强类型调用
    // 存储层只回报 Storage::Error，编号由这里按失败点选（E-KRN-70–76）；失败走 FATAL + 等待 + 复位
    void MountSystemVolume(const Boot::Info* info, const Kernel::HandoffFacts& handoff) {
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Mounting the system volume");
        ReportBlockDevices();
        const FsVfsSlotsV1* const vfs = VfsSlots();
        const Kernel::RegisteredExport* const volumeExport = s_registry.Find(kIfBootVolume, 1);
        if (volumeExport == nullptr || volumeExport->entry.slotCount != kBootVolumeSlotCountV1 || volumeExport->entry.slots == nullptr)
            Fail(Diagnostics::kFailVolumeInterface, "the Boot.Volume v1 export is missing or malformed");
        const BootVolumeSlotsV1* const volume = static_cast<const BootVolumeSlotsV1*>(volumeExport->entry.slots);

        // 挂载：facts 由模块写（首字段长度，v1 为 52 字节），内核不预填；挂载成功标记由
        // Boot.Volume 模块经 Core.Log 打印，内核核心不重复打印，只复核卷标识事实
        SystemVolumeFacts facts{};
        if (const Storage::Error error = volume->MountSystemVolume(info, &facts); error != Storage::Error::Ok)
            Fail(Diagnostics::kFailVolumeMount, StorageDetail("mount", error));
        if (facts.bytes < sizeof(facts)) Fail(Diagnostics::kFailVolumeMount, "the volume facts are shorter than v1");

        // 卷标识复核：BootInfo 提供非全零标识时必须逐字节相等（模块内已核对一次，这里不信任返回值）
        bool bootUuidGiven = false;
        for (uint32_t index = 0; index < Boot::InfoLayout::kVolumeUuidBytes; ++index) if (info->volumeUuid[index] != 0) bootUuidGiven = true;
        if (bootUuidGiven && memcmp(info->volumeUuid, facts.volumeUuid, Boot::InfoLayout::kVolumeUuidBytes) != 0)
            Fail(Diagnostics::kFailVolumeIdentity, "the mounted volume identifier doesn't match the BootInfo");

        // 清单：路径从 BootInfo.kernelPath 推出（集根 + /Manifest.mft），并交叉核对集身份
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Reading the kernel set manifest");
        const uint32_t pathBytes = BuildManifestPath(info);
        if (pathBytes == 0) Fail(Diagnostics::kFailManifestPath, "the BootInfo kernel path doesn't give a kernel set directory");
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
        Print::Write("Manifest=");
        for (uint32_t index = 0; index < pathBytes; ++index) Print::PutChar(s_manifestPath[index]);
        Print::Write("\r\n");

        // Stat → Open → Read：只读前 32 字节；Stat 同时确认不是目录、长度不小于前缀且不超过清单上限
        Storage::FileInfo fileInfo{};
        if (const Storage::Error error = vfs->Stat(s_manifestPath, &fileInfo); error != Storage::Error::Ok)
            Fail(Diagnostics::kFailManifestOpen, StorageDetail("stat", error));
        if (fileInfo.directory || fileInfo.bytes < kManifestPrefixBytes || fileInfo.bytes > KernelSet::ManifestLayout::kManifestMaxBytes)
            Fail(Diagnostics::kFailManifestRead, "the manifest size is out of range");
        uint64_t handle = 0;
        if (const Storage::Error error = vfs->Open(s_manifestPath, &handle); error != Storage::Error::Ok)
            Fail(Diagnostics::kFailManifestOpen, StorageDetail("open", error));
        uint32_t readBytes = 0;
        const Storage::Error readError = vfs->Read(handle, 0, s_manifestBytes, kManifestPrefixBytes, &readBytes);
        (void)vfs->Close(handle);
        if (readError != Storage::Error::Ok) Fail(Diagnostics::kFailManifestRead, StorageDetail("read", readError));
        if (readBytes != kManifestPrefixBytes) Fail(Diagnostics::kFailManifestRead, "the manifest read returned fewer bytes than the prefix");

        // 与交接副本比对：读盘前缀必须就是 Baleen 已验签的那份字节；不一致按 76 停机且不打印成功行
        uint32_t offset = 0;
        if (!ManifestPrefixEqual(handoff, readBytes, offset))
            Fail(Diagnostics::kFailManifestMismatch, ManifestMismatchDetail(readBytes, handoff.manifestCopy.bytes, offset));

        // 摘要行：SHA-256(读到的 32 字节) 的前 8 字节（16 位小写十六进制），bytes 按十进制
        Common::Digest::Sha256 sha;
        sha.Begin();
        sha.Update(s_manifestBytes, readBytes);
        uint8_t digest[Common::Digest::kSha256Bytes] = {};
        sha.Finish(digest);
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Ok);
        Print::Write("system manifest read: bytes=");
        WriteDecimal(readBytes);
        Print::Write(" digest=");
        for (uint32_t index = 0; index < 8; ++index) WriteByteHex(digest[index]);
        Print::Write("\r\n");
    }

    // —— ISO9660 第二来源核对：光盘档从 ISO9660 目录项独立推出系统卷区段 ——

    // 小端读 32 位：ISO9660 的目录项字段是小端
    uint32_t ReadLe32(const uint8_t* data) {
        return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
            | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    }

    // ISO9660 标识符比较：去掉 ";<版本>" 后缀后按 ASCII 大写比较（ISO 标识符为大写 8.3）
    // MakeImage 放进 ISO 的文件名是 SystemVolume.img，ISO 标识符会折叠成 SYSTEMVO.IMG;1
    bool IsoNameIsSystemVolume(const uint8_t* name, uint32_t nameBytes) {
        constexpr char kExpected[] = "SYSTEMVO.IMG";
        constexpr uint32_t kExpectedBytes = sizeof(kExpected) - 1;
        uint32_t end = nameBytes;
        for (uint32_t index = 0; index < nameBytes; ++index) if (name[index] == ';') { end = index; break; }
        if (end != kExpectedBytes) return false;
        for (uint32_t index = 0; index < kExpectedBytes; ++index) {
            uint8_t character = name[index];
            if (character >= 'a' && character <= 'z') character = static_cast<uint8_t>(character - 'a' + 'A');
            if (character != static_cast<uint8_t>(kExpected[index])) return false;
        }
        return true;
    }

    // 在已启动的块设备里找带 ISO9660 卷描述符的一个：读逻辑扇区 16 并核对 "CD001" 魔数
    // 只在光盘介质档调用；USB/HDD 档不读（返回 false 由调用方按“介质不是光盘”跳过）
    bool FindIso9660Device(const BlockDeviceOps*& ops, void*& context) {
        const Kernel::RegisteredExport* const deviceExport = s_registry.Find(kIfDevModel, 1);
        if (deviceExport == nullptr || deviceExport->entry.slotCount != kDevModelSlotCountV1 || deviceExport->entry.slots == nullptr) return false;
        const DevModelSlotsV1* const deviceModel = static_cast<const DevModelSlotsV1*>(deviceExport->entry.slots);
        uint32_t cursor = 0;
        for (uint32_t step = 0; step < kDeviceScanLimit; ++step) {
            DeviceFacts facts{};
            if (deviceModel->Enumerate(&cursor, &facts) != KernelError::kOk) return false;
            BindingFacts binding{};
            if (deviceModel->Binding(facts.deviceHandle, &binding) != KernelError::kOk) continue;
            if (binding.ops == nullptr || binding.ops->Read == nullptr || binding.ops->BlockBytes == nullptr) continue;
            const uint32_t blockBytes = binding.ops->BlockBytes(binding.context);
            if (blockBytes == 0) continue;
            if (binding.ops->Read(binding.context, static_cast<uint64_t>(16) * blockBytes, s_isoSector, kIsoSectorBytes) != Storage::Error::Ok) continue;
            if (memcmp(s_isoSector + 1, "CD001", 5) != 0) continue;
            ops = binding.ops;
            context = binding.context;
            return true;
        }
        return false;
    }

    // E-KRN-77 的 detail：ISO 侧与 BootInfo 侧的偏移/长度，固定宽度便于比对
    const char* IsoSourceMismatchDetail(uint64_t isoOffset, uint32_t isoBytes, const Boot::Info* info) {
        static char detail[96];
        uint32_t length = AppendHexField(detail, 0, "iso", static_cast<uint32_t>(isoOffset));
        length = AppendHexField(detail, length, "boot", static_cast<uint32_t>(info->volumeOffset));
        length = AppendHexField(detail, length, "bytes", isoBytes);
        length = AppendHexField(detail, length, "want", static_cast<uint32_t>(info->volumeBytes));
        detail[length - 1] = '\0';
        return detail;
    }

    // 光盘档的第二来源核对：从 ISO9660 根目录的目录项推出 SystemVolume.img 的区段（LBA × 2048 字节）
    // 与 BootInfo 的 volumeOffset/volumeBytes 比对；一致打稳定标记，不一致按 E-KRN-77 停机
    // 找不到 ISO9660 卷或根目录里没有系统卷文件：只 WARN E-KRN-78，不改变已完成的挂载判定
    void CheckIso9660SecondSource(const Boot::Info* info) {
        if (info->media != static_cast<uint32_t>(Boot::Media::Cdrom)) return;
        const BlockDeviceOps* ops = nullptr;
        void* context = nullptr;
        if (!FindIso9660Device(ops, context)) {
            Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnIsoSourceMissing, "no ISO9660 volume descriptor was readable");
            return;
        }
        // 主卷描述符：根目录记录在偏移 156，目录项里 extent LBA 在 +2、数据长度在 +10（均为小端）
        const uint8_t* const rootRecord = s_isoSector + 156;
        if (rootRecord[0] < 34) {
            Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnIsoSourceMissing, "the ISO9660 root directory record is malformed");
            return;
        }
        const uint32_t rootLba = ReadLe32(rootRecord + 2);
        if (ops->Read(context, static_cast<uint64_t>(rootLba) * kIsoSectorBytes, s_isoSector, kIsoSectorBytes) != Storage::Error::Ok) {
            Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnIsoSourceMissing, "the ISO9660 root directory isn't readable");
            return;
        }
        for (uint32_t position = 0; position + 33 <= kIsoSectorBytes;) {
            const uint32_t recordBytes = s_isoSector[position];
            if (recordBytes == 0) break;   // 目录项到 0 长度为止，后面是填充
            if (recordBytes < 34 || position + recordBytes > kIsoSectorBytes) break;
            const uint32_t nameBytes = s_isoSector[position + 32];
            if (nameBytes != 0 && position + 33 + nameBytes <= kIsoSectorBytes
                && IsoNameIsSystemVolume(s_isoSector + position + 33, nameBytes)) {
                const uint32_t extentLba = ReadLe32(s_isoSector + position + 2);
                const uint32_t dataBytes = ReadLe32(s_isoSector + position + 10);
                const uint64_t offset = static_cast<uint64_t>(extentLba) * kIsoSectorBytes;
                Kernel::Tags::WriteTag(Kernel::Tags::Tag::Ok);
                Print::Write("iso9660: system volume file=SystemVolume.img bytes=");
                WriteDecimal(dataBytes);
                Print::Write(" offset=");
                Print::WriteHex64(offset);
                Print::Write("\r\n");
                if (offset != info->volumeOffset || dataBytes != info->volumeBytes)
                    Fail(Diagnostics::kFailIsoSourceMismatch, IsoSourceMismatchDetail(offset, dataBytes, info));
                return;
            }
            position += recordBytes;
        }
        Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnIsoSourceMissing, "the ISO9660 root has no system volume file");
    }

    // —— 运行期模块装载：挂卷并读过清单之后，按 Modules.list 中非闭包条目逐条装载 ——

    // 运行期模块的集内路径是否合法：必须在 Modules/ 下，且拼出的卷内路径不越界
    // 成功时把卷内路径写进 s_runtimePath 并返回字节数；失败返回 0
    uint32_t BuildRuntimePath(const uint8_t* relative, uint32_t relativeBytes) {
        constexpr char kModulesPrefix[] = "Modules/";
        constexpr uint32_t kModulesPrefixBytes = sizeof(kModulesPrefix) - 1;
        if (relativeBytes <= kModulesPrefixBytes) return 0;
        for (uint32_t index = 0; index < kModulesPrefixBytes; ++index) if (relative[index] != kModulesPrefix[index]) return 0;
        if (s_setRootBytes == 0 || s_setRootBytes + 1 + relativeBytes > sizeof(s_runtimePath) - 1) return 0;
        for (uint32_t index = 0; index < s_setRootBytes; ++index) s_runtimePath[index] = s_setRootPath[index];
        s_runtimePath[s_setRootBytes] = '/';
        for (uint32_t index = 0; index < relativeBytes; ++index) s_runtimePath[s_setRootBytes + 1 + index] = static_cast<char>(relative[index]);
        const uint32_t bytes = s_setRootBytes + 1 + relativeBytes;
        s_runtimePath[bytes] = '\0';
        return bytes;
    }

    // 运行期模块是否可选：夹具约定按集内路径前缀承载（Modules/Optional/ = 可选）
    // 容器与清单条目都没有这个标志位，正式清单 Type 语义留后续阶段 定
    bool IsOptionalRuntime(const uint8_t* relative, uint32_t relativeBytes) {
        constexpr uint32_t kPrefixBytes = sizeof(kOptionalRuntimePrefix) - 1;
        if (relativeBytes < kPrefixBytes) return false;
        for (uint32_t index = 0; index < kPrefixBytes; ++index) if (relative[index] != static_cast<uint8_t>(kOptionalRuntimePrefix[index])) return false;
        return true;
    }

    // 运行期模块失败的 detail：清单条目路径 + ':' + 机制层原因，静态缓冲；detail 进诊断行括号
    const char* RuntimeDetail(const uint8_t* path, uint32_t pathBytes, const char* reason) {
        static char detail[192];
        uint32_t length = 0;
        for (uint32_t index = 0; index < pathBytes && length < sizeof(detail) - 2; ++index) detail[length++] = static_cast<char>(path[index]);
        if (reason != nullptr && reason[0] != '\0') {
            detail[length++] = ':';
            detail[length++] = ' ';
            for (const char* cursor = reason; *cursor != '\0' && length < sizeof(detail) - 1; ++cursor) detail[length++] = *cursor;
        }
        detail[length] = '\0';
        return detail;
    }

    // 运行期模块装载：闭包装载完成、系统卷挂载并读过清单之后调用一次
    // 交接事实里的运行期条目表就是 Modules.list 中不属于引导闭包期望集的条目，按列表顺序装载；
    // 读字节、交装载编排（closureModule=false）
    // 必需模块失败按 E-KRN-80（装载机制）/82（条目非法）/81（Init 失败）走 FATAL；可选模块失败只 WARN E-KRN-83 并继续
    void LoadRuntimeModules(const Boot::Info* info, const Kernel::HandoffFacts& handoff) {
        Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Loading the runtime modules");
        const FsVfsSlotsV1* const vfs = VfsSlots();
        Kernel::LoaderContext context;
        context.platform = &s_loaderPlatform;
        context.registry = &s_registry;
        context.modules = &s_modules;
        context.osModuleMajor = handoff.moduleMajor;
        context.bootInfo = info;
        uint32_t runtimeCount = 0;
        uint32_t loaded = 0;
        uint32_t failed = 0;
        uint32_t optionalCount = 0;
        for (uint32_t runtimeIndex = 0; runtimeIndex < handoff.runtimeCount; ++runtimeIndex) {
            const uint32_t listIndex = handoff.runtimeListIndex[runtimeIndex];
            const Kernel::ManifestEntry& entry = handoff.manifest.entries[handoff.list.artifactIndex[listIndex]];
            const bool optional = IsOptionalRuntime(entry.path, entry.pathBytes);
            if (optional) ++optionalCount;
            // 条目上限与路径合法性：唯一的运行期条目上限是静态缓冲与句柄表，不静默截断
            if (runtimeCount >= kRuntimeModuleMaxCount || BuildRuntimePath(entry.path, entry.pathBytes) == 0) {
                const char* const detail = RuntimeDetail(entry.path, entry.pathBytes, "the runtime module path or count is out of range");
                if (!optional) Fail(Diagnostics::kFailRuntimeEntry, detail);
                Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnRuntimeOptional, detail);
                ++failed;
                continue;
            }
            ++runtimeCount;
            // 读整份字节：Stat 先核对目录标志与长度（与清单登记一致、不超内核缓冲），再 Open/Read
            Storage::FileInfo fileInfo{};
            const Storage::Error statError = vfs->Stat(s_runtimePath, &fileInfo);
            if (statError != Storage::Error::Ok) {
                const char* const detail = RuntimeDetail(entry.path, entry.pathBytes, Storage::ErrorMessage(statError));
                if (!optional) Fail(Diagnostics::kFailRuntimeLoad, detail);
                Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnRuntimeOptional, detail);
                ++failed;
                continue;
            }
            if (fileInfo.directory || fileInfo.bytes == 0 || fileInfo.bytes > kRuntimeModuleMaxBytes || fileInfo.bytes != entry.dataBytes) {
                const char* const detail = RuntimeDetail(entry.path, entry.pathBytes, "the runtime module size doesn't match the manifest entry");
                if (!optional) Fail(Diagnostics::kFailRuntimeEntry, detail);
                Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnRuntimeOptional, detail);
                ++failed;
                continue;
            }
            uint64_t handle = 0;
            const Storage::Error openError = vfs->Open(s_runtimePath, &handle);
            if (openError != Storage::Error::Ok) {
                const char* const detail = RuntimeDetail(entry.path, entry.pathBytes, Storage::ErrorMessage(openError));
                if (!optional) Fail(Diagnostics::kFailRuntimeLoad, detail);
                Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnRuntimeOptional, detail);
                ++failed;
                continue;
            }
            uint32_t readBytes = 0;
            const Storage::Error readError = vfs->Read(handle, 0, s_runtimeBytes, static_cast<uint32_t>(fileInfo.bytes), &readBytes);
            (void)vfs->Close(handle);
            if (readError != Storage::Error::Ok || readBytes != fileInfo.bytes) {
                const char* const detail = RuntimeDetail(entry.path, entry.pathBytes, readError != Storage::Error::Ok ? Storage::ErrorMessage(readError) : "the runtime module read returned fewer bytes than the stat");
                if (!optional) Fail(Diagnostics::kFailRuntimeLoad, detail);
                Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnRuntimeOptional, detail);
                ++failed;
                continue;
            }
            // 与闭包同一套装载编排：closureModule=false 表示运行期模块（来源核对不做，摘要由装载器第 1 步复算）
            Kernel::LoadRequest request;
            request.bytes = s_runtimeBytes;
            request.bytesCount = readBytes;
            request.manifestEntry = &entry;
            request.handoff = nullptr;
            request.closureModule = false;
            request.preloadOrder = 0;
            Kernel::LoadOutcome outcome;
            const Diagnostics::Diagnostic diagnostic = Kernel::LoadModule(context, request, outcome);
            if (diagnostic.code != nullptr) {
                // Init 失败归 81（必需模块初始化失败），装载机制失败归 80；detail 带条目路径与机制层原因
                const bool initFailed = diagnostic.code == Diagnostics::kFailModuleInit.code;
                const char* const detail = RuntimeDetail(entry.path, entry.pathBytes, outcome.reason);
                if (!optional) Fail(initFailed ? Diagnostics::kFailRuntimeInit : Diagnostics::kFailRuntimeLoad, detail);
                Diagnostics::WriteDiagnostic(Kernel::Tags::Tag::Warn, Diagnostics::kWarnRuntimeOptional, detail);
                ++failed;
                continue;
            }
            ++loaded;
        }
        // 汇总行：loaded = 成功装载数，failed = 可选失败降级数，optional = 清单里的可选条目数
        // 必需失败会走 FATAL，不会走到这里；正式发布集没有运行期条目时三个数都是 0
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Ok);
        Print::Write("runtime modules: loaded=");
        WriteDecimal(loaded);
        Print::Write(" failed=");
        WriteDecimal(failed);
        Print::Write(" optional=");
        WriteDecimal(optionalCount);
        Print::Write("\r\n");
    }
}

// 内核异常入口的公共目标：打印带编号的向量、错误码与出错位置后停机，不返回
// 帧由 Core.asm 的入口桩整理，自低到高固定为：向量号、错误码、返回偏移、CS、RFLAGS
extern "C" void _Kernel_Exception_Handler(const uint64_t* frame) {
    // 异常行的现场是多个数值字段：先写编号行开头（标签 + 编号 + 文本），再按固定顺序补括号现场
    Kernel::Diagnostics::WriteDiagnosticPrefix(Kernel::Tags::Tag::Fatal, Kernel::Diagnostics::kException);
    Print::Write(" (Vec=");
    Print::WriteHex(static_cast<uint32_t>(frame[0]));
    Print::Write(" Err=");
    Print::WriteHex(static_cast<uint32_t>(frame[1]));
    Print::Write(" Rip=");
    Print::WriteHex64(frame[2]);
    Print::Write(" Cs=");
    Print::WriteHex(static_cast<uint32_t>(frame[3]));
    Print::Write(" Rflags=");
    Print::WriteHex64(frame[4]);
    Print::Write(")\r\n");
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
    if (const char* reason = Kernel::CheckImageData(__image_start, linkedBytes, facts)) Fail(Diagnostics::kFailImageData, reason);
    if (facts.imageBytes != linkedBytes) Fail(Diagnostics::kFailImageBytes);
    if (facts.memoryBytes != linkedSpan) Fail(Diagnostics::kFailImageMemory);
    if (facts.loadAddress != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__image_start))) Fail(Diagnostics::kFailImageLoadAddress);
    if (facts.entryOffset != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_Kernel_Start) - reinterpret_cast<uintptr_t>(__image_start))) Fail(Diagnostics::kFailImageEntry);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The kernel image data matches the linked image");
    ReportKernelFacts(facts);

    // BootInfo：尺寸与版本先行，通过之后其余字段才是可解释的事实
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Checking the BootInfo");
    if (info == nullptr) Fail(Diagnostics::kFailBootInfoNull);
    // 指针可用之后交给 Fail 路径：FATAL 的等待时长按 BootInfo 的 fatalWaitMs 决定
    s_info = info;
    if (info->version != Boot::InfoLayout::kVersion) Fail(Diagnostics::kFailBootInfoVersion);
    if (info->bytes < Boot::InfoLayout::kBytes) Fail(Diagnostics::kFailBootInfoBytes);
    if (info->flags != 0) Fail(Diagnostics::kFailBootInfoFlags);
    if (facts.bootInfoVersion != Boot::InfoLayout::kVersion) Fail(Diagnostics::kFailImageBootInfoVersion);
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
            Fail(Diagnostics::kFailUefiServicesActive);
        default:
            Fail(Diagnostics::kFailFirmwareState);
    }

    // CPU 事实：长模式是入口契约的前提，能力位只作记录
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Probing the CPU");
    if (!s_cpu.Install()) Fail(Diagnostics::kFailCpuid);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The CPU is probed");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("Cpu=");
    Print::Write(s_cpu.Vendor());
    Print::Write(" LongMode=");
    Print::WriteHex(s_cpu.HasLongMode() ? 1 : 0);
    Print::Write(" Tsc=");
    Print::WriteHex(s_cpu.HasTsc() ? 1 : 0);
    Print::Write("\r\n");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("Apic=");
    Print::WriteHex(s_cpu.HasApic() ? 1 : 0);
    Print::Write(" Nx=");
    Print::WriteHex(s_cpu.HasNx() ? 1 : 0);
    Print::Write("\r\n");

    // 运行期段表、任务寄存器与中断表：此后 CPU 异常停在诊断行上，而不是三重故障
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Installing descriptor tables");
    if (const char* reason = Kernel::InstallDescriptorTables()) Fail(Diagnostics::kFailDescriptorTables, reason);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The descriptor tables are installed");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("Gdt=0x08 Idt=256 Tss=0x28 Ist=1\r\n");

    // 引导页表：恒等映射保持映像与栈可用，直映射窗口供内核访问物理内存
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Installing the boot page tables");
    if (const char* reason = Kernel::InstallBootPaging()) Fail(Diagnostics::kFailBootPaging, reason);
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

    // 交接包核对：清单副本、列表副本、预装记录与内核镜像事实的一致性（9 步，见 Handoff.hpp）
    // 放在引导页表之后、页帧分配之前：三类缓冲与模块字节都按直映射窗口读；任何一项不成立
    // 都不交接、不降级，按 E-KRN-5x 走 FATAL 路径（等待与复位在 Fail 里完成）
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Checking the kernel set handoff package");
    Kernel::HandoffFacts handoff;
    const char* handoffReason = nullptr;
    const uint8_t* const osBuildId = __image_start + Kernel::ImageDataLayout::kFieldBuildId;
    if (const Diagnostics::Diagnostic reason = Kernel::CheckHandoff(*info, osBuildId, facts.moduleMajor, handoff, &handoffReason); reason.code != nullptr)
        Fail(reason, handoffReason);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The kernel set handoff package is consistent");
    ReportHandoffFacts(handoff);

    // 内存层：按内存图放行可用页，保留低地址与内核自身；分配返回物理地址
    // 内核跨度按页上取整后交给分配器：跨度尾部不满一页的部分（.bss 尾页，里面有控制台目标指针）
    // 也必须标占——MarkRange 对结束地址按页下取整，不取整会让首次分配把这一页交出去
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Initializing the page allocator");
    const uint64_t kernelBegin = reinterpret_cast<uintptr_t>(__image_start);
    const uint64_t kernelEnd = (reinterpret_cast<uintptr_t>(__bss_end) + Kernel::kPageBytes - 1) & ~(Kernel::kPageBytes - 1);
    s_pages.Initialize(*info, kernelBegin, kernelEnd);
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The page allocator is ready");
    Kernel::Tags::WriteTag(Kernel::Tags::Tag::Continue);
    Print::Write("ManagedMiB=");
    Print::WriteHex(static_cast<uint32_t>(Kernel::kManagedLimitBytes >> 20));
    Print::Write(" FreeMiB=");
    Print::WriteHex(static_cast<uint32_t>(s_pages.FreePages() >> 8));   // 1MiB = 256 页
    Print::Write("\r\n");

    // 交接区间标占：三类缓冲与每个模块字节在首次分配之前逐段保留，分配器不再交出这些页
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Reserving the handoff ranges");
    if (!ReserveHandoff(handoff)) Fail(Diagnostics::kFailHandoffMissing, "reserving a handoff range failed");
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The handoff ranges are reserved");

    // 复制接管：三类缓冲与模块字节复制到内核自有 4 KiB 帧，之后内核只读自有副本
    // 分配失败按交接包不可用停机（E-KRN-50）；没有为“接管失败”单列编号，沿用交接包缺失的编号
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Info, "Taking over the handoff buffers");
    if (!TakeOverHandoff(handoff)) Fail(Diagnostics::kFailHandoffMissing, "copying a handoff range into kernel memory failed");
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Ok, "The handoff buffers are copied into kernel memory");
    ReportHandoffCopies(handoff);

    // —— 内核核心导出、引导闭包装载与系统卷挂载：内核初始化的接线点 ——
    // 页帧分配器就绪、交接包核对通过、缓冲接管完成之后：先登记内核核心导出（Core.* 十张槽表，
    // 失败 E-KRN-59），再按预装顺序与依赖拓扑逐条装载引导闭包；每个模块 Init 成功由装载器打印标记
    // 闭包全部 Init 之后再挂载系统卷并读取清单（失败 E-KRN-70–76）；任何失败走 FATAL（等待与复位在 Fail 里完成）
    RegisterCoreExports(info);
    LoadBootClosure(info, handoff);
    MountSystemVolume(info, handoff);
    CheckIso9660SecondSource(info);
    LoadRuntimeModules(info, handoff);

    // 阶段边界：调度、进程与 IPC 模型尚未实现，停在可观测的状态上而不是跑进未定义代码
    // 这是阶段边界行而不是失败路径：不带 E-KRN 编号，与进度行一样只说明当前停在哪里
    Kernel::Tags::WriteLine(Kernel::Tags::Tag::Warn, "The kernel core stage ends here: the scheduler and the process model aren't implemented yet");
    Stop();
}
