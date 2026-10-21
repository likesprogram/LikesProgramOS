/* main.cpp
    CheckEfiBlockDevice：Baleen UEFI 的块设备适配与固件内存图的宿主侧检查，按需手工执行，不构成默认构建依赖

    检查对象是目标侧源码 Packages/Baleen/Uefi/Src/EfiBlockDevice.cpp 与 UefiMemory.cpp：本工具把
    这两份源码按宿主选项编译后直接运行，不做第二份参考解析
    块设备部分：底层协议是就地构造的假 EFI_BLOCK_IO_PROTOCOL，按与固件同一套调用约定
    （BALEEN_EFIAPI）声明，背后是一块 64 KiB 的内存介质，函数里按固件语义核对介质标识、
    介质在位、块数整倍数、缓冲对齐与范围——这样「未对齐缓冲经内部对齐缓冲中转」那条路径
    才真的被检查到，而不是只看返回值
    内存图部分：假启动服务表提供 getMemoryMap / allocatePool / freePool，取图缓冲用宿主堆代替
    固件池，脚本能制造 BufferTooSmall 与格式非法，并按用例核对池分配的调用次数、缓冲尺寸与归还
    覆盖范围：块设备的绑定事实与拒绝、读写的参数校验与错误映射、介质更换与拔出、
    对齐中转的直读与跨多次下发、块大小超过中转上限时的已知边界；
    固件内存图退出启动服务前后两种可用性视角、BufferTooSmall 重取与 MapKey、格式与参数拒绝
*/

#include <BootInfo.hpp>
#include <Diagnostics.hpp>
#include <EfiBlockDevice.hpp>
#include <EfiTypes.hpp>
#include <UefiMemory.hpp>

