/* BootX64.cpp
    UEFI x64 侧的主流程：以固件应用形态跑完与 BaleenCore 相同的职责，直到把内核交出去

    顺序与 BaleenCore 一致：取介质块设备、读并校验布局描述、挂 Ext4 系统卷、核对卷标识、
    报写缓存刷新能力、选版（读引导状态记录、验清单与签名、防回滚与吊销）、定位并校验内核镜像、
    按 Modules.list 预装引导闭包、整幅读进装载地址、取内存图、退出启动服务、构造 BootInfo，
    最后按内核入口契约跳进去

    只有内核镜像、清单副本、列表副本、预装记录与模块字节全部就绪才交权：装载或预装失败都按
    两阶段选版回退（候选集失败改选已知良好集，已知良好集失败终止），不交出残缺的交接现场；
    交接缓冲经 UefiAllocator 放在低 4GiB，失败路径逐段 FreePages 回收

    与 BaleenCore 的差别只在载体：介质来自块设备协议而不是 BIOS 读盘服务，
    控制台来自固件简单文本输出（退出启动服务后换成直接驱动的串口），内存图来自固件而不是 E820；
    判据、编号与文本两侧一致，编号按同号异前缀约定：共享实现里的序号不变，前缀随编译目标换成 E-UEFI
*/

#include <stdint.h>

#include <BootClosure.hpp>
#include <BootInfo.hpp>
#include <BootMemory.hpp>
#include <Console.hpp>
#include <ControllerPath.hpp>
#include <Diagnostics.hpp>
#include <EfiBlockDevice.hpp>
#include <EfiTypes.hpp>
#include <Ext4.hpp>
#include <LayoutCheck.hpp>
#include <LayoutDescriptor.hpp>
#include <Libc.hpp>
#include <LoadKernel.hpp>
#include <Print.hpp>
#include <Print/SerialTarget.hpp>
#include <PrintTarget.hpp>
#include <ScreenFacts.hpp>
#include <SelectSet.hpp>
#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/SubDevice.hpp>
#include <UefiClock.hpp>
#include <UefiDiagnostics.hpp>
#include <UefiMemory.hpp>

// 交接入口：由 EnterKernel.asm 提供，关中断、换栈、按 RDI 传 BootInfo 后跳内核入口，不返回
extern "C" [[noreturn]] void _Uefi_Enter_Kernel(uint64_t entry, uint64_t bootInfo);

namespace {
    using namespace Baleen::Uefi;
    using Baleen::Diagnostic;
    using Baleen::PrintTargets::Tag;

    // 交接缓冲分配失败：落点分配失败与 BaleenCore 的 E-CORE-97 同号异前缀（共享实现的编号约定）
    constexpr Diagnostic kFailHandoffMemory{BALEEN_STAGE_CODE("97"), "There is no memory for preloading the boot closure modules"};
    // 自建控制器路径的失败：固件没有块协议时经 PCI 自建读路径，各阶段有独立编号
    constexpr Diagnostic kFailControllerDevicePath{BALEEN_STAGE_CODE("100"), "The boot controller path is unavailable"};
    constexpr Diagnostic kFailControllerPciIo{BALEEN_STAGE_CODE("101"), "The boot controller has no PCI I/O protocol"};
    constexpr Diagnostic kFailControllerClass{BALEEN_STAGE_CODE("102"), "The boot controller isn't a supported AHCI or NVMe device"};
    constexpr Diagnostic kFailControllerBar{BALEEN_STAGE_CODE("103"), "The controller BAR is unusable"};
    constexpr Diagnostic kFailControllerInit{BALEEN_STAGE_CODE("104"), "The controller didn't initialize or read"};

    // 自建控制器路径的阶段到诊断编号的映射：失败时由 BindMedium 报告
    Diagnostic ControllerStageDiagnostic(ControllerStage stage) {
        switch (stage) {
            case ControllerStage::DevicePath: return kFailControllerDevicePath;
            case ControllerStage::PciIo: return kFailControllerPciIo;
            case ControllerStage::Class: return kFailControllerClass;
            case ControllerStage::Bar: return kFailControllerBar;
            case ControllerStage::Init: return kFailControllerInit;
            default: return kFailBindDisk;   // 绑定时段不明确：沿用介质块设备绑定失败
        }
    }

