/* BootX64.cpp
    UEFI x64 侧的主流程：以固件应用形态跑完与 BaleenCore 相同的职责，直到把内核交出去

    顺序与 BaleenCore 一致：取介质块设备、读并校验布局描述、挂 Ext4 系统卷、核对卷标识、
    报写缓存刷新能力、选版（读引导状态记录、验清单与签名、防回滚与吊销）、定位并校验内核镜像、
    整幅读进装载地址、取内存图、退出启动服务、构造 BootInfo，最后按内核入口契约跳进去

    与 BaleenCore 的差别只在载体：介质来自块设备协议而不是 BIOS 读盘服务，
    控制台来自固件简单文本输出（退出启动服务后换成直接驱动的串口），内存图来自固件而不是 E820；
    判据、编号与文本两侧一致，编号按 Docs/Specs/Baleen/失败诊断编号.md 第四节的
    同号异前缀约定：共享实现里的序号不变，前缀随编译目标换成 E-UEFI
*/

#include <stdint.h>

#include <BootInfo.hpp>
#include <Console.hpp>
#include <Diagnostics.hpp>
#include <EfiBlockDevice.hpp>
#include <EfiTypes.hpp>
#include <Ext4.hpp>
#include <LayoutDescriptor.hpp>
#include <Libc.hpp>
#include <LoadKernel.hpp>
#include <Print.hpp>
#include <PrintTarget.hpp>
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
    Baleen::Uefi::EfiBlockDevice s_medium;
    Storage::SubDevice s_systemVolume;
    alignas(64) Baleen::Core::Ext4FileSystem s_ext4;
    Baleen::Uefi::MemoryMap s_memory;
    // 选版结果：目标集路径、集目录与集身份
    Baleen::Core::SelectedSet s_selected;
    // 交接事实：内核入口按物理地址读取它
    Boot::Info s_bootInfo;

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

    // 校验布局描述的固定字段与校验和：不通过时返回带编号的原因；判据与 BaleenCore 完全一致
    Diagnostic CheckLayoutBlock(const uint8_t* block) {
        using Baleen::LayoutDescriptor;
        if (Get32(block + LayoutDescriptor::kFieldMagic) != LayoutDescriptor::kMagic) return kFailLayoutMagic;
        if (Get16(block + LayoutDescriptor::kFieldVersion) != LayoutDescriptor::kVersion) return kFailLayoutVersion;
        if (Get16(block + LayoutDescriptor::kFieldHeaderBytes) != LayoutDescriptor::kHeaderBytes) return kFailLayoutHead;
        if (Get32(block + LayoutDescriptor::kFieldLayoutBytes) != LayoutDescriptor::kLayoutBytes) return kFailLayoutBytes;
        if ((Get32(block + LayoutDescriptor::kFieldFlags) & ~LayoutDescriptor::kKnownFlags) != 0) return kFailLayoutFlags;
        if (Get16(block + LayoutDescriptor::kFieldReserved) != 0) return kFailLayoutReserved;
        if (Get32(block + LayoutDescriptor::kFieldChecksum) != LayoutDescriptor::Checksum(block)) return kFailLayoutChecksum;
        const uint32_t pathBytes = Get16(block + LayoutDescriptor::kFieldKernelPathBytes);
        if (pathBytes == 0 || pathBytes > LayoutDescriptor::kKernelPathMaxBytes) return kFailLayoutPathBytes;
        if (block[LayoutDescriptor::kFieldKernelPath] != '/') return kFailLayoutPathAbsolute;
        if (Get64(block + LayoutDescriptor::kFieldVolumeBytes) == 0) return kFailLayoutVolumeBytes;
        return Diagnostic{};
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
    BlockIo* WholeMediumBlockIo(BootServices* bootServices, EfiHandle deviceHandle, BlockIo* loaded) {
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

    // 构造交接事实：固件启动服务已退出，内存图、启动来源与内核集身份按本次交接的实际情况填入
    void BuildBootInfo(Boot::Media media, uint32_t sectorBytes, uint64_t mediumBytes, uint64_t loaderBegin, uint64_t loaderBytes) {
        using Baleen::LayoutDescriptor;
        s_bootInfo.bytes = Boot::InfoLayout::kBytes;
        s_bootInfo.version = Boot::InfoLayout::kVersion;
        s_bootInfo.flags = 0;
        s_bootInfo.firmware = static_cast<uint32_t>(Boot::Firmware::UefiExited);
        s_bootInfo.media = static_cast<uint32_t>(media);
        s_bootInfo.sectorBytes = sectorBytes;
        s_bootInfo.channels = static_cast<uint32_t>(Boot::Channel::Serial) | static_cast<uint32_t>(Boot::Channel::DebugPort);
        s_bootInfo.memoryMapCount = s_memory.Count();
        s_bootInfo.memoryMapTruncated = s_memory.Truncated() ? 1u : 0u;
        s_bootInfo.reserved0 = 0;
        // 内存图数组的物理地址：本阶段静态存储，交接期间保持有效
        s_bootInfo.memoryMap = reinterpret_cast<uint64_t>(s_memory.Entries());
        for (uint32_t i = 0; i < 4; ++i) s_bootInfo.reserved[i] = 0;
        for (uint32_t i = 0; i < Boot::InfoLayout::kKernelSetBuildIdBytes; ++i) s_bootInfo.kernelSetBuildId[i] = s_selected.verified ? s_selected.buildId[i] : 0;
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
        for (uint32_t i = 0; i < 4; ++i) s_bootInfo.reserved1[i] = 0;
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

    // —— 介质块设备：从「本映像由哪个设备加载」出发取块设备协议 ——
    // 走固件能力是引导期访问介质的首选，UEFI 侧因此不需要任何自建的控制器路径
    // 布局描述在介质字节偏移 0x20000：被加载的设备是分区时上溯到整个设备，否则块号会从分区起点算
    Baleen::PrintTargets::WriteLine(Tag::Info, "Binding the medium block device");
    LoadedImage* image = nullptr;
    if (IsError(bootServices->handleProtocol(imageHandle, &kLoadedImageProtocolGuid, reinterpret_cast<void**>(&image))) ||
        image == nullptr || image->deviceHandle == nullptr)
        Fail(kFailBindDisk, "the loaded image reports no device handle");
    BlockIo* imageBlockIo = nullptr;
    if (IsError(bootServices->handleProtocol(image->deviceHandle, &kBlockIoProtocolGuid, reinterpret_cast<void**>(&imageBlockIo))) ||
        imageBlockIo == nullptr)
        Fail(kFailBindDisk, "the device handle has no block I/O protocol");
    BlockIo* const medium = WholeMediumBlockIo(bootServices, image->deviceHandle, imageBlockIo);
    if (medium == nullptr) Fail(kFailBindDisk, "the whole medium has no block I/O protocol");
    if (medium != imageBlockIo) Baleen::PrintTargets::WriteLine(Tag::Info, "The loaded image sits on a partition; binding the whole medium instead");
    if (const Storage::Error error = s_medium.Bind(medium); error != Storage::Error::Ok)
        Fail(kFailBindDisk, Storage::ErrorMessage(error));
    // 启动设备线索：把分区句柄的设备路径抄进静态缓冲，随 BootInfo 交给内核
    // 抄不到（没有设备路径协议或路径过长）不是失败：内核还有卷标识可以核对，只报一条 WARN
    DevicePath* cluePath = nullptr;
    if (IsError(bootServices->handleProtocol(image->deviceHandle, &kDevicePathProtocolGuid, reinterpret_cast<void**>(&cluePath))) || !CopyDeviceClue(cluePath))
        Baleen::WriteDiagnostic(Tag::Warn, kWarnDeviceClueTooLong);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The medium block device is bound");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Block=");
    Print::WriteHex(s_medium.BlockBytes());
    Print::Write(" Bytes=");
    Print::WriteHex64(s_medium.CapacityBytes());
    Print::Write(" Rw=");
    Print::WriteHex(s_medium.Writable() ? 1 : 0);
    Print::Write(" Removable=");
    Print::WriteHex(s_medium.Removable() ? 1 : 0);
    Print::Write(" Align=");
    Print::WriteHex(s_medium.IoAlign());
    Print::Write("\r\n");

    // —— 布局描述：与 BIOS 侧读同一处介质字节偏移，判据逐条一致 ——
    Baleen::PrintTargets::WriteLine(Tag::Info, "Reading the layout descriptor");
    if (const Storage::Error error = s_medium.Read(Baleen::LayoutDescriptor::kOffset, s_layoutBlock, sizeof(s_layoutBlock)); error != Storage::Error::Ok)
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
    if (const Storage::Error error = Storage::SubDevice::Open(s_medium, volumeOffset, volumeBytes, s_systemVolume); error != Storage::Error::Ok)
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
    // 只读介质没有写缓存要刷，按“无可用原语”报出；命令出错按写失败处理
    const Storage::Error flush = s_medium.Flush();
    if (flush == Storage::Error::Ok) {
        Baleen::PrintTargets::WriteLine(Tag::Ok, "The write cache can be flushed on this medium");
        Baleen::PrintTargets::WriteTag(Tag::Continue);
        Print::Write("Flush=firmware FlushBlocks\r\n");
    } else if (flush == Storage::Error::NotSupported || flush == Storage::Error::ReadOnly) {
        Baleen::PrintTargets::WriteLine(Tag::Info, "No write cache flush primitive is available on this medium");
    } else {
        Baleen::PrintTargets::WriteLine(Tag::Warn, "The write cache flush failed on this medium");
    }

    // —— 内存图：选版与装载都要拿它核对事实，交接时再取一次把 MapKey 交回固件 ——
    Baleen::PrintTargets::WriteLine(Tag::Info, "Capturing the firmware memory map");
    if (const Diagnostic reason = s_memory.Allocate(bootServices); reason.code != nullptr) Fail(reason);
    if (const Diagnostic reason = s_memory.Capture(bootServices); reason.code != nullptr) Fail(reason);
    s_memory.Convert();
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The firmware memory map is captured");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Entries=");
    Print::WriteHex(s_memory.Count());
    Print::Write(" Truncated=");
    Print::WriteHex(s_memory.Truncated() ? 1 : 0);
    Print::Write("\r\n");

    // —— 选版：读引导状态记录，选出目标集并验证清单与签名 ——
    // 状态机与写入边界见 Docs/Specs/内核集更新与回滚.md 与 Baleen/引导状态记录.md；
    // 清单与信任锚见 Docs/Specs/内核集清单与签名.md；只读介质与写失败按降级路径直启
    Baleen::PrintTargets::WriteLine(Tag::Info, "Selecting the kernel set");
    if (const Diagnostic reason = Baleen::Core::SelectKernelSet(s_ext4, s_medium, stateOffset, stateBytes, s_kernelPath, Get32(s_layoutBlock + Baleen::LayoutDescriptor::kFieldFlags), s_selected); reason.code != nullptr)
        Fail(reason);
    Baleen::PrintTargets::WriteLine(Tag::Ok, s_selected.verified ? "The kernel set is verified" : "No kernel set identity; starting the image the layout names");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Set=");
    if (s_selected.verified) {
        // 集标识按字节顺序的 64 位小写十六进制，与集目录名同一串字符
        const char kHexDigits[] = "0123456789abcdef";
        for (uint32_t i = 0; i < 32; ++i) {
            const char text[3] = { kHexDigits[s_selected.buildId[i] >> 4], kHexDigits[s_selected.buildId[i] & 0x0f], '\0' };
            Print::Write(text);
        }
    } else {
        Print::Write("none");
    }
    Print::Write(" Attempt=");
    Print::WriteHex64(s_selected.attemptId);
    Print::Write("\r\n");

    // —— 内核镜像：按选中集的路径定位、读取并校验数据与整幅摘要 ——
    Baleen::PrintTargets::WriteLine(Tag::Info, "Locating and checking the kernel image");
    Baleen::Core::KernelFacts kernelFacts;
    bool kernelMissing = false;
    const Diagnostic kernelReason = Baleen::Core::CheckKernelImage(s_ext4, s_selected.kernelPath, s_memory.Entries(), s_memory.Count(), kernelFacts, kernelMissing);
    if (kernelMissing) {
        Baleen::WriteDiagnostic(Tag::Warn, kWarnKernelMissing);
        Baleen::PrintTargets::WriteLine(Tag::Warn, "The UEFI stage ends here: there is no kernel to hand over");
        Stop();
    }
    if (kernelReason.code != nullptr) Fail(kernelReason);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel image is valid");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Bytes=");
    Print::WriteHex(kernelFacts.imageBytes);
    Print::Write(" Load=");
    Print::WriteHex(kernelFacts.loadAddress);
    Print::Write(" Entry=");
    Print::WriteHex(kernelFacts.entryOffset);
    Print::Write("\r\n");

    // —— 装载：整幅读进装载地址，按静态内存跨度清零未落盘尾部 ——
    Baleen::PrintTargets::WriteLine(Tag::Info, "Loading the kernel image");
    if (const Diagnostic reason = Baleen::Core::LoadKernelImage(s_ext4, s_selected.kernelPath, kernelFacts.loadAddress, kernelFacts.imageBytes); reason.code != nullptr)
        Fail(reason);
    uint8_t* const kernelImage = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kernelFacts.loadAddress));
    if (kernelFacts.memoryBytes > kernelFacts.imageBytes)
        memset(kernelImage + kernelFacts.imageBytes, 0, kernelFacts.memoryBytes - kernelFacts.imageBytes);
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The kernel image is loaded");
    Baleen::PrintTargets::WriteTag(Tag::Continue);
    Print::Write("Load=");
    Print::WriteHex(kernelFacts.loadAddress);
    Print::Write(" Bytes=");
    Print::WriteHex(kernelFacts.imageBytes);
    Print::Write(" Span=");
    Print::WriteHex(kernelFacts.memoryBytes);
    Print::Write("\r\n");

    // —— 退出启动服务：MapKey 必须与当前内存图匹配，键过期就重新取图再试 ——
    // 这一步不是清理，而是固定运行模式：此后不再调用任何固件服务，BootInfo 里如实标记
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
    s_memory.Convert();
    // 8254 此时归本阶段使用：CPUID 没给出频率时用它实测一次，好把耗时换算成毫秒
    s_clock.CalibrateWithPit();
    Baleen::PrintTargets::WriteLine(Tag::Ok, "The firmware boot services are exited");

    // —— 交接状态：BootInfo 与内存图数组都在本阶段的静态存储里 ——
    // 启动介质按块大小判定：光盘是 2048 字节块，其余按硬盘上报
    Baleen::PrintTargets::WriteLine(Tag::Info, "Building the handover state");
    BuildBootInfo(s_medium.BlockBytes() == 2048 ? Boot::Media::Cdrom : Boot::Media::Hdd, s_medium.BlockBytes(), s_medium.CapacityBytes(),
                  reinterpret_cast<uint64_t>(image->imageBase), image->imageSize);
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