#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
    using Baleen::Diagnostic;
    using Baleen::Uefi::BlockIo;
    using Baleen::Uefi::BlockIoMedia;
    using Baleen::Uefi::BootServices;
    using Baleen::Uefi::EfiBlockDevice;
    using Baleen::Uefi::EfiLba;
    using Baleen::Uefi::EfiStatus;
    using Baleen::Uefi::MemoryDescriptor;
    using Baleen::Uefi::MemoryMap;

    // 模拟介质规模：64 KiB、512 字节逻辑块，共 128 块
    constexpr uint32_t kMediumBytes = 64 * 1024;
    // 模拟介质的默认逻辑块大小
    constexpr uint32_t kBlockBytes = 512;
    // 模拟介质的默认块数
    constexpr uint32_t kBlockCount = kMediumBytes / kBlockBytes;
    // 实现的中转上限，与 EfiBlockDevice 的 kTransferBytes 同值
    constexpr uint32_t kTransferBytes = 4096;
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 25;
    // 已跑用例数
    unsigned g_ran = 0;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckEfiBlockDevice: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 假块设备：协议实例必须是首成员，协议函数据此从 this 指针取回整份假设备
    struct FakeDevice {
        BlockIo blockIo = {};                                   // 协议实例，动作指向本文件的静态函数
        BlockIoMedia media = {};                                // 介质事实
        uint8_t data[kMediumBytes] = {};                        // 模拟介质
        EfiStatus readStatus = Baleen::Uefi::kStatusSuccess;    // 读的脚本化返回值
        EfiStatus writeStatus = Baleen::Uefi::kStatusSuccess;   // 写的脚本化返回值
        EfiStatus flushStatus = Baleen::Uefi::kStatusSuccess;   // 刷新的脚本化返回值
        uint32_t reads = 0;                                     // 协议收到的读次数
        uint32_t writes = 0;                                    // 协议收到的写次数
        uint32_t flushes = 0;                                   // 协议收到的刷新次数
        uint32_t lastMediaId = 0;                               // 最近一次动作收到的介质标识
        EfiLba lastLba = 0;                                     // 最近一次读写的块号
        uint64_t lastBytes = 0;                                 // 最近一次读写的字节数
        const void* lastBuffer = nullptr;                       // 最近一次读写的缓冲
    };

    static_assert(offsetof(FakeDevice, blockIo) == 0, "协议实例必须是首成员");

    // 假协议的复位：本工具不使用，恒报不受支持
    EfiStatus BALEEN_EFIAPI FakeReset(BlockIo* self, uint8_t extendedVerification) {
        (void)self;
        (void)extendedVerification;
        return Baleen::Uefi::kStatusUnsupported;
    }

    // 缓冲是否满足介质事实里的对齐要求：与固件的判定同源
    bool MediumBufferAligned(const FakeDevice& device, const void* buffer) {
        const uint32_t ioAlign = device.media.ioAlign;
        if (ioAlign <= 1) return true;
        return (reinterpret_cast<uintptr_t>(buffer) % ioAlign) == 0;
    }

    // 假协议的读：先按固件语义核对事实，再把介质字节搬到调用方缓冲
    EfiStatus BALEEN_EFIAPI FakeReadBlocks(BlockIo* self, uint32_t mediaId, EfiLba lba, uint64_t bytes, void* buffer) {
        FakeDevice& device = *reinterpret_cast<FakeDevice*>(self);
        ++device.reads;
        device.lastMediaId = mediaId;
        device.lastLba = lba;
        device.lastBytes = bytes;
        device.lastBuffer = buffer;
        if (device.readStatus != Baleen::Uefi::kStatusSuccess) return device.readStatus;
        if (mediaId != device.media.mediaId) return Baleen::Uefi::kStatusMediaChanged;
        if (device.media.mediaPresent == 0) return Baleen::Uefi::kStatusNoMedia;
        if (bytes == 0 || (bytes % device.media.blockSize) != 0) return Baleen::Uefi::kStatusBadBufferSize;
        if (!MediumBufferAligned(device, buffer)) return Baleen::Uefi::kStatusInvalidParameter;
        const uint64_t firstBlock = lba;
        const uint64_t blocks = bytes / device.media.blockSize;
        if (firstBlock > device.media.lastBlock || blocks > (device.media.lastBlock - firstBlock) + 1) {
            return Baleen::Uefi::kStatusInvalidParameter;
        }
        std::memcpy(buffer, device.data + static_cast<std::size_t>(firstBlock * device.media.blockSize), static_cast<std::size_t>(bytes));
        return Baleen::Uefi::kStatusSuccess;
    }

    // 假协议的写：只读介质按固件语义返回写保护，其余核对与读一致
    EfiStatus BALEEN_EFIAPI FakeWriteBlocks(BlockIo* self, uint32_t mediaId, EfiLba lba, uint64_t bytes, const void* buffer) {
        FakeDevice& device = *reinterpret_cast<FakeDevice*>(self);
        ++device.writes;
        device.lastMediaId = mediaId;
        device.lastLba = lba;
        device.lastBytes = bytes;
        device.lastBuffer = buffer;
        if (device.writeStatus != Baleen::Uefi::kStatusSuccess) return device.writeStatus;
        if (mediaId != device.media.mediaId) return Baleen::Uefi::kStatusMediaChanged;
        if (device.media.mediaPresent == 0) return Baleen::Uefi::kStatusNoMedia;
        if (device.media.readOnly != 0) return Baleen::Uefi::kStatusWriteProtected;
        if (bytes == 0 || (bytes % device.media.blockSize) != 0) return Baleen::Uefi::kStatusBadBufferSize;
        if (!MediumBufferAligned(device, buffer)) return Baleen::Uefi::kStatusInvalidParameter;
        const uint64_t firstBlock = lba;
        const uint64_t blocks = bytes / device.media.blockSize;
        if (firstBlock > device.media.lastBlock || blocks > (device.media.lastBlock - firstBlock) + 1) {
            return Baleen::Uefi::kStatusInvalidParameter;
        }
        std::memcpy(device.data + static_cast<std::size_t>(firstBlock * device.media.blockSize), buffer, static_cast<std::size_t>(bytes));
        return Baleen::Uefi::kStatusSuccess;
    }

    // 假协议的刷新：只回报脚本化状态，用来核对状态到错误的映射
    EfiStatus BALEEN_EFIAPI FakeFlushBlocks(BlockIo* self, uint32_t mediaId) {
        FakeDevice& device = *reinterpret_cast<FakeDevice*>(self);
        ++device.flushes;
        device.lastMediaId = mediaId;
        return device.flushStatus;
    }

    // 初始化假设备的事实与入口：512 字节块、128 块、可写、在位、不要求对齐
    void InitDevice(FakeDevice& device) {
        device.blockIo.revision = Baleen::Uefi::kBlockIoRevision2;   // FlushBlocks 从 REVISION2 起才有
        device.blockIo.media = &device.media;
        device.blockIo.reset = &FakeReset;
        device.blockIo.readBlocks = &FakeReadBlocks;
        device.blockIo.writeBlocks = &FakeWriteBlocks;
        device.blockIo.flushBlocks = &FakeFlushBlocks;
        device.media.mediaId = 1;
        device.media.removableMedia = 0;
        device.media.mediaPresent = 1;
        device.media.logicalPartition = 0;
        device.media.readOnly = 0;
        device.media.writeCaching = 1;
        device.media.blockSize = kBlockBytes;
        device.media.ioAlign = 0;
        device.media.lastBlock = kBlockCount - 1;
        device.media.lowestAlignedLba = 0;
        device.media.logicalBlocksPerPhysicalBlock = 1;
        device.media.optimalTransferLengthGranularity = 0;
    }

    // 介质字节的确定模式：第 i 字节是 seed + (i & 0xFF)，读回时按同一式核对
    uint8_t MediumByte(uint8_t seed, uint64_t offset) { return static_cast<uint8_t>(seed + (offset & 0xFF)); }

    // 用确定模式填满模拟介质
    void FillMedium(FakeDevice& device, uint8_t seed) {
        for (uint32_t i = 0; i < kMediumBytes; ++i) device.data[i] = MediumByte(seed, i);
    }

    // 清空动作计数与最近一次动作的记录，便于核对某个动作是否触及协议
    void ResetCounters(FakeDevice& device) {
        device.reads = 0;
        device.writes = 0;
        device.flushes = 0;
        device.lastMediaId = 0;
        device.lastLba = 0;
        device.lastBytes = 0;
        device.lastBuffer = nullptr;
    }

    // 核对一段缓冲与介质模式一致
    void ExpectBytes(const uint8_t* actual, uint8_t seed, uint64_t offset, uint32_t bytes, const std::string& what) {
        for (uint32_t i = 0; i < bytes; ++i) {
            if (actual[i] != MediumByte(seed, offset + i)) {
                Fail(what + "：第 " + std::to_string(i) + " 字节不符");
            }
        }
    }

    // 核对未绑定实例的全部事实
    void ExpectUnbound(const EfiBlockDevice& device, const std::string& what) {
        Require(!device.Bound(), what + "：不应留下绑定");
        Require(device.BlockBytes() == 0, what + "：块大小应为零");
        Require(device.CapacityBytes() == 0, what + "：容量应为零");
        Require(!device.Writable(), what + "：不应可写");
        Require(!device.MediaPresent(), what + "：介质不应在位");
        Require(device.MediaId() == 0, what + "：介质标识应为零");
        Require(device.IoAlign() == 0, what + "：对齐要求应为零");
        Require(!device.Removable(), what + "：不应报告可移动");
    }

    // 用例：绑定空协议、协议没有介质，以及失败绑定把旧事实清掉
    void CaseBindNull() {
        EfiBlockDevice device;
        ExpectUnbound(device, "默认构造");
        Require(device.Bind(nullptr) == Storage::Error::InvalidArgument, "空协议应报参数非法");
        ExpectUnbound(device, "空协议");
        FakeDevice fake;
        InitDevice(fake);
        fake.blockIo.media = nullptr;
        Require(device.Bind(&fake.blockIo) == Storage::Error::InvalidArgument, "协议没有介质应报参数非法");
        ExpectUnbound(device, "协议没有介质");
        fake.blockIo.media = &fake.media;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "合法协议应绑定成功");
        Require(device.Bound(), "绑定后应为已绑定");
        Require(device.Bind(nullptr) == Storage::Error::InvalidArgument, "再次空绑定应报参数非法");
        ExpectUnbound(device, "失败绑定之后");
    }

    // 用例：介质不在位时拒绝绑定，插回后可绑定
    void CaseBindNoMedia() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.mediaPresent = 0;
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::NotFound, "介质不在位应报 NotFound");
        ExpectUnbound(device, "介质不在位");
        fake.media.mediaPresent = 1;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "介质插回应能绑定");
        Require(device.MediaPresent(), "绑定后应报告介质在位");
    }

    // 用例：块大小为零或不是 2 的幂时拒绝绑定
    void CaseBindBlockSize() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        for (const uint32_t blockBytes : {0u, 3u, 500u, 768u, 1000u}) {
            fake.media.blockSize = blockBytes;
            Require(device.Bind(&fake.blockIo) == Storage::Error::NotSupported, "非法块大小应报 NotSupported：" + std::to_string(blockBytes));
            ExpectUnbound(device, "非法块大小 " + std::to_string(blockBytes));
        }
        for (const uint32_t blockBytes : {1u, 512u, 4096u}) {
            fake.media.blockSize = blockBytes;
            Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "2 的幂块大小应能绑定：" + std::to_string(blockBytes));
            Require(device.BlockBytes() == blockBytes, "块大小应取自介质事实");
        }
    }

    // 用例：对齐要求超过实现上限 4096 时拒绝绑定，边界值 4096 接受
    void CaseBindIoAlign() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        for (const uint32_t ioAlign : {4097u, 8192u, 65536u}) {
            fake.media.ioAlign = ioAlign;
            Require(device.Bind(&fake.blockIo) == Storage::Error::NotSupported, "超上限对齐应报 NotSupported：" + std::to_string(ioAlign));
            ExpectUnbound(device, "超上限对齐 " + std::to_string(ioAlign));
        }
        for (const uint32_t ioAlign : {0u, 1u, 512u, kTransferBytes}) {
            fake.media.ioAlign = ioAlign;
            Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "上限内对齐应能绑定：" + std::to_string(ioAlign));
            Require(device.IoAlign() == ioAlign, "对齐要求应取自介质事实");
        }
    }

    // 用例：绑定成功的事实，以及重新绑定覆盖旧事实
    void CaseBindFacts() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.mediaId = 0x12345678;
        fake.media.blockSize = 512;
        fake.media.lastBlock = 127;
        fake.media.ioAlign = 512;
        fake.media.readOnly = 1;
        fake.media.removableMedia = 1;
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "合法协议应绑定成功");
        Require(device.Bound(), "绑定后应为已绑定");
        Require(device.BlockBytes() == 512, "块大小应取自介质事实");
        Require(device.CapacityBytes() == 65536, "容量应为 (lastBlock + 1) * 块大小");
        Require(!device.Writable(), "只读介质不应可写");
        Require(device.MediaPresent(), "介质应在位");
        Require(device.IoAlign() == 512, "对齐要求应取自介质事实");
        Require(device.MediaId() == 0x12345678, "介质标识应取自介质事实");
        Require(device.Removable(), "可移动介质应报告可移动");
        // 重新绑定：事实随新介质更新，不应留着上一份
        fake.media.mediaId = 7;
        fake.media.blockSize = 1024;
        fake.media.lastBlock = 63;
        fake.media.ioAlign = 0;
        fake.media.readOnly = 0;
        fake.media.removableMedia = 0;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "重新绑定应成功");
        Require(device.BlockBytes() == 1024 && device.CapacityBytes() == 65536, "重新绑定后块大小与容量应更新");
        Require(device.Writable() && device.IoAlign() == 0 && device.MediaId() == 7 && !device.Removable(), "重新绑定后其余事实应更新");
    }

    // 用例：lastBlock 取最大时容量无法用 64 位表示，绑定时就拒绝而不是回绕成零容量
    void CaseBindCapacityWrap() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.lastBlock = ~static_cast<EfiLba>(0);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::NotSupported, "容量无法表示时应拒绝绑定");
        Require(!device.Bound() && device.CapacityBytes() == 0, "拒绝后不应留下绑定");
        uint8_t buffer[kBlockBytes] = {};
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::NotSupported, "未绑定时的读应报不支持");
        Require(device.Write(0, buffer, kBlockBytes) == Storage::Error::NotSupported, "未绑定时的写应报不支持");
        Require(fake.reads == 0 && fake.writes == 0, "拒绝不应触及协议");
    }

    // 用例：未绑定的读、写与刷新一律报不支持，参数校验不参与
    void CaseReadUnbound() {
        EfiBlockDevice device;
        uint8_t buffer[kBlockBytes] = {};
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::NotSupported, "未绑定时读应报不支持");
        Require(device.Read(0, nullptr, kBlockBytes) == Storage::Error::NotSupported, "未绑定时空缓冲也报不支持");
        Require(device.Read(256, buffer, 0) == Storage::Error::NotSupported, "未绑定时零长度也报不支持");
        Require(device.Write(0, buffer, kBlockBytes) == Storage::Error::NotSupported, "未绑定时写应报不支持");
        Require(device.Flush() == Storage::Error::NotSupported, "未绑定时刷新应报不支持");
    }

    // 用例：读的参数校验与容量边界
    void CaseReadChecks() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "合法协议应绑定成功");
        ResetCounters(fake);
        const uint64_t capacity = device.CapacityBytes();
        uint8_t buffer[1024] = {};
        Require(device.Read(0, nullptr, kBlockBytes) == Storage::Error::InvalidArgument, "空缓冲应报参数非法");
        Require(device.Read(0, buffer, 0) == Storage::Error::InvalidArgument, "零长度应报参数非法");
        Require(device.Read(256, buffer, kBlockBytes) == Storage::Error::InvalidArgument, "偏移未按块对齐应报参数非法");
        Require(device.Read(0, buffer, 513) == Storage::Error::InvalidArgument, "长度未按块对齐应报参数非法");
        Require(device.Read(0, buffer, 256) == Storage::Error::InvalidArgument, "长度不是块的整数倍应报参数非法");
        Require(device.Read(capacity, buffer, kBlockBytes) == Storage::Error::InvalidArgument, "起点等于容量应报参数非法");
        Require(device.Read(capacity - kBlockBytes, buffer, 1024) == Storage::Error::InvalidArgument, "区间越过容量应报参数非法");
        Require(device.Read(0, buffer, kMediumBytes + kBlockBytes) == Storage::Error::InvalidArgument, "整段加一块应报参数非法");
        Require(device.Read(~static_cast<uint64_t>(kBlockBytes - 1), buffer, kBlockBytes) == Storage::Error::InvalidArgument, "近顶偏移应报参数非法且不回绕");
        Require(fake.reads == 0, "全部拒绝都不应触及协议");
        Require(device.Read(capacity - kBlockBytes, buffer, kBlockBytes) == Storage::Error::Ok, "最后一块应可读");
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::Ok, "首块应可读");
        Require(fake.reads == 2, "两次合法读应各下发一次");
    }

    // 用例：读到的内容与块号换算
    void CaseReadContent() {
        FakeDevice fake;
        InitDevice(fake);
        FillMedium(fake, 0x40);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "合法协议应绑定成功");
        ResetCounters(fake);
        uint8_t buffer[1024] = {};
        Require(device.Read(3 * kBlockBytes, buffer, 1024) == Storage::Error::Ok, "对齐读应成功");
        ExpectBytes(buffer, 0x40, 3 * kBlockBytes, 1024, "读到的内容");
        Require(fake.reads == 1, "一次读应只下发一次");
        Require(fake.lastLba == 3, "块号应由偏移除以块大小得到");
        Require(fake.lastBytes == 1024, "下发长度应与请求一致");
        Require(fake.lastMediaId == fake.media.mediaId, "下发的介质标识应是绑定时的那个");
        uint8_t whole[kMediumBytes] = {};
        Require(device.Read(0, whole, kMediumBytes) == Storage::Error::Ok, "整段读应成功");
        ExpectBytes(whole, 0x40, 0, kMediumBytes, "整段读到的内容");
        Require(fake.reads == 2, "整段读应再下发一次");
    }

    // 用例：介质标识被换掉之后读、写与刷新都报 NotFound 且不再下发
    void CaseReadMediaChanged() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "合法协议应绑定成功");
        ResetCounters(fake);
        uint8_t buffer[kBlockBytes] = {};
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::Ok, "介质未换时读应成功");
        Require(fake.reads == 1, "读应下发一次");
        fake.media.mediaId = 2;
        Require(device.MediaPresent(), "介质仍在位");
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::NotFound, "介质标识变化后读应报 NotFound");
        Require(fake.reads == 1, "介质标识不符不应下发读");
        Require(device.Write(0, buffer, kBlockBytes) == Storage::Error::NotFound, "介质标识变化后写应报 NotFound");
        Require(fake.writes == 0, "介质标识不符不应下发写");
        Require(device.Flush() == Storage::Error::NotFound, "介质标识变化后刷新应报 NotFound");
        Require(fake.flushes == 0, "介质标识不符不应下发刷新");
        Require(device.MediaId() == 1, "MediaId 报告的是绑定时的标识，不是当前介质标识");
    }

    // 用例：写的只读拒绝与参数校验
    void CaseWriteChecks() {
        FakeDevice readOnly;
        InitDevice(readOnly);
        readOnly.media.readOnly = 1;
        EfiBlockDevice device;
        Require(device.Bind(&readOnly.blockIo) == Storage::Error::Ok, "只读介质应能绑定");
        Require(!device.Writable(), "只读介质不应可写");
        uint8_t buffer[1024] = {};
        Require(device.Write(0, nullptr, kBlockBytes) == Storage::Error::InvalidArgument, "空缓冲先按参数非法拒绝");
        Require(device.Write(256, buffer, kBlockBytes) == Storage::Error::InvalidArgument, "未对齐的写先按参数非法拒绝");
        Require(device.Write(0, buffer, kBlockBytes) == Storage::Error::ReadOnly, "参数合法时只读介质报 ReadOnly");
        Require(readOnly.writes == 0, "只读拒绝不应触及协议");
        // 可写介质的参数校验
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice writable;
        Require(writable.Bind(&fake.blockIo) == Storage::Error::Ok, "可写介质应能绑定");
        const uint64_t capacity = writable.CapacityBytes();
        ResetCounters(fake);
        Require(writable.Write(0, nullptr, kBlockBytes) == Storage::Error::InvalidArgument, "空缓冲应报参数非法");
        Require(writable.Write(0, buffer, 0) == Storage::Error::InvalidArgument, "零长度应报参数非法");
        Require(writable.Write(256, buffer, kBlockBytes) == Storage::Error::InvalidArgument, "偏移未按块对齐应报参数非法");
        Require(writable.Write(0, buffer, 513) == Storage::Error::InvalidArgument, "长度未按块对齐应报参数非法");
        Require(writable.Write(capacity, buffer, kBlockBytes) == Storage::Error::InvalidArgument, "起点等于容量应报参数非法");
        Require(writable.Write(capacity - kBlockBytes, buffer, 1024) == Storage::Error::InvalidArgument, "区间越过容量应报参数非法");
        Require(fake.writes == 0, "全部拒绝都不应触及协议");
    }

    // 用例：写的内容落到模拟介质并能原样读回
    void CaseWriteContent() {
        FakeDevice fake;
        InitDevice(fake);
        FillMedium(fake, 0);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可写介质应能绑定");
        uint8_t pattern[1024];
        for (uint32_t i = 0; i < 1024; ++i) pattern[i] = static_cast<uint8_t>(0xA0 + (i & 0x3F));
        ResetCounters(fake);
        Require(device.Write(4 * kBlockBytes, pattern, 1024) == Storage::Error::Ok, "对齐写应成功");
        Require(fake.writes == 1, "一次写应只下发一次");
        Require(fake.lastLba == 4, "块号应由偏移除以块大小得到");
        Require(fake.lastBytes == 1024, "下发长度应与请求一致");
        Require(fake.lastMediaId == fake.media.mediaId, "下发的介质标识应是绑定时的那个");
        Require(std::memcmp(fake.data + 4 * kBlockBytes, pattern, 1024) == 0, "写应落到介质的对应偏移");
        Require(fake.data[4 * kBlockBytes - 1] == MediumByte(0, 4 * kBlockBytes - 1), "起始之前不应被改动");
        uint8_t back[1024] = {};
        Require(device.Read(4 * kBlockBytes, back, 1024) == Storage::Error::Ok, "写后读回应成功");
        Require(std::memcmp(back, pattern, 1024) == 0, "读回内容应与写入一致");
        Require(device.Write(device.CapacityBytes() - kBlockBytes, pattern, kBlockBytes) == Storage::Error::Ok, "最后一块应可写");
    }

    // 用例：刷新返回值到错误的映射，以及介质指针消失时的核对
    void CaseFlushMapping() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可写介质应能绑定");
        ResetCounters(fake);
        fake.flushStatus = Baleen::Uefi::kStatusSuccess;
        Require(device.Flush() == Storage::Error::Ok, "协议成功应报 Ok");
        fake.flushStatus = Baleen::Uefi::kStatusUnsupported;
        // 固件对该服务报"不支持"= 这块介质没有刷新原语，按"无缓存"通过：
        // 与 BIOS 侧同一口径，把它当刷盘失败会让这类固件上永远做不出状态转换
        Require(device.Flush() == Storage::Error::Ok, "不受支持按无缓存通过");
        fake.flushStatus = Baleen::Uefi::kStatusDeviceError;
        Require(device.Flush() == Storage::Error::Io, "设备错误应报 Io");
        fake.flushStatus = Baleen::Uefi::kStatusNoMedia;
        Require(device.Flush() == Storage::Error::NotFound, "无介质应报 NotFound");
        fake.flushStatus = Baleen::Uefi::kStatusWriteProtected;
        Require(device.Flush() == Storage::Error::ReadOnly, "写保护应报 ReadOnly");
        fake.flushStatus = Baleen::Uefi::kStatusNotReady;
        Require(device.Flush() == Storage::Error::Io, "未就绪按底层出错报 Io");
        Require(fake.flushes == 6, "每次刷新都应下发一次");
        Require(fake.lastMediaId == fake.media.mediaId, "下发的介质标识应是绑定时的那个");
        fake.blockIo.media = nullptr;
        Require(device.Flush() == Storage::Error::NotFound, "介质指针消失应报 NotFound");
        Require(fake.flushes == 6, "介质指针消失不应下发");
    }

    // 用例：老协议版本（0x00010000）没有 FlushBlocks——不能碰那个成员，按"无缓存"通过
    void CaseLegacyRevisionHasNoFlush() {
        FakeDevice fake;
        InitDevice(fake);
        fake.blockIo.revision = 0x00010000;   // EFI 1.10 时代：结构体里没有 FlushBlocks
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "老版本协议也应能绑定");
        Require(!device.HasFlushPrimitive(), "老版本协议必须报没有刷新原语");
        ResetCounters(fake);
        Require(device.Flush() == Storage::Error::Ok, "没有原语时按无缓存通过（与 BIOS 侧同一口径）");
        Require(fake.flushes == 0, "老版本协议下绝不能调用 FlushBlocks（那个成员不存在）");
    }

    // 用例：协议有 FlushBlocks 但固件对本次刷新报 Unsupported——同样按"无缓存"通过
    void CaseUnsupportedFlushIsNoCache() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可写介质应能绑定");
        Require(device.HasFlushPrimitive(), "REVISION2 起应报有刷新原语");
        ResetCounters(fake);
        fake.flushStatus = Baleen::Uefi::kStatusUnsupported;
        Require(device.Flush() == Storage::Error::Ok, "固件拒绝刷新按无缓存通过");
        Require(fake.flushes == 1, "该服务存在时应下发一次");
        // 真正的设备错误仍然按失败上报：那是"刷了但没成功"，与"没有原语"不是一回事
        fake.flushStatus = Baleen::Uefi::kStatusDeviceError;
        Require(device.Flush() == Storage::Error::Io, "设备错误必须按 Io 上报");
    }

    // 用例：只读介质没有写缓存要刷——不声称刷新原语，刷新按只读拒绝且不下发固件调用
    void CaseFlushReadOnly() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.readOnly = 1;
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "只读介质应能绑定");
        Require(!device.Writable(), "只读介质不应可写");
        Require(!device.HasFlushPrimitive(), "只读介质不应声称有刷新原语");
        ResetCounters(fake);
        Require(device.Flush() == Storage::Error::ReadOnly, "只读介质刷新应报 ReadOnly");
        Require(fake.flushes == 0, "只读介质不应下发刷新调用");
        // 老协议 + 只读：只读判定先于版本判定，仍按只读拒绝
        FakeDevice legacy;
        InitDevice(legacy);
        legacy.blockIo.revision = 0x00010000;
        legacy.media.readOnly = 1;
        EfiBlockDevice legacyDevice;
        Require(legacyDevice.Bind(&legacy.blockIo) == Storage::Error::Ok, "老协议只读介质也应能绑定");
        Require(!legacyDevice.HasFlushPrimitive(), "老协议只读介质不应声称有刷新原语");
        ResetCounters(legacy);
        Require(legacyDevice.Flush() == Storage::Error::ReadOnly, "只读判定应先于版本判定");
        Require(legacy.flushes == 0, "老协议只读介质不应下发刷新调用");
    }

    // 用例：REV2 可写固件对 FlushBlocks 报 Unsupported——按无缓存通过，随后撤回刷新原语声称
    // 选择器提交状态时按 HasFlushPrimitive 报 E-CORE-71 / E-UEFI-71，而不是按 Flush 的返回值
    void CaseFlushUnsupportedWithdraws() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可写介质应能绑定");
        Require(device.HasFlushPrimitive(), "REV2 可写介质应先声称有刷新原语");
        fake.flushStatus = Baleen::Uefi::kStatusUnsupported;
        ResetCounters(fake);
        Require(device.Flush() == Storage::Error::Ok, "固件报不受支持按无缓存通过");
        Require(fake.flushes == 1, "固件报不受支持之前应下发一次");
        Require(!device.HasFlushPrimitive(), "固件报过不受支持之后不应再声称有刷新原语");
        // 第二次刷新仍按无缓存通过：Flush 的返回值说明不了持久化，能力要问 HasFlushPrimitive
        Require(device.Flush() == Storage::Error::Ok, "再次刷新仍按无缓存通过");
        Require(fake.flushes == 2, "第二次刷新仍会下发固件调用");
        Require(!device.HasFlushPrimitive(), "重复报不受支持不应恢复刷新原语声称");
        // 固件改口说成功：成功一次就恢复声称（以实际能力为准）
        fake.flushStatus = Baleen::Uefi::kStatusSuccess;
        Require(device.Flush() == Storage::Error::Ok, "固件刷成功应报 Ok");
        Require(device.HasFlushPrimitive(), "刷新成功一次后应恢复刷新原语声称");
    }

    // 用例：对齐要求 4096 时未对齐缓冲的读经中转，内容与对齐读一致
    void CaseReadBounce() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.ioAlign = kTransferBytes;
        FillMedium(fake, 0x80);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可绑定");
        Require(device.IoAlign() == kTransferBytes, "对齐要求应为 4096");
        alignas(kTransferBytes) uint8_t aligned[1024] = {};
        alignas(kTransferBytes) uint8_t storage[512 + 1024] = {};
        uint8_t* unaligned = storage + 512;
        Require((reinterpret_cast<uintptr_t>(unaligned) % kTransferBytes) != 0, "用例自身构造有误");
        ResetCounters(fake);
        Require(device.Read(0, aligned, 1024) == Storage::Error::Ok, "对齐缓冲应直读");
        Require((reinterpret_cast<uintptr_t>(fake.lastBuffer) % kTransferBytes) == 0, "直读下发的缓冲应对齐");
        Require(device.Read(0, unaligned, 1024) == Storage::Error::Ok, "未对齐缓冲应经中转读成功");
        Require((reinterpret_cast<uintptr_t>(fake.lastBuffer) % kTransferBytes) == 0, "中转下发的缓冲应对齐");
        Require(fake.lastBytes == 1024, "中转下发的长度应与请求一致");
        Require(std::memcmp(aligned, unaligned, 1024) == 0, "中转读的内容应与直读一致");
        ExpectBytes(unaligned, 0x80, 0, 1024, "中转读到的内容");
    }

    // 用例：中转按 4096 上限切成多次下发，跨多次下发的整段内容仍然一致
    void CaseReadBounceSplit() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.ioAlign = kTransferBytes;
        FillMedium(fake, 0x55);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可绑定");
        alignas(kTransferBytes) uint8_t storage[10240 + 64] = {};
        uint8_t* unaligned = storage + 64;
        const uint32_t bytes = 10240;
        ResetCounters(fake);
        Require(device.Read(2 * kBlockBytes, unaligned, bytes) == Storage::Error::Ok, "跨多次下发的读应成功");
        Require(fake.reads == 3, "10240 字节应切成三次下发");
        Require(fake.lastLba == (2 * kBlockBytes + 8192) / kBlockBytes, "最后一次下发的块号应随偏移推进");
        Require(fake.lastBytes == 2048, "最后一次下发的长度应是余下的 2048 字节");
        Require((reinterpret_cast<uintptr_t>(fake.lastBuffer) % kTransferBytes) == 0, "每次下发的缓冲都应对齐");
        ExpectBytes(unaligned, 0x55, 2 * kBlockBytes, bytes, "跨多次下发读到的内容");
    }

    // 用例：对齐要求 4096 时未对齐缓冲的写经中转，介质内容与源缓冲一致
    void CaseWriteBounce() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.ioAlign = kTransferBytes;
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可绑定");
        alignas(kTransferBytes) uint8_t storage[10240 + 128] = {};
        uint8_t* unaligned = storage + 128;
        for (uint32_t i = 0; i < 10240; ++i) unaligned[i] = static_cast<uint8_t>(i * 7 + 3);
        ResetCounters(fake);
        Require(device.Write(1024, unaligned, 10240) == Storage::Error::Ok, "未对齐缓冲的写应经中转成功");
        Require(fake.writes == 3, "10240 字节应切成三次下发");
        Require((reinterpret_cast<uintptr_t>(fake.lastBuffer) % kTransferBytes) == 0, "中转下发的缓冲应对齐");
        Require(std::memcmp(fake.data + 1024, unaligned, 10240) == 0, "写应落到介质的对应偏移");
        Require(fake.data[1023] == 0 && fake.data[1024 + 10240] == 0, "写入区间之外不应被改动");
        uint8_t back[10240] = {};
        Require(device.Read(1024, back, 10240) == Storage::Error::Ok, "写后读回应成功");
        Require(std::memcmp(back, unaligned, 10240) == 0, "读回内容应与写入一致");
    }

    // 用例：介质在两次调用之间被拔掉，插回后不必重新绑定
    void CaseMediaRemoved() {
        FakeDevice fake;
        InitDevice(fake);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::Ok, "可绑定");
        ResetCounters(fake);
        uint8_t buffer[kBlockBytes] = {};
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::Ok, "在位时读应成功");
        fake.media.mediaPresent = 0;
        Require(!device.MediaPresent(), "拔出后应报告介质不在位");
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::NotFound, "拔出后读应报 NotFound");
        Require(fake.reads == 1, "拔出后不应下发读");
        Require(device.Write(0, buffer, kBlockBytes) == Storage::Error::NotFound, "拔出后写应报 NotFound");
        Require(fake.writes == 0, "拔出后不应下发写");
        // 刷新与读写同一条核对：介质不在位时不下发
        Require(device.Flush() == Storage::Error::NotFound, "拔出后刷新应报 NotFound");
        Require(fake.flushes == 0, "拔出后不应下发刷新");
        fake.media.mediaPresent = 1;
        Require(device.MediaPresent(), "插回后应报告介质在位");
        Require(device.Read(0, buffer, kBlockBytes) == Storage::Error::Ok, "插回后读应恢复");
        Require(device.Flush() == Storage::Error::Ok, "插回后刷新应恢复");
    }

    // 用例：块大小超过中转缓冲上限 4096 的设备在绑定时就被拒绝，不留到未对齐调用才失败
    void CaseLargeBlockBounce() {
        FakeDevice fake;
        InitDevice(fake);
        fake.media.blockSize = 8192;
        fake.media.lastBlock = 7;
        fake.media.ioAlign = kTransferBytes;
        FillMedium(fake, 0x20);
        EfiBlockDevice device;
        Require(device.Bind(&fake.blockIo) == Storage::Error::NotSupported, "块大小超过中转上限应拒绝绑定");
        Require(!device.Bound() && device.BlockBytes() == 0, "拒绝后不应留下绑定");
        alignas(kTransferBytes) uint8_t storage[64 + 8192] = {};
        Require(device.Read(0, storage, 8192) == Storage::Error::NotSupported, "未绑定时的读应报不支持");
        Require(fake.reads == 0, "拒绝之后不应触及协议");
    }

    // ———— 固件内存图的假启动服务 ————

    // 假池分配与假内存图的状态：函数指针按脚本填，用例按这里留下的记录核对
    struct FakeMemoryServices {
        MemoryDescriptor descriptors[16] = {};                 // 假内存图的描述符数组
        uint64_t descriptorCount = 0;                          // 已装入的描述符条数
        uint64_t mapKey = 0;                                   // 成功取图时回报的 MapKey
        uint64_t descriptorBytes = sizeof(MemoryDescriptor);   // 回报的单条描述符长度
        uint32_t descriptorVersion = 1;                        // 回报的描述符版本
        uint64_t retryBytes = 0;                               // 脚本化的首次「缓冲太小」所需字节数
        bool retryOnce = false;                                // 是否还欠一次「缓冲太小」
        bool allocationFails = false;                          // 脚本开关：池分配一律失败
        uint32_t getMapCalls = 0;                              // getMemoryMap 收到的调用次数
        uint32_t allocateCalls = 0;                            // allocatePool 收到的调用次数
        uint32_t freeCalls = 0;                                // freePool 收到的调用次数
        uint32_t lastPoolType = 0;                             // 最近一次分配请求的池类型
        uint64_t lastAllocateBytes = 0;                        // 最近一次分配请求的字节数
        void* firstAllocated = nullptr;                        // 第一次成功分配的缓冲：核对确实被释放
        void* lastAllocated = nullptr;                         // 最近一次成功分配的缓冲
        void* lastFreed = nullptr;                             // 最近一次释放的缓冲
    };

    // 假启动服务背后的内存图与池分配状态
    FakeMemoryServices g_fakeMemory;

    // EFI_OUT_OF_RESOURCES：假池分配在脚本要求失败或宿主 malloc 失败时回报
    constexpr EfiStatus kFakeOutOfResources = Baleen::Uefi::kStatusErrorBit | 9;

    // 往假内存图里追加一条描述符：类型、物理起址与页数；虚拟起址与属性本阶段不读
    void AddDescriptor(uint32_t type, uint64_t base, uint64_t pages) {
        Require(g_fakeMemory.descriptorCount < sizeof(g_fakeMemory.descriptors) / sizeof(g_fakeMemory.descriptors[0]),
                "假内存图的描述符超出用例容量");
        MemoryDescriptor& descriptor = g_fakeMemory.descriptors[g_fakeMemory.descriptorCount++];
        descriptor.type = type;
        descriptor.reserved = 0;
        descriptor.physicalStart = base;
        descriptor.virtualStart = 0;
        descriptor.pages = pages;
        descriptor.attributes = 0;
    }

    // 把假内存图与池分配记录复位，每个用例都从同一份干净状态开始
    void ResetFakeMemory() { g_fakeMemory = FakeMemoryServices{}; }

    // 假取图：脚本先回报一次「缓冲太小」并写出所需字节数，之后把描述符数组拷进调用方缓冲；
    // 成功时写回 MapKey、描述符长度与版本，描述符长度可由脚本改成不合规值
    EfiStatus BALEEN_EFIAPI FakeGetMemoryMap(uint64_t* mapBytes, MemoryDescriptor* map, uint64_t* mapKey,
                                             uint64_t* descriptorBytes, uint32_t* descriptorVersion) {
        ++g_fakeMemory.getMapCalls;
        if (g_fakeMemory.retryOnce) {
            g_fakeMemory.retryOnce = false;
            *mapBytes = g_fakeMemory.retryBytes;
            *descriptorBytes = sizeof(MemoryDescriptor);
            return Baleen::Uefi::kStatusBufferTooSmall;
        }
        const uint64_t actualBytes = g_fakeMemory.descriptorCount * sizeof(MemoryDescriptor);
        if (*mapBytes < actualBytes) {
            *mapBytes = actualBytes;
            *descriptorBytes = sizeof(MemoryDescriptor);
            return Baleen::Uefi::kStatusBufferTooSmall;
        }
        if (actualBytes != 0) std::memcpy(map, g_fakeMemory.descriptors, static_cast<std::size_t>(actualBytes));
        *mapBytes = actualBytes;
        *mapKey = g_fakeMemory.mapKey;
        *descriptorBytes = g_fakeMemory.descriptorBytes;
        *descriptorVersion = g_fakeMemory.descriptorVersion;
        return Baleen::Uefi::kStatusSuccess;
    }

    // 假池分配：用宿主堆代替固件池，记录调用次数、字节数与缓冲，脚本可让分配一律失败
    EfiStatus BALEEN_EFIAPI FakeAllocatePool(uint32_t poolType, uint64_t bytes, void** buffer) {
        ++g_fakeMemory.allocateCalls;
        g_fakeMemory.lastPoolType = poolType;
        g_fakeMemory.lastAllocateBytes = bytes;
        if (g_fakeMemory.allocationFails || buffer == nullptr) return kFakeOutOfResources;
        void* memory = std::malloc(static_cast<std::size_t>(bytes));
        if (memory == nullptr) return kFakeOutOfResources;
        if (g_fakeMemory.firstAllocated == nullptr) g_fakeMemory.firstAllocated = memory;
        g_fakeMemory.lastAllocated = memory;
        *buffer = memory;
        return Baleen::Uefi::kStatusSuccess;
    }

    // 假池释放：归还宿主堆，并记下缓冲以便核对旧缓冲确实被释放
    EfiStatus BALEEN_EFIAPI FakeFreePool(void* buffer) {
        ++g_fakeMemory.freeCalls;
        g_fakeMemory.lastFreed = buffer;
        std::free(buffer);
        return Baleen::Uefi::kStatusSuccess;
    }

    // 假启动服务表：只填本工具用到的三个成员，其余成员按规范顺序留空
    BootServices g_bootServices = {};

    // 装配假启动服务表：只给取图与池分配/释放赋值，别的成员本工具不碰
    void InitFakeBootServices() {
        g_bootServices.getMemoryMap = &FakeGetMemoryMap;
        g_bootServices.allocatePool = &FakeAllocatePool;
        g_bootServices.freePool = &FakeFreePool;
    }

    // 成功时应返回空诊断（code 为空）
    void ExpectNoDiagnostic(const Diagnostic& diagnostic, const std::string& what) {
        Require(diagnostic.code == nullptr, what + "：应返回空诊断");
    }

    // 失败编号应与预期一致；只比编号，文本允许润色
    void ExpectDiagnosticCode(const Diagnostic& diagnostic, const char* code, const std::string& what) {
        Require(diagnostic.code != nullptr && std::string_view(diagnostic.code) == code, what + "：应报 " + code);
    }

    // 核对转换后的一个条目：起址、按页换算的长度、可用标记与属性有效位
    void ExpectMemoryEntry(const Boot::MemoryMapEntry& entry, uint64_t base, uint64_t pages, uint32_t type, const std::string& what) {
        Require(entry.base == base, what + "：起址不符");
        Require(entry.length == pages * Baleen::Uefi::kPageBytes, what + "：长度应按页数换算成字节");
        Require(entry.type == type, what + "：可用标记不符");
        Require(entry.attributes == 1, what + "：属性应置有效位");
    }

    // 用例：同一份退出前的快照按两种视角转换——退出前只有常规内存可用，退出后启动服务与引导器内存归系统
    void CaseMemoryMapViews() {
        ResetFakeMemory();
        // 一条假描述符的预期：固件类型、物理起址与页数
        struct Region {
            uint32_t type;   // 固件内存类型
            uint64_t base;   // 物理起址
            uint64_t pages;  // 页数
        };
        constexpr Region regions[] = {
            {Baleen::Uefi::kMemoryConventional, 0x100000, 2},
            {Baleen::Uefi::kMemoryLoaderCode, 0x200000, 1},
            {Baleen::Uefi::kMemoryLoaderData, 0x300000, 1},
            {Baleen::Uefi::kMemoryBootServicesCode, 0x400000, 1},
            {Baleen::Uefi::kMemoryBootServicesData, 0x500000, 2},
            {5u, 0x600000, 1},    // 运行期服务代码：始终不可用
            {10u, 0x700000, 1},   // ACPI NVS：始终不可用
            {11u, 0x800000, 1},   // MMIO：始终不可用
        };
        // 退出启动服务之后各条目的可用标记：前五类归系统，其余始终不可用
        constexpr bool usableAfter[] = {true, true, true, true, true, false, false, false};
        constexpr uint32_t count = sizeof(regions) / sizeof(regions[0]);
        static_assert(sizeof(regions) / sizeof(regions[0]) == sizeof(usableAfter) / sizeof(usableAfter[0]), "两个预期数组的长度应一致");
        for (const Region& region : regions) AddDescriptor(region.type, region.base, region.pages);
        g_fakeMemory.mapKey = 0x1000;
        MemoryMap map;
        ExpectNoDiagnostic(map.Allocate(&g_bootServices), "分配取图缓冲");
        ExpectNoDiagnostic(map.Capture(&g_bootServices), "取内存图");
        // 退出启动服务之前：装载跨度必须落在常规内存上，启动服务与引导器内存这时还在固件手里
        Require(map.Convert(false) == count, "退出前视角应转换出全部条目");
        Require(map.Count() == count && !map.Truncated(), "退出前视角的条目数应与快照一致");
        const Boot::MemoryMapEntry* const entries = map.Entries();
        for (uint32_t i = 0; i < count; ++i) {
            ExpectMemoryEntry(entries[i], regions[i].base, regions[i].pages, i == 0 ? 1u : 2u, "退出前视角第 " + std::to_string(i) + " 条");
        }
        // 退出启动服务之后：启动服务与引导器内存都归系统；运行期服务、ACPI NVS 与 MMIO 仍不可用
        Require(map.Convert(true) == count, "退出后视角应转换出全部条目");
        for (uint32_t i = 0; i < count; ++i) {
            ExpectMemoryEntry(entries[i], regions[i].base, regions[i].pages, usableAfter[i] ? 1u : 2u, "退出后视角第 " + std::to_string(i) + " 条");
        }
        Require(g_fakeMemory.getMapCalls == 1, "两种视角都从同一份快照转换，不应再取图");
        Require(map.Key() == 0x1000, "取图成功应保留 MapKey");
        map.Release(&g_bootServices);
        Require(g_fakeMemory.freeCalls == 1, "用例结束应归还取图缓冲");
    }

    // 用例：首次取图报 BufferTooSmall——按固件回报的长度重新分配再取，成功时保留 MapKey 且旧缓冲被释放
    void CaseMemoryMapRetry() {
        ResetFakeMemory();
        // 脚本化的首次取图：回报的所需长度大于按条目上限加余量算出的初始缓冲
        g_fakeMemory.retryOnce = true;
        g_fakeMemory.retryBytes = 8192;
        g_fakeMemory.mapKey = 0x1234;
        for (uint32_t i = 0; i < 3; ++i) AddDescriptor(Baleen::Uefi::kMemoryConventional, 0x100000 + i * 0x100000, 1);
        MemoryMap map;
        ExpectNoDiagnostic(map.Allocate(&g_bootServices), "分配取图缓冲");
        Require(g_fakeMemory.allocateCalls == 1 && g_fakeMemory.lastPoolType == Baleen::Uefi::kPoolLoaderData, "取图缓冲应按引导器数据池分配");
        const uint64_t initialBytes = g_fakeMemory.lastAllocateBytes;
        void* const firstBuffer = g_fakeMemory.lastAllocated;
        Require(initialBytes < g_fakeMemory.retryBytes, "用例前提：固件回报的所需长度应大于初始缓冲");
        ExpectNoDiagnostic(map.Capture(&g_bootServices), "缓冲太小之后重新取图");
        Require(g_fakeMemory.getMapCalls == 2, "第一次取图报缓冲太小、第二次成功");
        Require(g_fakeMemory.allocateCalls == 2, "应按固件回报的长度重新分配一次取图缓冲");
        Require(g_fakeMemory.freeCalls == 1, "重新分配之前应释放旧缓冲");
        Require(g_fakeMemory.lastFreed == firstBuffer, "被释放的应就是第一次分配的缓冲");
        Require(g_fakeMemory.lastAllocateBytes > initialBytes, "重取用的缓冲应比初始缓冲大");
        Require(g_fakeMemory.lastAllocateBytes >= g_fakeMemory.retryBytes, "重取用的缓冲不应小于固件回报的所需长度");
        Require(map.Key() == 0x1234, "成功的那次取图应保留 MapKey");
        Require(map.Convert(false) == 3, "重取后的快照应能转换出全部条目");
        for (uint32_t i = 0; i < 3; ++i) {
            ExpectMemoryEntry(map.Entries()[i], 0x100000 + i * 0x100000, 1, 1, "重取后的第 " + std::to_string(i) + " 条");
        }
        map.Release(&g_bootServices);
        Require(g_fakeMemory.freeCalls == 2, "用例结束应连同新缓冲一起归还");
    }

    // 用例：空指针与格式非法一律拒绝——分配参数为空、未分配就取图、固件回报的描述符长度不合规
    void CaseMemoryMapReject() {
        ResetFakeMemory();
        MemoryMap map;
        ExpectDiagnosticCode(map.Allocate(nullptr), "E-UEFI-79", "分配时启动服务为空");
        ExpectDiagnosticCode(map.Capture(nullptr), "E-UEFI-79", "取图时启动服务为空");
        ExpectDiagnosticCode(map.Capture(&g_bootServices), "E-UEFI-79", "未分配取图缓冲就取图");
        // 池分配失败也归到「分配取图缓冲」这条编号
        g_fakeMemory.allocationFails = true;
        ExpectDiagnosticCode(map.Allocate(&g_bootServices), "E-UEFI-79", "池分配失败");
        g_fakeMemory.allocationFails = false;
        ExpectNoDiagnostic(map.Allocate(&g_bootServices), "恢复后重新分配取图缓冲");
        AddDescriptor(Baleen::Uefi::kMemoryConventional, 0x100000, 1);
        // 固件回报的单条描述符长度小于本阶段的结构：格式不受支持，不猜布局
        g_fakeMemory.descriptorBytes = 16;
        ExpectDiagnosticCode(map.Capture(&g_bootServices), "E-UEFI-81", "描述符长度小于 40 字节");
        // 恢复合规长度后，同一份缓冲还能继续取图
        g_fakeMemory.descriptorBytes = sizeof(MemoryDescriptor);
        ExpectNoDiagnostic(map.Capture(&g_bootServices), "格式恢复后取图");
        Require(map.Convert(false) == 1, "恢复后的快照应能正常转换");
        Require(map.Entries()[0].type == 1, "恢复后的常规内存应可用");
        map.Release(&g_bootServices);
        Require(g_fakeMemory.freeCalls == 1, "用例结束应归还取图缓冲");
    }

    // 跑全部用例并核对数量
    void RunCases() {
        InitFakeBootServices();
        Case("bind 空协议与空介质", CaseBindNull);
        Case("bind 介质不在位", CaseBindNoMedia);
        Case("bind 块大小非法", CaseBindBlockSize);
        Case("bind 对齐上限", CaseBindIoAlign);
        Case("bind 成功事实与重新绑定", CaseBindFacts);
        Case("bind 容量回绕", CaseBindCapacityWrap);
        Case("read 未绑定", CaseReadUnbound);
        Case("read 参数校验与容量边界", CaseReadChecks);
        Case("read 内容与块号换算", CaseReadContent);
        Case("read 介质标识变化", CaseReadMediaChanged);
        Case("write 只读与参数校验", CaseWriteChecks);
        Case("write 内容落到介质", CaseWriteContent);
        Case("flush 状态映射", CaseFlushMapping);
        Case("老协议版本没有刷新原语", CaseLegacyRevisionHasNoFlush);
        Case("固件拒绝刷新按无缓存通过", CaseUnsupportedFlushIsNoCache);
        Case("flush 只读介质无原语", CaseFlushReadOnly);
        Case("flush 固件不支持刷新后撤回声称", CaseFlushUnsupportedWithdraws);
        Case("read 对齐中转", CaseReadBounce);
        Case("read 对齐中转跨多次下发", CaseReadBounceSplit);
        Case("write 对齐中转", CaseWriteBounce);
        Case("介质拔出与插回", CaseMediaRemoved);
        Case("块大小超过中转上限", CaseLargeBlockBounce);
        Case("memorymap 退出前后两种视角", CaseMemoryMapViews);
        Case("memorymap 缓冲太小重取", CaseMemoryMapRetry);
        Case("memorymap 参数与格式拒绝", CaseMemoryMapReject);
        Require(g_ran == kCaseCount, "用例数量与登记不符");
        std::cout << "CheckEfiBlockDevice: " << g_ran << " 项用例全部通过" << std::endl;
    }

    // 打印用法
    void Usage() {
        std::cout << "CheckEfiBlockDevice：Baleen UEFI 块设备适配与固件内存图的宿主侧检查\n"
                     "用法：CheckEfiBlockDevice [选项]\n"
                     "  -h, --help      显示本说明\n"
                     "退出码：0 全部通过、1 有用例失败\n";
    }
}

// 进程入口：检查全部在内存里做，不需要输入路径
int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            if (argument == "-h" || argument == "--help") { Usage(); return 0; }
            Fail(std::string("未知选项：") + std::string(argument));
        }
        RunCases();
        std::cout << "CheckEfiBlockDevice: 全部检查通过" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckEfiBlockDevice: 失败：" << error.what() << std::endl;
        return 1;
    }
}