    // 布局描述读取缓冲：一个 4096 字节块，按固件可能要求的最大对齐摆放
    alignas(4096) uint8_t s_layoutBlock[Baleen::LayoutDescriptor::kBytes] = {};
    // 内核路径与它的父目录（内核集目录）
    char s_kernelPath[Baleen::LayoutDescriptor::kKernelPathMaxBytes + 1] = {};
    char s_setDirectory[Baleen::LayoutDescriptor::kKernelPathMaxBytes + 1] = {};
    // 目录项名字缓冲：足够放下卷内最长名字
    constexpr uint32_t kNameBytes = 128;
    // 设备路径上溯用的缓冲：分区节点的父路径通常只有几十字节，给足余量
    constexpr uint32_t kParentPathBytes = 256;
    // 启动设备线索缓冲：把固件的设备路径抄进来，退出启动服务之后不再依赖固件内存
    // 上限取契约里的 kDeviceClueMaxBytes：足够放下 PciRoot/Pci/Sata/HD 这类分区路径，
    // 放不下时线索按“未提供”处理并报 WARN
    constexpr uint32_t kDeviceClueBytes = Boot::kDeviceClueMaxBytes;
    uint8_t s_deviceClue[kDeviceClueBytes] = {};
    uint32_t s_deviceClueUsed = 0;                              // 线索字节数；0 表示未提供
    Boot::DeviceKind s_deviceClueKind = Boot::DeviceKind::None; // 线索类型
    // 引导计时：入口取时间戳并尝试用 CPUID 校准
    Baleen::Uefi::Clock s_clock;
    // 控制台、介质块设备、系统卷偏移视图、Ext4 实例、内存图与交接事实：静态存储，不做动态分配
    Baleen::Uefi::Console s_console;
    Baleen::Uefi::EfiBlockDevice s_efiMedium;
    Baleen::Uefi::ControllerBlockDevice s_controllerMedium;
    // 当前使用的介质块设备：固件块协议或自建控制器路径，两者实现同一个 Storage::BlockDevice 接口
    Storage::BlockDevice* s_medium = nullptr;
    Storage::SubDevice s_systemVolume;
    alignas(64) Baleen::Core::Ext4FileSystem s_ext4;
    Baleen::Uefi::MemoryMap s_memory;
    // 选版结果：目标集路径、集目录与集身份
    Baleen::Core::SelectedSet s_selected;
    // 预装：清单条目到预装条目的视图缓冲、来源与计划；机制本身不动态分配，这些都放静态存储
    Baleen::Core::PreloadArtifact s_listedArtifactViews[Baleen::Core::kPreloadMaxEntries] = {};
    Baleen::Core::PreloadSource s_preloadSource;
    Baleen::Core::PreloadPlan s_preloadPlan;
    // 预装结果：三类交接缓冲的物理地址与长度；清单 / 列表 / 记录三份副本在低 4GiB
    Baleen::Core::PreloadResult s_preloadResult;
    // 交接缓冲分配器：模块字节与三份副本都经它分配，失败路径按已分配表 FreePages 回收
    Baleen::Uefi::UefiAllocator s_handoffAllocator;
    // GOP 屏幕事实：退出启动服务之前采集，之后不再读固件协议内存
    Baleen::Uefi::ScreenFacts s_screen;
    // 交接事实：内核入口按物理地址读取它
    Boot::Info s_bootInfo;
    // 本映像的装载跨度：必须在退出启动服务之前从 LoadedImage 协议取出并保存，
    // 协议内存退出后不保证有效；随 BootInfo 交给内核，内核不得把它当可用内存
    uint64_t s_loaderBegin = 0;
    uint64_t s_loaderBytes = 0;

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

    // 校验布局描述：判据、编号与 BaleenCore 侧共用 LayoutCheck 的一份实现
    Diagnostic CheckLayoutBlock(const uint8_t* block) {
        return Baleen::Core::LayoutCheck::Check(block);
    }

    // 重叠安全的内存搬运：退出启动服务之后，中转缓冲与固定装载地址都可能落在
    // 同一段归系统的可用内存里，正向/反向按源与目标的相对位置选择
    void MoveBytes(uint8_t* destination, const uint8_t* source, uint32_t bytes) {
        if (destination == source || bytes == 0) return;
        if (destination < source) {
            for (uint32_t i = 0; i < bytes; ++i) destination[i] = source[i];
        } else {
            for (uint32_t i = bytes; i > 0; --i) destination[i - 1] = source[i - 1];
        }
    }

    // 取路径的父目录：去掉最后一个 '/' 之后的部分；没有父目录时返回 false
    bool ParentDirectory(const char* path, char* out) {
        uint32_t length = 0;
        while (path[length] != '\0') ++length;
        while (length > 0 && path[length - 1] != '/') --length;
        if (length == 0) return false;
        // 去掉结尾的分隔符，保留根目录本身
        while (length > 1 && path[length - 1] == '/') --length;
        for (uint32_t i = 0; i < length; ++i) out[i] = path[i];
        out[length] = '\0';
        return true;
    }

    // 取介质块设备：布局描述按介质起点算位置，所以块设备视图必须是整个设备
    // 被加载的设备是分区时（块设备报告 LogicalPartition）沿设备路径上溯一层，取父设备的块设备协议
    // 上溯失败返回空，由调用方按绑定失败处理
    // 只有固件块协议路径会调用：强制自建控制器路径的验证变体里它不出现，避免未使用告警
    [[maybe_unused]] BlockIo* WholeMediumBlockIo(BootServices* bootServices, EfiHandle deviceHandle, BlockIo* loaded) {
        if (loaded == nullptr || loaded->media == nullptr) return nullptr;
        if (loaded->media->logicalPartition == 0) return loaded;   // 本来就是整个设备
        DevicePath* path = nullptr;
        if (IsError(bootServices->handleProtocol(deviceHandle, &kDevicePathProtocolGuid, reinterpret_cast<void**>(&path))) || path == nullptr) return nullptr;
        // 走到最后一个节点（分区节点），把它之前的路径抄进本地缓冲并以结束节点收尾
        const uint8_t* const begin = reinterpret_cast<const uint8_t*>(path);
        const uint8_t* last = nullptr;
        const DevicePath* node = path;
        for (;;) {
            if (node->type == kDevicePathEndType && node->subType == kDevicePathEndSubType) break;
            const uint32_t nodeBytes = static_cast<uint32_t>(node->length[0]) | (static_cast<uint32_t>(node->length[1]) << 8);
            if (nodeBytes < sizeof(DevicePath)) return nullptr;   // 节点长度非法
            last = reinterpret_cast<const uint8_t*>(node);
            node = reinterpret_cast<const DevicePath*>(last + nodeBytes);
        }
        if (last == nullptr) return nullptr;   // 只剩结束节点：没有可上溯的父设备
        const uint32_t parentBytes = static_cast<uint32_t>(last - begin);
        if (parentBytes + sizeof(DevicePath) > kParentPathBytes) return nullptr;
        alignas(8) uint8_t parent[kParentPathBytes] = {};
        for (uint32_t i = 0; i < parentBytes; ++i) parent[i] = begin[i];
        parent[parentBytes] = kDevicePathEndType;
        parent[parentBytes + 1] = kDevicePathEndSubType;
        parent[parentBytes + 2] = sizeof(DevicePath);
        parent[parentBytes + 3] = 0;
        DevicePath* pointer = reinterpret_cast<DevicePath*>(parent);
        EfiHandle disk = nullptr;
        if (IsError(bootServices->locateDevicePath(&kBlockIoProtocolGuid, &pointer, &disk)) || disk == nullptr) return nullptr;
        BlockIo* whole = nullptr;
        if (IsError(bootServices->handleProtocol(disk, &kBlockIoProtocolGuid, reinterpret_cast<void**>(&whole)))) return nullptr;
        return whole;
    }

    // 把固件的设备路径抄进静态缓冲：线索交给内核之后，固件内存不该再被依赖
    // 返回是否成功；路径过长或节点长度非法时线索保持“未提供”，由调用方报一条 WARN
    bool CopyDeviceClue(DevicePath* path) {
        s_deviceClueUsed = 0;
        s_deviceClueKind = Boot::DeviceKind::None;
        if (path == nullptr) return false;
        // 走到结束节点，累计字节数（含结束节点本身）
        uint32_t bytes = 0;
        const DevicePath* node = path;
        for (;;) {
            if (node->type == kDevicePathEndType && node->subType == kDevicePathEndSubType) {
                bytes += sizeof(DevicePath);
                break;
            }
            const uint32_t nodeBytes = static_cast<uint32_t>(node->length[0]) | (static_cast<uint32_t>(node->length[1]) << 8);
            if (nodeBytes < sizeof(DevicePath)) return false;   // 节点长度非法
            bytes += nodeBytes;
            if (bytes + sizeof(DevicePath) > kDeviceClueBytes) return false;
            node = reinterpret_cast<const DevicePath*>(reinterpret_cast<const uint8_t*>(node) + nodeBytes);
        }
        if (bytes > kDeviceClueBytes) return false;
        const uint8_t* const source = reinterpret_cast<const uint8_t*>(path);
        for (uint32_t i = 0; i < bytes; ++i) s_deviceClue[i] = source[i];
        s_deviceClueUsed = bytes;
        s_deviceClueKind = Boot::DeviceKind::UefiDevicePath;
        return true;
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

    // 预装引导闭包：清单副本、列表副本、模块字节与记录数组都放在低 4GiB
    // 本映像可能被固件装到 4GiB 以上，映像内静态缓冲内核按恒等映射读不到，
    // 因此三份副本都要复制到分配器给的缓冲；失败返回带编号的原因，已分配落点由调用方统一回收
    Baleen::Diagnostic PreloadBootClosure() {
        const Baleen::Core::ManifestFacts& manifest = s_selected.handoff.manifest;
        Baleen::Core::PreloadSpan manifestSpan{};
        if (!s_handoffAllocator.Allocate(manifest.manifestBytes, manifestSpan)) return kFailHandoffMemory;
        Baleen::Core::PreloadSpan listSpan{};
        if (!s_handoffAllocator.Allocate(manifest.bootListBytes, listSpan)) return kFailHandoffMemory;
        memcpy(manifestSpan.data, manifest.manifestCopy, manifest.manifestBytes);
        memcpy(listSpan.data, manifest.bootListCopy, manifest.bootListBytes);

        s_preloadSource = Baleen::Core::PreloadSource{};
        s_preloadSource.setDirectory = s_selected.setDirectory;
        s_preloadSource.manifest = manifestSpan.data;
        s_preloadSource.manifestBytes = manifest.manifestBytes;
        s_preloadSource.list = listSpan.data;
        s_preloadSource.listBytes = manifest.bootListBytes;
        s_preloadSource.moduleMajor = manifest.moduleMajor;
        s_preloadSource.artifacts = s_listedArtifactViews;
        s_preloadSource.artifactCount = BuildListedArtifacts(manifest);
        if (const Baleen::Diagnostic reason = Baleen::Core::PlanPreload(s_preloadSource, s_preloadPlan); reason.code != nullptr) return reason;
        Baleen::Core::StoragePreloadSink sink(s_ext4, s_handoffAllocator);
        if (const Baleen::Diagnostic reason = Baleen::Core::PreloadModules(s_preloadPlan, sink, s_preloadResult); reason.code != nullptr) return reason;

        // 记录数组：共享实现写在映像内的静态存储里，这里复制到低 4GiB 缓冲后再交接
        const uint32_t recordBytes = Baleen::Core::kPreloadArrayHeaderBytes + s_preloadResult.recordCount * Baleen::Core::kPreloadRecordBytes;
        Baleen::Core::PreloadSpan recordSpan{};
        if (!s_handoffAllocator.Allocate(recordBytes, recordSpan)) return kFailHandoffMemory;
        memcpy(recordSpan.data, reinterpret_cast<const void*>(static_cast<uintptr_t>(s_preloadResult.recordsPhys)), recordBytes);
        s_preloadResult.recordsPhys = recordSpan.phys;
        return Diagnostic{};
    }

    // 采集 GOP 屏幕事实：拿不到协议、格式不受支持都按“未提供”给 None，不是失败
    // 屏幕是诊断增强，退出启动服务之前读完事实，之后不再碰固件协议内存
    void CollectGopFacts(BootServices* bootServices) {
        s_screen = Baleen::Uefi::ScreenFacts{};
        if (bootServices == nullptr || bootServices->locateProtocol == nullptr) return;
        GraphicsOutputProtocol* gop = nullptr;
        if (IsError(bootServices->locateProtocol(&kGraphicsOutputProtocolGuid, nullptr, reinterpret_cast<void**>(&gop))) || gop == nullptr || gop->mode == nullptr) return;
        s_screen = Baleen::Uefi::CollectScreenFacts(gop->mode);
    }

    // 写十进制无符号数：预装摘要行的 count 与 bytes 用十进制，x64 上除法是原生指令
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
    // 预装失败不落成半套，也不走本行；退出启动服务之后调用，此时串口是直接驱动的可靠通道
    void ReportPreloadSummary() {
        Baleen::PrintTargets::WriteTag(Tag::Continue);
        Print::Write("boot closure preloaded: count=");
        WriteDecimal(s_preloadResult.recordCount);
        Print::Write(" bytes=");
        WriteDecimal(static_cast<uint32_t>(s_preloadResult.moduleBytes));
        Print::Write(" bad=0\r\n");
    }

    // 播报本次选定的目标集：集标识按字节顺序的 64 位小写十六进制，与集目录名同一串字符
    // 回退改选之后同样播报一次，日志里能看出交接用的是哪一份
    void ReportSelectedSet() {
        Baleen::PrintTargets::WriteTag(Tag::Continue);
        Print::Write("Set=");
        static const char kHexDigits[] = "0123456789abcdef";
        for (uint32_t i = 0; i < 32; ++i) {
            const char text[3] = { kHexDigits[s_selected.buildId[i] >> 4], kHexDigits[s_selected.buildId[i] & 0x0f], '\0' };
            Print::Write(text);
        }
        Print::Write(" Attempt=");
        Print::WriteHex64(s_selected.attemptId);
        Print::Write(" Rev=");
        Print::WriteHex64(s_selected.moduleRevision);
        Print::Write("\r\n");
    }

    // 构造交接事实：固件启动服务已退出，内存图、启动来源与内核集身份按本次交接的实际情况填入
    void BuildBootInfo(Boot::Media media, uint32_t sectorBytes, uint64_t mediumBytes, uint64_t loaderBegin, uint64_t loaderBytes) {
        using Baleen::LayoutDescriptor;
        s_bootInfo.bytes = Boot::InfoLayout::kBytes;
        s_bootInfo.version = Boot::InfoLayout::kVersion;
        s_bootInfo.flags = 0;
        s_bootInfo.firmware = static_cast<uint32_t>(Boot::Firmware::UefiExited);
        s_bootInfo.media = static_cast<uint32_t>(media);
        s_bootInfo.sectorBytes = sectorBytes;
        // 输出通道按事实给出：调试口无条件可用，串口以存在性探测为准（与 BaleenCore 同一写法）
        uint32_t channels = static_cast<uint32_t>(Boot::Channel::DebugPort);
        if (Print::SerialTarget::Probe(Print::SerialTarget::kDefaultBase)) channels |= static_cast<uint32_t>(Boot::Channel::Serial);
        s_bootInfo.channels = channels;
        s_bootInfo.memoryMapCount = s_memory.Count();
        s_bootInfo.memoryMapTruncated = s_memory.Truncated() ? 1u : 0u;
        // FATAL 等待按构建配置给出：0 = 内核按默认 10 秒；屏幕事实按退出启动服务之前
        // 采集到的 GOP 形态填，拿不到时是 None，内核只走流式通道，不改启动判定
        s_bootInfo.fatalWaitMs = Baleen::Uefi::kFatalWaitMs;
        // 内存图数组的物理地址：本阶段静态存储，交接期间保持有效
        s_bootInfo.memoryMap = reinterpret_cast<uint64_t>(s_memory.Entries());
        s_bootInfo.screenAddress = s_screen.address;
        s_bootInfo.screenWidth = s_screen.width;
        s_bootInfo.screenHeight = s_screen.height;
        s_bootInfo.screenStride = s_screen.stride;
        s_bootInfo.screenBytes = s_screen.bytes;
        s_bootInfo.screenKind = static_cast<uint32_t>(s_screen.kind);
        s_bootInfo.screenFormat = static_cast<uint32_t>(s_screen.format);
        // 内核集身份：选版成功的集标识与试启动身份；选版成功时集一定经过清单验签
        for (uint32_t i = 0; i < Boot::InfoLayout::kKernelSetBuildIdBytes; ++i) s_bootInfo.kernelSetBuildId[i] = s_selected.buildId[i];
        s_bootInfo.attemptId = s_selected.attemptId;
        // —— 启动来源：布局描述给出的位置事实，加固件环境自己的介质与映像事实 ——
        s_bootInfo.layoutOffset = LayoutDescriptor::kOffset;
        s_bootInfo.volumeOffset = Get64(s_layoutBlock + LayoutDescriptor::kFieldVolumeOffset);
        s_bootInfo.volumeBytes = Get64(s_layoutBlock + LayoutDescriptor::kFieldVolumeBytes);
        s_bootInfo.stateOffset = Get64(s_layoutBlock + LayoutDescriptor::kFieldStateOffset);
        s_bootInfo.stateBytes = Get32(s_layoutBlock + LayoutDescriptor::kFieldStateBytes);
        s_bootInfo.mediumBytes = mediumBytes;
        s_bootInfo.kernelPathBytes = Get16(s_layoutBlock + LayoutDescriptor::kFieldKernelPathBytes);
        s_bootInfo.kernelPath = reinterpret_cast<uint64_t>(s_kernelPath);
        for (uint32_t i = 0; i < Boot::InfoLayout::kVolumeUuidBytes; ++i)
            s_bootInfo.volumeUuid[i] = s_layoutBlock[LayoutDescriptor::kFieldVolumeUuid + i];
        // 设备线索：固件给的设备路径，内核据此定位启动介质（拿不到时按“未提供”）
        s_bootInfo.devicePath = reinterpret_cast<uint64_t>(s_deviceClue);
        s_bootInfo.devicePathBytes = s_deviceClueUsed;
        s_bootInfo.deviceKind = static_cast<uint32_t>(s_deviceClueKind);
        // 引导器自身占用区间：本映像的装载跨度，内核不得当成可用内存
        s_bootInfo.loaderBegin = loaderBegin;
        s_bootInfo.loaderBytes = loaderBytes;
        // 引导闭包交接包：清单副本、列表副本与预装记录数组都由预装步骤给出，
        // 三份副本都在低 4GiB，且指向实际装载的模块字节
        s_bootInfo.kernelSetManifest = s_preloadResult.manifestPhys;
        s_bootInfo.bootList = s_preloadResult.listPhys;
        s_bootInfo.bootModules = s_preloadResult.recordsPhys;
        s_bootInfo.kernelSetManifestBytes = s_preloadResult.manifestBytes;
        s_bootInfo.bootListBytes = s_preloadResult.listBytes;
    }

    // 停机并停留，供自动化读取诊断；失败路径与阶段边界共用
    [[noreturn]] void Stop() {
        for (;;) __asm__ volatile("hlt");
    }

    // 打印 FATAL 原因后停机；detail 用于补上存储层错误名一类的现场，不替代编号
    [[noreturn]] void Fail(const Diagnostic& reason, const char* detail = nullptr) {
        Baleen::WriteDiagnostic(Tag::Fatal, reason, detail);
        Stop();
    }
}

// 入口：固件按 EFIAPI（Microsoft x64 约定）调用，两个参数在 RCX 与 RDX 里
extern "C" BALEEN_EFIAPI Baleen::Uefi::EfiStatus efi_main(void* imageHandle, Baleen::Uefi::SystemTable* systemTable) {
    // 计时先于一切：入口时间戳要尽量早，控制台装配与校准的耗时都算在本阶段里
    s_clock.Install();
    if (systemTable == nullptr || !s_console.Install(systemTable)) return kStatusInvalidParameter;
    Baleen::PrintTargets::WriteLine(Tag::BaleenUefi, "LikesProgramOS Baleen UEFI (x64)");
    if (imageHandle == nullptr || systemTable->bootServices == nullptr) Fail(kFailNoBootServices);
    BootServices* const bootServices = systemTable->bootServices;

    // —— 介质块设备：从「本映像由哪个设备加载」出发 ——
    // 首选固件块协议；固件没有给启动控制器提供块协议（或验证变体强制）时，走自建控制器路径：
    // 经 PCI I/O 协议定位 AHCI / NVMe，用内核侧同一份只读实现访问整盘。
    // 两条路径都实现 Storage::BlockDevice，布局描述 / 卷 / 选版 / 预装 / 交接流程不分叉。
    // 布局描述在介质字节偏移 0x20000：被加载的设备是分区时上溯到整个设备，否则块号会从分区起点算
    Baleen::PrintTargets::WriteLine(Tag::Info, "Binding the medium block device");
    LoadedImage* image = nullptr;
    if (IsError(bootServices->handleProtocol(imageHandle, &kLoadedImageProtocolGuid, reinterpret_cast<void**>(&image))) ||
        image == nullptr || image->deviceHandle == nullptr)
        Fail(kFailBindDisk, "the loaded image reports no device handle");
    // 引导器占用区间：在退出启动服务之前取出并保存，之后不再读协议内存
    if (image->imageBase == nullptr || image->imageSize == 0) Fail(kFailLoadedImageFacts);
    s_loaderBegin = reinterpret_cast<uint64_t>(image->imageBase);
    s_loaderBytes = image->imageSize;
    bool firmwareBound = false;
#ifndef FORCE_CONTROLLER_PATH
    BlockIo* imageBlockIo = nullptr;
    if (!IsError(bootServices->handleProtocol(image->deviceHandle, &kBlockIoProtocolGuid, reinterpret_cast<void**>(&imageBlockIo))) &&
        imageBlockIo != nullptr) {
        BlockIo* const medium = WholeMediumBlockIo(bootServices, image->deviceHandle, imageBlockIo);
        if (medium != nullptr) {
            if (medium != imageBlockIo) Baleen::PrintTargets::WriteLine(Tag::Info, "The loaded image sits on a partition; binding the whole medium instead");
            if (s_efiMedium.Bind(medium) == Storage::Error::Ok) {
                s_medium = &s_efiMedium;
                firmwareBound = true;
            }
        }
    }
    if (!firmwareBound) Baleen::PrintTargets::WriteLine(Tag::Warn, "The firmware block I/O protocol isn't usable; trying the self-built controller path");
#endif
    if (!firmwareBound) {
        if (s_controllerMedium.Bind(bootServices, image->deviceHandle) != Storage::Error::Ok)
            Fail(ControllerStageDiagnostic(s_controllerMedium.Stage()), s_controllerMedium.Detail());
        s_medium = &s_controllerMedium;
        Baleen::PrintTargets::WriteLine(Tag::Info, "The self-built controller path is bound");
    }
    // 启动设备线索：把分区句柄的设备路径抄进静态缓冲，随 BootInfo 交给内核
    // 抄不到（没有设备路径协议或路径过长）不是失败：内核还有卷标识可以核对，只报一条 WARN
    DevicePath* cluePath = nullptr;
    if (IsError(bootServices->handleProtocol(image->deviceHandle, &kDevicePathProtocolGuid, reinterpret_cast<void**>(&cluePath))) || !CopyDeviceClue(cluePath))
        Baleen::WriteDiagnostic(Tag::Warn, kWarnDeviceClueTooLong);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The medium block device is bound");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Block=");
    Print::WriteHex(s_medium->BlockBytes());
    Print::Write(" Bytes=");
    Print::WriteHex64(s_medium->CapacityBytes());
    Print::Write(" Rw=");
    Print::WriteHex(s_medium->Writable() ? 1 : 0);
    if (s_medium == &s_efiMedium) {
        Print::Write(" Removable=");
        Print::WriteHex(s_efiMedium.Removable() ? 1 : 0);
        Print::Write(" Align=");
        Print::WriteHex(s_efiMedium.IoAlign());
    } else {
        Print::Write(" Controller=");
        Print::Write(s_controllerMedium.ControllerName());
    }
    Print::Write("\r\n");
    if (s_medium == &s_controllerMedium) {
        Baleen::PrintTargets::WriteTag(Tag::Continue);
        Print::Write("ControllerModel=");
        Print::Write(s_controllerMedium.Model());
        Print::Write("\r\n");
    }

    // —— 布局描述：与 BIOS 侧读同一处介质字节偏移，判据逐条一致 ——
    Baleen::PrintTargets::WriteLine(Tag::Info, "Reading the layout descriptor");
    if (const Storage::Error error = s_medium->Read(Baleen::LayoutDescriptor::kOffset, s_layoutBlock, sizeof(s_layoutBlock)); error != Storage::Error::Ok)
        Fail(kFailReadLayout, Storage::ErrorMessage(error));
    if (const Diagnostic reason = CheckLayoutBlock(s_layoutBlock); reason.code != nullptr) Fail(reason);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The layout descriptor is valid");
    const uint64_t volumeOffset = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeOffset);
    const uint64_t volumeBytes = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeBytes);
    const uint64_t stateOffset = Get64(s_layoutBlock + Baleen::LayoutDescriptor::kFieldStateOffset);
    const uint32_t stateBytes = Get32(s_layoutBlock + Baleen::LayoutDescriptor::kFieldStateBytes);
    const uint32_t kernelPathBytes = Get16(s_layoutBlock + Baleen::LayoutDescriptor::kFieldKernelPathBytes);
    for (uint32_t i = 0; i < kernelPathBytes; ++i) s_kernelPath[i] = static_cast<char>(s_layoutBlock[Baleen::LayoutDescriptor::kFieldKernelPath + i]);
    s_kernelPath[kernelPathBytes] = '\0';
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Volume=");
    Print::WriteHex64(volumeOffset);
    Print::Write(" Bytes=");
    Print::WriteHex64(volumeBytes);
    Print::Write(" State=");
    Print::WriteHex64(stateOffset);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Kernel=");
    Print::Write(s_kernelPath);
    Print::Write("\r\n");

    // —— 系统卷：在介质设备上开偏移视图，挂 Ext4 并核对卷标识 ——
    Baleen::PrintTargets::WriteLine(Tag::Info, "Mounting the system volume");
    if (const Storage::Error error = Storage::SubDevice::Open(*s_medium, volumeOffset, volumeBytes, s_systemVolume); error != Storage::Error::Ok)
        Fail(kFailOpenVolume, Storage::ErrorMessage(error));
    if (const Storage::Error error = s_ext4.Mount(s_systemVolume, Storage::MountMode::ReadOnly); error != Storage::Error::Ok)
        Fail(kFailMountVolume, Storage::ErrorMessage(error));
    if (__builtin_memcmp(s_ext4.Uuid(), s_layoutBlock + Baleen::LayoutDescriptor::kFieldVolumeUuid, 16) != 0)
        Fail(kFailVolumeUuid);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The system volume matches the layout descriptor");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Ext4=");
    Print::WriteHex(s_ext4.BlockBytes());
    Print::Write(" Inodes=");
    Print::WriteHex(s_ext4.InodesCount());
    Print::Write(" Blocks=");
    Print::WriteHex64(s_ext4.BlocksCount());
    Print::Write("\r\n");

    // —— 内核集目录：按布局描述给出的内核路径取父目录，逐项列出来 ——
    Baleen::PrintTargets::WriteLine(Tag::Info, "Listing the kernel set directory");
    if (!ParentDirectory(s_kernelPath, s_setDirectory)) Fail(kFailOpenSetDirectory, "the kernel path has no parent directory");
    Storage::FileId directory{};
    if (const Storage::Error error = s_ext4.Open(s_setDirectory, directory); error != Storage::Error::Ok)
        Fail(kFailOpenSetDirectory, Storage::ErrorMessage(error));
    Storage::FileInfo directoryInfo{};
    if (const Storage::Error error = s_ext4.Stat(s_setDirectory, directoryInfo); error != Storage::Error::Ok)
        Fail(kFailOpenSetDirectory, Storage::ErrorMessage(error));
    if (!directoryInfo.directory) Fail(kFailOpenSetDirectory, "the kernel path's parent isn't a directory");
    if (!s_ext4.Caps().directoryListing) Fail(kFailListSetDirectory, "the volume implementation doesn't list directories");
    uint64_t cursor = 0;
    uint32_t entries = 0;
    for (;;) {
        char name[kNameBytes] = {};
        uint32_t nameBytes = 0;
        Storage::FileInfo info{};
        const Storage::Error error = s_ext4.ReadDirectory(directory, cursor, name, sizeof(name), nameBytes, info);
        if (error == Storage::Error::NotFound) break;
        if (error != Storage::Error::Ok) Fail(kFailListSetDirectory, Storage::ErrorMessage(error));
        Baleen::PrintTargets::WriteTag(Tag::Continue);
        Print::Write(info.directory ? "D " : "F ");
        Print::Write(name);
        if (!info.directory) {
            Print::Write(" ");
            Print::WriteHex64(info.bytes);
        }
        Print::Write("\r\n");
        ++entries;
    }
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel set directory is listed");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Dir=");
    Print::Write(s_setDirectory);
    Print::Write(" Entries=");
    Print::WriteHex(entries);
    Print::Write("\r\n");

    // —— 写缓存刷新能力：固件给的 FlushBlocks 就是刷新原语，不需要自建控制器路径 ——
    // 只读介质没有写缓存要刷；固件对 FlushBlocks 报 Unsupported 时这一次按"无缓存"通过，
    // 但"没有原语"已经记下——结论按 HasFlushPrimitive 给，不按单次 Flush 的返回值谎称可刷新
    const Storage::Error flush = s_medium->Flush();
    if (flush == Storage::Error::Ok && s_medium->HasFlushPrimitive()) {
        Baleen::PrintTargets::WriteLine(Tag::Ok, "The write cache can be flushed on this medium");
        Baleen::PrintTargets::WriteTag(Tag::Continue);
        Print::Write("Flush=firmware FlushBlocks\r\n");
    } else if (flush == Storage::Error::Ok || flush == Storage::Error::NotSupported || flush == Storage::Error::ReadOnly) {
        Baleen::PrintTargets::WriteLine(Tag::Info, "No write cache flush primitive is available on this medium");
    } else {
        Baleen::PrintTargets::WriteLine(Tag::Warn, "The write cache flush failed on this medium");
    }

    // —— 选版（第一阶段）：读引导状态记录，定下目标集并验证清单与签名 ——
    // 选版落实引导状态记录的状态机与写入边界，并验证清单与信任锚；
    // 只读介质与写失败按降级路径启动缺省集，
    // 缺省集与其它集一样先验证清单与签名（没有未验证的直启回退）
    Baleen::PrintTargets::WriteLine(Tag::Info, "Selecting the kernel set");
    if (const Diagnostic reason = Baleen::Core::BeginSetSelection(s_ext4, *s_medium, stateOffset, stateBytes, s_kernelPath, Get32(s_layoutBlock + Baleen::LayoutDescriptor::kFieldFlags), s_selected); reason.code != nullptr)
        Fail(reason);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel set is verified");
    ReportSelectedSet();

    // —— 内核镜像与引导闭包：装载或预装失败都按两阶段选版回退 ——
    // 候选集失败时由选择器写 Failed 并改选已知良好集或上一修订；已知良好集（含缺省集）失败终止
    // 只有内核镜像、清单副本、列表副本、预装记录与模块字节全部就绪才往下走，不交出残缺的交接现场
    s_handoffAllocator.Install(bootServices);
    Baleen::Core::KernelFacts kernelFacts;
    uint64_t kernelSpan = 0;
    uint64_t staging = 0;
    uint64_t stagingPages = 0;
    uint8_t* stagingImage = nullptr;
    for (;;) {
        // —— 内核镜像：按选中集的路径定位、读取并校验数据与整幅摘要 ——
        // 装载区在退出启动服务之后按最终内存图核对：退出前看到的“可用”不等于本阶段能写
        Baleen::PrintTargets::WriteLine(Tag::Info, "Locating and checking the kernel image");
        bool kernelMissing = false;
        const Diagnostic kernelReason = Baleen::Core::CheckKernelImage(s_ext4, s_selected.kernelPath, s_selected.buildId, nullptr, 0, s_loaderBegin, s_loaderBytes, kernelFacts, kernelMissing);
        Diagnostic failure{};
        bool preloadFailed = false;
        if (kernelMissing) {
            Baleen::WriteDiagnostic(Tag::Warn, kWarnKernelMissing);
            failure = kWarnKernelMissing;
        } else if (kernelReason.code != nullptr) {
            failure = kernelReason;
        } else {
            Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel image is valid");
            Baleen::PrintTargets::WriteTag(Tag::Continue);
            Print::Write("Bytes=");
            Print::WriteHex(kernelFacts.imageBytes);
            Print::Write(" Load=");
            Print::WriteHex(kernelFacts.loadAddress);
            Print::Write(" Entry=");
            Print::WriteHex(kernelFacts.entryOffset);
            Print::Write("\r\n");

            // —— 内核镜像先读进固件分配的中转缓冲：镜像数据声明的固定装载地址在本阶段退出启动服务
            //    之前可能正落在固件启动服务内存里，不能直接写入；AllocatePages 保证中转缓冲的归属 ——
            kernelSpan = kernelFacts.memoryBytes > kernelFacts.imageBytes ? kernelFacts.memoryBytes : kernelFacts.imageBytes;
            stagingPages = (static_cast<uint64_t>(kernelFacts.imageBytes) + kPageBytes - 1) / kPageBytes;
            staging = 0;
            const EfiStatus stagingStatus = bootServices->allocatePages(kAllocateAnyPages, kMemoryLoaderData, stagingPages, &staging);
            if (IsError(stagingStatus) || staging == 0) {
                failure = kFailKernelMemory;
            } else {
                Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel image buffer is allocated");
                // —— 装载：整幅读进中转缓冲；固定装载地址等退出启动服务后再写入 ——
                Baleen::PrintTargets::WriteLine(Tag::Info, "Loading the kernel image");
                stagingImage = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(staging));
                if (const Diagnostic reason = Baleen::Core::LoadKernelImageTo(s_ext4, s_selected.kernelPath, stagingImage, kernelFacts.imageBytes); reason.code != nullptr) {
                    failure = reason;
                    bootServices->freePages(staging, stagingPages);
                    staging = 0;
                } else {
                    Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel image is loaded into the firmware buffer");
                    Baleen::PrintTargets::WriteTag(Tag::Continue);
                    Print::Write("Load=");
                    Print::WriteHex(kernelFacts.loadAddress);
                    Print::Write(" Bytes=");
                    Print::WriteHex(kernelFacts.imageBytes);
                    Print::Write(" Span=");
                    Print::WriteHex(kernelFacts.memoryBytes);
                    Print::Write("\r\n");

                    // —— 预装：按 Modules.list 把引导闭包读进低 4GiB，三类缓冲都在那里 ——
                    Baleen::PrintTargets::WriteLine(Tag::Info, "Preloading the boot closure");
                    const Diagnostic preloadReason = PreloadBootClosure();
                    if (preloadReason.code == nullptr) break;
                    failure = preloadReason;
                    preloadFailed = true;
                    bootServices->freePages(staging, stagingPages);
                    staging = 0;
                }
            }
        }

        // —— 回退：候选集的装载 / 预装失败由选择器改选已知良好集或上一修订 ——
        // 本次已分配的交接缓冲先整体归还；中转缓冲在上面各失败分支里已单独归还
        s_handoffAllocator.ReleaseAll();
        Baleen::Core::SelectedSet fallback;
        const Diagnostic next = Baleen::Core::SelectAfterLoadFailure(s_ext4, *s_medium, stateOffset, stateBytes, s_kernelPath, Get32(s_layoutBlock + Baleen::LayoutDescriptor::kFieldFlags), s_selected, preloadFailed, failure, fallback);
        if (next.code != nullptr) {
            // 没有内核可交且没有可回退的目标：本阶段到此为止（E-UEFI-58 与 E-CORE-58 同一句话）
            if (!preloadFailed && kernelMissing && next.code == kWarnKernelMissing.code) {
                Baleen::WriteDiagnostic(Tag::Warn, kWarnNoKernel);
                Stop();
            }
            Fail(next);
        }
        s_selected = fallback;
        Baleen::PrintTargets::WriteLine(Tag::Warn, "The selected set failed to load; falling back to the recorded set");
        ReportSelectedSet();
    }

    // —— GOP 屏幕事实：退出启动服务之前读完，之后不再碰固件协议内存 ——
    // 拿不到协议或格式不受支持都给 None，屏幕不是启动前提，不因它失败
    CollectGopFacts(bootServices);

    // —— 退出启动服务：MapKey 必须与当前内存图匹配，键过期就重新取图再试 ——
    // 最终快照必须紧贴 ExitBootServices 取得；取图缓冲在退出前分配，之后不再调用固件
    Baleen::PrintTargets::WriteLine(Tag::Info, "Capturing the final firmware memory map");
    if (const Diagnostic reason = s_memory.Allocate(bootServices); reason.code != nullptr) Fail(reason);
    Baleen::PrintTargets::WriteLine(Tag::Info, "Exiting the firmware boot services");
    bool exited = false;
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        if (const Diagnostic reason = s_memory.Capture(bootServices); reason.code != nullptr) Fail(reason);
        const Baleen::Uefi::EfiStatus status = bootServices->exitBootServices(imageHandle, s_memory.Key());
        if (!IsError(status)) {
            exited = true;
            break;
        }
        // 只有 MapKey 过期才值得重试：其余错误重试也不会变
        if (status != kStatusInvalidParameter) break;
    }
    if (!exited) Fail(kFailExitBootServices);
    // 固件控制台与池分配都到此为止：控制台换成串口与调试口，最后一张内存图就是交给内核的事实
    s_console.ExitFirmwareConsole();
    // 取图快照是退出启动服务之前抓的；退出之后启动服务与引导器内存归系统，
    // 按退出后的视角转换，BootServices 内存不再当作固件占用
    s_memory.Convert(true);
    // 固定装载跨度在退出后必须整段归系统：最终内存图说可用才允许拷入并清零静态内存，
    // 这段内存在退出前可能是固件启动服务内存，本阶段从未在退出前写过它
    if (!Baleen::SpanUsable(s_memory.Entries(), s_memory.Count(), kernelFacts.loadAddress, kernelSpan)) Fail(kFailKernelSpanAfterExit);
    // 目标跨度不得与仍在执行的引导器映像重叠：本阶段还要从这里跳内核
    const uint64_t kernelEnd = static_cast<uint64_t>(kernelFacts.loadAddress) + kernelSpan;
    if (static_cast<uint64_t>(kernelFacts.loadAddress) < s_loaderBegin + s_loaderBytes && s_loaderBegin < kernelEnd)
        Fail(kFailKernelSpanAfterExit);
    uint8_t* const kernelImage = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kernelFacts.loadAddress));
    // 中转缓冲与目标可能落在同一段归系统的内存里，用重叠安全的搬运
    MoveBytes(kernelImage, stagingImage, kernelFacts.imageBytes);
    if (kernelFacts.memoryBytes > kernelFacts.imageBytes)
        memset(kernelImage + kernelFacts.imageBytes, 0, kernelFacts.memoryBytes - kernelFacts.imageBytes);
    // 8254 此时归本阶段使用：CPUID 没给出频率时用它实测一次，好把耗时换算成毫秒
    s_clock.CalibrateWithPit();
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The firmware boot services are exited");
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel image is placed at its load address");
    // 预装摘要行：预装已在退出启动服务之前完成，这里用直接驱动的串口输出，保证可抓取
    ReportPreloadSummary();

    // —— 交接状态：BootInfo 与内存图数组都在本阶段的静态存储里 ——
    // 启动介质按块大小判定：光盘是 2048 字节块，其余按硬盘上报
    Baleen::PrintTargets::WriteLine(Tag::Info, "Building the handover state");
    BuildBootInfo(s_medium->BlockBytes() == 2048 ? Boot::Media::Cdrom : Boot::Media::Hdd, s_medium->BlockBytes(), s_medium->CapacityBytes(),
                  s_loaderBegin, s_loaderBytes);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The handover state is ready");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("BootInfo=");
    Print::WriteHex64(reinterpret_cast<uint64_t>(&s_bootInfo));
    Print::Write(" Mmap=");
    Print::WriteHex(s_bootInfo.memoryMapCount);
    Print::Write(" Firmware=");
    Print::WriteHex(s_bootInfo.firmware);
    Print::Write(" Channels=");
    Print::WriteHex(s_bootInfo.channels);
    Print::Write("\r\n");
    // 启动来源：内核与运行期更新据此知道“从哪个介质、哪个卷、哪条路径起来的”
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Boot=Volume=");
    Print::WriteHex64(s_bootInfo.volumeOffset);
    Print::Write(" Bytes=");
    Print::WriteHex64(s_bootInfo.volumeBytes);
    Print::Write(" State=");
    Print::WriteHex64(s_bootInfo.stateOffset);
    Print::Write(" Kind=");
    Print::WriteHex(s_bootInfo.deviceKind);
    Print::Write(" Clue=");
    Print::WriteHex(s_bootInfo.devicePathBytes);
    Print::Write(" Medium=");
    Print::WriteHex64(s_bootInfo.mediumBytes);
    Print::Write("\r\n");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Path=");
    Print::Write(s_kernelPath);
    Print::Write("\r\n");
    // 引导闭包交接包与屏幕事实：内核入口按这些地址读三类缓冲，屏幕只报形态与等待配置
    // 三类缓冲都在低 4GiB；屏幕拿不到 GOP 或格式不受支持时 Kind=0
    Baleen::PrintTargets::WriteTag(Tag::Continue);
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
    Baleen::PrintTargets::WriteTag(Tag::Continue);
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

    // 引导耗时：从本阶段入口到交接的毫秒数与预算判定；没有校准源时 Verdict=NoClock
    const uint32_t bootMs = s_clock.MillisSinceEntry();
    const bool overBudget = s_clock.IsCalibrated() && bootMs > kBootBudgetMs;
    Baleen::PrintTargets::WriteTag(overBudget ? Tag::Warn : Tag::Continue);
    if (overBudget) {
        Print::Write(kWarnBootBudget.code);
        Print::Write(" ");
    }
    Print::Write("BootMs=");
    Print::WriteHex(bootMs);
    Print::Write(" BudgetMs=");
    Print::WriteHex(kBootBudgetMs);
    Print::Write(" Verdict=");
    Print::Write(s_clock.IsCalibrated() ? (overBudget ? "Over" : "Ok") : "NoClock");
    Print::Write("\r\n");

    // —— 进入内核：长模式与分页已在运行，按入口契约把 BootInfo 放进 RDI 后跳过去，不返回 ——
    Baleen::PrintTargets::WriteLine(Tag::Ok, "Entering the kernel");
    _Uefi_Enter_Kernel(static_cast<uint64_t>(kernelFacts.loadAddress) + kernelFacts.entryOffset, reinterpret_cast<uint64_t>(&s_bootInfo));
    Stop();
}
