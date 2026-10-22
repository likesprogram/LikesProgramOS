/* EfiTypes.hpp
    UEFI x64 固件应用用到的最小固件接口：表与协议的字段布局，以及协议标识

    只声明引导路径真正读到的成员，其余成员按规范顺序占位，不读也不写；
    布局是两侧 ABI 的一部分，用 static_assert 钉住偏移，规范一侧改动会让构建直接失败
    本头只为 x64 构建：指针 8 字节、UINTN 64 位、EFI_STATUS 64 位
    规范出处：UEFI Specification 2.10，第 4 章（表）、第 7 章（启动服务）、
    第 12 章（控制台协议与图形输出）、第 13 章（块设备与已加载映像协议）
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Baleen {
    namespace Uefi {
        // 固件接口的调用约定：UEFI 规范要求 EFIAPI 即 Microsoft x64 约定（前两个参数在 RCX 与 RDX）
        // 少标一处都会让固件在错误的寄存器里取参数并在固件代码里出异常，函数指针与入口都要标
        #define BALEEN_EFIAPI __attribute__((ms_abi))

        // EFI_STATUS：0 是成功，最高位置位是错误
        using EfiStatus = uint64_t;
        // EFI_HANDLE：不透明句柄
        using EfiHandle = void*;
        // EFI_LBA：逻辑块地址
        using EfiLba = uint64_t;
        // EFI_PHYSICAL_ADDRESS：物理地址
        using EfiPhysicalAddress = uint64_t;

        // 成功
        constexpr EfiStatus kStatusSuccess = 0;
        // 错误码的最高位
        constexpr EfiStatus kStatusErrorBit = 0x8000000000000000ull;
        // 参数非法
        constexpr EfiStatus kStatusInvalidParameter = kStatusErrorBit | 2;
        // 不受支持
        constexpr EfiStatus kStatusUnsupported = kStatusErrorBit | 3;
        // 缓冲大小不是块的整数倍，或取内存图时缓冲不够（此时固件回报所需长度）
        constexpr EfiStatus kStatusBadBufferSize = kStatusErrorBit | 4;
        // 缓冲太小：取内存图时要按固件回报的长度重新分配
        constexpr EfiStatus kStatusBufferTooSmall = kStatusErrorBit | 5;
        // 设备未就绪
        constexpr EfiStatus kStatusNotReady = kStatusErrorBit | 6;
        // 设备错误
        constexpr EfiStatus kStatusDeviceError = kStatusErrorBit | 7;
        // 写保护
        constexpr EfiStatus kStatusWriteProtected = kStatusErrorBit | 8;
        // 介质不在位
        constexpr EfiStatus kStatusNoMedia = kStatusErrorBit | 12;
        // 介质已更换
        constexpr EfiStatus kStatusMediaChanged = kStatusErrorBit | 13;
        // 找不到
        constexpr EfiStatus kStatusNotFound = kStatusErrorBit | 14;

        // 协议标识：16 字节原始字节序，前三个字段按小端存放
        struct Guid {
            uint8_t bytes[16];   // 原始字节
        };

        // 一个错误状态是否成立
        constexpr bool IsError(EfiStatus status) { return (status & kStatusErrorBit) != 0; }


        // EFI 表头：所有 EFI 表都以它开头，24 字节
        struct TableHeader {
            uint64_t signature;      // 表标记
            uint32_t revision;       // 规范版本
            uint32_t headerBytes;    // 表头长度
            uint32_t crc32;          // 表头校验
            uint32_t reserved;       // 保留
        };

        static_assert(sizeof(TableHeader) == 24, "EFI_TABLE_HEADER 不是 24 字节");

        // 简单文本输出协议：只需要 OutputString，前面的成员按规范占位
        struct SimpleTextOutput {
            void* reset;                                                     // 复位
            EfiStatus (BALEEN_EFIAPI *outputString)(SimpleTextOutput*, const char16_t*);    // 输出 UTF-16 字符串
        };

        static_assert(offsetof(SimpleTextOutput, outputString) == 8, "OutputString 的偏移已变");

        // —— 图形输出协议（GOP，UEFI 2.10 §12.9）：屏幕事实的唯一来源 ——
        // 像素格式：GOP 的 EFI_GRAPHICS_OUTPUT_PIXEL_FORMAT 取值，本阶段只支持前两种 32 位打包格式
        constexpr uint32_t kPixelFormatRedGreenBlueReserved8BitPerColor = 0;   // 小端字节序 R-G-B-X，对应 Boot::ScreenFormat::Rgbx8888
        constexpr uint32_t kPixelFormatBlueGreenRedReserved8BitPerColor = 1;   // 小端字节序 B-G-R-X，对应 Boot::ScreenFormat::Bgrx8888
        constexpr uint32_t kPixelFormatBitMask = 2;                            // 通道由位掩码描述，本阶段不支持
        constexpr uint32_t kPixelFormatBltOnly = 3;                            // 只能经 Blt 写、没有线性帧缓冲，本阶段不支持

        // 像素位掩码：PixelBitMask 格式下各通道在 32 位像素里的位段
        struct PixelBitmask {
            uint32_t redMask;        // 红色通道掩码
            uint32_t greenMask;      // 绿色通道掩码
            uint32_t blueMask;       // 蓝色通道掩码
            uint32_t reservedMask;   // 保留通道掩码
        };

        static_assert(sizeof(PixelBitmask) == 16, "EFI_PIXEL_BITMASK 不是 16 字节");

        // 位块传送的像素：B-G-R-X 顺序，本阶段不调用 Blt，只保留结构定义
        struct GraphicsOutputBltPixel {
            uint8_t blue;       // 蓝色分量
            uint8_t green;      // 绿色分量
            uint8_t red;        // 红色分量
            uint8_t reserved;   // 保留
        };

        static_assert(sizeof(GraphicsOutputBltPixel) == 4, "EFI_GRAPHICS_OUTPUT_BLT_PIXEL 不是 4 字节");

        // 图形输出模式的信息：一个模式的分辨率、像素格式与扫描行像素数，x64 下 36 字节且无隐式填充
        struct GraphicsOutputModeInformation {
            uint32_t version;                // 结构版本，0 表示本定义
            uint32_t horizontalResolution;   // 水平像素数
            uint32_t verticalResolution;     // 垂直像素数
            uint32_t pixelFormat;            // 像素格式，取值见 kPixelFormat* 常量
            PixelBitmask pixelInformation;   // PixelBitMask 格式下的通道掩码，其它格式固件写 0
            uint32_t pixelsPerScanLine;      // 每扫描行的像素数：行步长按它算，可能大于水平像素数
        };

        static_assert(sizeof(GraphicsOutputModeInformation) == 36, "EFI_GRAPHICS_OUTPUT_MODE_INFORMATION 不是 36 字节");
        static_assert(offsetof(GraphicsOutputModeInformation, horizontalResolution) == 4, "GOP 模式信息的水平分辨率偏移已变");
        static_assert(offsetof(GraphicsOutputModeInformation, pixelFormat) == 12, "GOP 模式信息的像素格式偏移已变");
        static_assert(offsetof(GraphicsOutputModeInformation, pixelsPerScanLine) == 32, "GOP 模式信息的扫描行像素数偏移已变");

        // 图形输出协议的当前模式：帧缓冲事实与模式信息指针；两份信息都是固件内存，退出启动服务后失效
        struct GraphicsOutputProtocolMode {
            uint32_t maxMode;                 // 模式编号上界，合法编号为 0 .. maxMode - 1
            uint32_t mode;                    // 当前模式编号
            GraphicsOutputModeInformation* info;   // 当前模式的信息
            uint64_t sizeOfInfo;              // 模式信息的字节数
            EfiPhysicalAddress frameBufferBase;    // 帧缓冲物理基址
            uint64_t frameBufferSize;         // 帧缓冲可写字节数
        };

        static_assert(offsetof(GraphicsOutputProtocolMode, info) == 8, "GOP 当前模式的信息指针偏移已变");
        static_assert(offsetof(GraphicsOutputProtocolMode, frameBufferBase) == 24, "GOP 当前模式的帧缓冲基址偏移已变");
        static_assert(offsetof(GraphicsOutputProtocolMode, frameBufferSize) == 32, "GOP 当前模式的可写字节数偏移已变");

        // 图形输出协议：本阶段只读 Mode 取屏幕事实，三个函数保留完整签名但本阶段不调用
        // Blt 的 operation 是 EFI_GRAPHICS_OUTPUT_BLT_OPERATION（枚举宽度 4 字节）
        struct GraphicsOutputProtocol {
            EfiStatus (BALEEN_EFIAPI *queryMode)(GraphicsOutputProtocol*, uint32_t, uint64_t*, GraphicsOutputModeInformation**);   // 查询模式信息，Info 由固件分配
            EfiStatus (BALEEN_EFIAPI *setMode)(GraphicsOutputProtocol*, uint32_t);                                                   // 切换模式
            EfiStatus (BALEEN_EFIAPI *blt)(GraphicsOutputProtocol*, GraphicsOutputBltPixel*, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);   // 位块传送
            GraphicsOutputProtocolMode* mode;   // 当前模式
        };

        static_assert(offsetof(GraphicsOutputProtocol, mode) == 24, "GOP 的 Mode 指针偏移已变");

        // 设备路径节点：类型、子类型与节点长度（含这 4 字节）；整条路径以结束节点收尾
        struct DevicePath {
            uint8_t type;        // 节点类型
            uint8_t subType;     // 节点子类型
            uint8_t length[2];   // 节点长度，小端，含本 4 字节
        };

        static_assert(sizeof(DevicePath) == 4, "设备路径节点头不是 4 字节");

        // 内存图条目：字段与固件的 40 字节描述符一一对应，x64 下无隐式填充
        struct MemoryDescriptor {
            uint32_t type;             // 内存类型，取值见下面的 kMemory* 常量
            uint32_t reserved;         // 填充，固件写 0
            EfiPhysicalAddress physicalStart;  // 物理起址
            EfiPhysicalAddress virtualStart;   // 虚拟起址，本阶段不使用
            uint64_t pages;            // 页数，每页 4096 字节
            uint64_t attributes;       // 属性位
        };

        static_assert(sizeof(MemoryDescriptor) == 40, "EFI_MEMORY_DESCRIPTOR 不是 40 字节");
        static_assert(offsetof(MemoryDescriptor, physicalStart) == 8, "内存描述符的 PhysicalStart 偏移已变");
        static_assert(offsetof(MemoryDescriptor, pages) == 24, "内存描述符的 NumberOfPages 偏移已变");

        // 池分配类型：只用 EfiLoaderData，退出启动服务之前由本阶段自己释放
        constexpr uint32_t kPoolLoaderData = 2;
        // 内存页大小
        constexpr uint64_t kPageBytes = 4096;
        // 低 4GiB 恒等映射的上界：内核只按低 4GiB 建恒等映射，交接缓冲与屏幕帧缓冲都要落在它之下
        constexpr uint64_t kLowMemoryLimit = 0x100000000ull;
        // 内存类型中，退出启动服务之后交给系统使用的那几类
        // 其余类型（运行期服务、ACPI NVS、MMIO、持久内存等）在交接时一律按不可用上报
        constexpr uint32_t kMemoryLoaderCode = 1;          // 引导器代码
        constexpr uint32_t kMemoryLoaderData = 2;          // 引导器数据
        constexpr uint32_t kMemoryBootServicesCode = 3;    // 启动服务代码
        constexpr uint32_t kMemoryBootServicesData = 4;    // 启动服务数据
        constexpr uint32_t kMemoryConventional = 7;        // 常规内存：固件自己也不占用的空闲 RAM

        // 页分配方式：按任意空闲地址分配、分配在给定地址之下，或只按指定物理地址分配
        // 内核镜像先读进按任意地址分配的固件缓冲；固定装载地址要等退出启动服务、
        // 固件内存交给系统之后才能写入；交接缓冲用 kAllocateMaxAddress 限制在低 4GiB
        constexpr uint32_t kAllocateAnyPages = 0;
        constexpr uint32_t kAllocateMaxAddress = 1;   // 分配在入参 *memory 给出的地址之下，含边界
        constexpr uint32_t kAllocateAddress = 2;

        // 结束节点的类型与子类型
        constexpr uint8_t kDevicePathEndType = 0x7F;
        constexpr uint8_t kDevicePathEndSubType = 0xFF;

        // 启动服务表：只用到池分配、内存图、协议查找与退出启动服务，其余成员按规范顺序占位
        // 占位成员的真实签名各不相同，这里统一用 void* 只保证宽度与顺序
        struct BootServices {
            TableHeader header;              // 表头
            void* raiseTpl;                  // 0  RaiseTPL
            void* restoreTpl;                // 1  RestoreTPL
            // 分配物理页：内核镜像先放进按任意地址分配的中转缓冲；
            // 镜像数据声明的固定装载地址在退出启动服务之后才写，不能假设那块内存空闲
            EfiStatus (BALEEN_EFIAPI *allocatePages)(uint32_t allocType, uint32_t memoryType, uint64_t pages, uint64_t* memory);
            EfiStatus (BALEEN_EFIAPI *freePages)(uint64_t memory, uint64_t pages);   // 3  FreePages
            // 按物理地址取内存图：MapKey 是本次内存图的身份，退出启动服务时必须原样交回
            EfiStatus (BALEEN_EFIAPI *getMemoryMap)(uint64_t* mapBytes, MemoryDescriptor* map, uint64_t* mapKey, uint64_t* descriptorBytes, uint32_t* descriptorVersion);
            // 按字节分配池内存；退出启动服务之前必须释放（本阶段分配的内存图缓冲由本阶段自己管）
            EfiStatus (BALEEN_EFIAPI *allocatePool)(uint32_t poolType, uint64_t bytes, void** buffer);
            EfiStatus (BALEEN_EFIAPI *freePool)(void* buffer);   // 7 释放池内存
            void* createEvent;               // 8  CreateEvent
            void* setTimer;                  // 9  SetTimer
            void* waitForEvent;              // 10 WaitForEvent
            void* signalEvent;               // 11 SignalEvent
            void* closeEvent;                // 12 CloseEvent
            void* checkEvent;                // 13 CheckEvent
            void* installProtocolInterface;   // 14 InstallProtocolInterface
            void* reinstallProtocolInterface; // 15 ReinstallProtocolInterface
            void* uninstallProtocolInterface; // 16 UninstallProtocolInterface
            // 按句柄取协议接口
            EfiStatus (BALEEN_EFIAPI *handleProtocol)(EfiHandle handle, const Guid* protocol, void** interface);
            void* reserved;                  // 18 保留
            void* registerProtocolNotify;    // 19 RegisterProtocolNotify
            void* locateHandle;              // 20 LocateHandle
            // 按设备路径找设备句柄：取匹配最长前缀的句柄，并把路径指针推进到未匹配的剩余部分
            EfiStatus (BALEEN_EFIAPI *locateDevicePath)(const Guid* protocol, DevicePath** path, EfiHandle* device);
            void* installConfigurationTable; // 22 InstallConfigurationTable
            void* loadImage;                 // 23 LoadImage
            void* startImage;                // 24 StartImage
            void* exit;                      // 25 Exit
            void* unloadImage;               // 26 UnloadImage
            // 退出启动服务：MapKey 必须与当前内存图匹配，否则返回 EFI_INVALID_PARAMETER 并要重新取图
            EfiStatus (BALEEN_EFIAPI *exitBootServices)(EfiHandle imageHandle, uint64_t mapKey);
            void* getNextMonotonicCount;     // 27 GetNextMonotonicCount
            EfiStatus (BALEEN_EFIAPI *stall)(uint64_t microseconds);   // 28 Stall：微秒级忙等，用于控制器轮询之间的退避
            void* setWatchdogTimer;          // 29 SetWatchdogTimer
            void* connectController;         // 30 ConnectController
            void* disconnectController;      // 31 DisconnectController
            void* openProtocol;              // 32 OpenProtocol
            void* closeProtocol;             // 33 CloseProtocol
            void* openProtocolInformation;   // 34 OpenProtocolInformation
            void* protocolsPerHandle;        // 35 ProtocolsPerHandle
            void* locateHandleBuffer;        // 36 LocateHandleBuffer
            // 按协议标识取第一个支持它的接口：屏幕事实采集用它取 GOP（registration 传空）
            EfiStatus (BALEEN_EFIAPI *locateProtocol)(const Guid* protocol, void* registration, void** interface);
        };

        static_assert(offsetof(BootServices, allocatePages) == 40, "AllocatePages 的偏移已变");
        static_assert(offsetof(BootServices, freePages) == 48, "FreePages 的偏移已变");
        static_assert(offsetof(BootServices, getMemoryMap) == 56, "GetMemoryMap 的偏移已变");
        static_assert(offsetof(BootServices, allocatePool) == 64, "AllocatePool 的偏移已变");
        static_assert(offsetof(BootServices, freePool) == 72, "FreePool 的偏移已变");
        static_assert(offsetof(BootServices, handleProtocol) == 152, "HandleProtocol 的偏移已变");
        static_assert(offsetof(BootServices, locateDevicePath) == 184, "LocateDevicePath 的偏移已变");
        static_assert(offsetof(BootServices, exitBootServices) == 232, "ExitBootServices 的偏移已变");
        static_assert(offsetof(BootServices, locateProtocol) == 320, "LocateProtocol 的偏移已变");
        static_assert(offsetof(BootServices, stall) == 248, "Stall 的偏移已变");

        // 系统表：只用到控制台输出、启动服务与固件厂商标识，其后的成员不读
        struct SystemTable {
            TableHeader header;                  // 表头
            const char16_t* firmwareVendor;      // 固件厂商标识
            uint32_t firmwareRevision;           // 固件版本
            EfiHandle consoleInHandle;           // 控制台输入句柄
            void* consoleIn;                     // 控制台输入协议
            EfiHandle consoleOutHandle;          // 控制台输出句柄
            SimpleTextOutput* consoleOut;        // 控制台输出协议
            EfiHandle standardErrorHandle;       // 标准错误句柄
            SimpleTextOutput* standardError;     // 标准错误协议
            void* runtimeServices;               // 运行期服务
            BootServices* bootServices;          // 启动服务
        };

        static_assert(offsetof(SystemTable, consoleOut) == 64, "EFI_SYSTEM_TABLE 的 ConOut 偏移已变");
        static_assert(offsetof(SystemTable, bootServices) == 96, "EFI_SYSTEM_TABLE 的 BootServices 偏移已变");

        // 已加载映像协议：用到 DeviceHandle（据此找到加载本映像的设备）与映像自身的装载跨度
        // 装载跨度随 BootInfo 交给内核：引导器的残留不能被当成可用内存
        struct LoadedImage {
            uint32_t revision;            // 版本
            EfiHandle parentHandle;       // 父映像句柄
            SystemTable* systemTable;     // 系统表
            EfiHandle deviceHandle;       // 加载本映像的设备句柄
            void* filePath;               // 卷内文件路径（设备路径，相对本卷）
            void* reserved;               // 保留
            uint32_t loadOptionsBytes;    // 加载选项长度
            void* loadOptions;            // 加载选项
            void* imageBase;              // 本映像装载的基址
            uint64_t imageSize;           // 本映像占用的字节数
        };

        static_assert(offsetof(LoadedImage, deviceHandle) == 24, "LoadedImage 的 DeviceHandle 偏移已变");
        static_assert(offsetof(LoadedImage, imageBase) == 64, "LoadedImage 的 ImageBase 偏移已变");
        static_assert(offsetof(LoadedImage, imageSize) == 72, "LoadedImage 的 ImageSize 偏移已变");

        // 块设备介质事实
        struct BlockIoMedia {
            uint32_t mediaId;                 // 介质标识：更换介质后变化
            uint8_t removableMedia;           // 是否可移动
            uint8_t mediaPresent;             // 介质是否在位
            uint8_t logicalPartition;         // 是否只是逻辑分区
            uint8_t readOnly;                 // 是否只读
            uint8_t writeCaching;             // 是否启用写缓存
            uint32_t blockSize;               // 逻辑块大小，字节
            uint32_t ioAlign;                 // 缓冲对齐要求
            EfiLba lastBlock;                 // 最后一个逻辑块号
            EfiLba lowestAlignedLba;          // 最低对齐块号
            uint32_t logicalBlocksPerPhysicalBlock;      // 每个物理块里的逻辑块数
            uint32_t optimalTransferLengthGranularity;   // 建议传输粒度
        };

        static_assert(offsetof(BlockIoMedia, blockSize) == 12, "BlockIoMedia 的 BlockSize 偏移已变");
        static_assert(offsetof(BlockIoMedia, lastBlock) == 24, "BlockIoMedia 的 LastBlock 偏移已变");

        // 块设备协议的版本：FlushBlocks 是 REVISION2 才加进来的成员，更老的固件报 0x00010000
        // 那时结构体里没有这个函数指针，调用它就是越界读——必须先看版本再决定能不能刷
        constexpr uint64_t kBlockIoRevision2 = 0x00020001;

        // 块设备协议：读写与刷新都以整个设备（不是分区）为单位，块号从设备起点算
        struct BlockIo {
            uint64_t revision;            // 版本
            BlockIoMedia* media;          // 介质事实
            EfiStatus (BALEEN_EFIAPI *reset)(BlockIo*, uint8_t);                                        // 复位
            EfiStatus (BALEEN_EFIAPI *readBlocks)(BlockIo*, uint32_t, EfiLba, uint64_t, void*);         // 读块
            EfiStatus (BALEEN_EFIAPI *writeBlocks)(BlockIo*, uint32_t, EfiLba, uint64_t, const void*);  // 写块
            EfiStatus (BALEEN_EFIAPI *flushBlocks)(BlockIo*, uint32_t);                                 // 刷新写缓存
        };

        static_assert(offsetof(BlockIo, readBlocks) == 24, "BlockIo 的 ReadBlocks 偏移已变");
        static_assert(offsetof(BlockIo, flushBlocks) == 40, "BlockIo 的 FlushBlocks 偏移已变");

        // —— PCI I/O 协议（UEFI 2.10 §14.4）：固件如果没有给目标控制器提供块协议，
        //    本阶段经 PCI 总线驱动拿配置空间与 BAR，再按控制器类码走自建读路径 ——
        // 只声明用到的成员：配置空间读写与 BAR 资源查询，其余成员按规范顺序占位
        struct PciIoProtocol;

        // 配置空间读写：width 取 EFI_PCI_IO_PROTOCOL_WIDTH，offset 是 PCI 配置空间的字节偏移
        using PciIoConfigAccess = EfiStatus (BALEEN_EFIAPI *)(PciIoProtocol*, uint32_t width, uint32_t offset, uint64_t count, void* buffer);

        // BAR 资源查询：resources 由固件按 EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR 分配，
        // 调用方读完后用 BootServices->freePool 归还；supports 可为空
        using PciIoGetBarAttributes = EfiStatus (BALEEN_EFIAPI *)(PciIoProtocol*, uint32_t barIndex, uint64_t* supports, void** resources);

        // PCI I/O 协议：字段顺序与偏移按 UEFI 规范，未用成员以 void* 占位
        struct PciIoProtocol {
            void* pollMem;                      // 0x00 PollMem
            void* pollIo;                       // 0x08 PollIo
            void* memRead;                      // 0x10 Mem.Read
            void* memWrite;                     // 0x18 Mem.Write
            void* ioRead;                       // 0x20 Io.Read
            void* ioWrite;                      // 0x28 Io.Write
            PciIoConfigAccess pciRead;          // 0x30 Pci.Read
            PciIoConfigAccess pciWrite;         // 0x38 Pci.Write
            void* copyMem;                      // 0x40
            void* map;                          // 0x48
            void* unmap;                        // 0x50
            void* allocateBuffer;               // 0x58
            void* freeBuffer;                   // 0x60
            void* flush;                        // 0x68
            void* getLocation;                  // 0x70
            void* attributes;                   // 0x78
            PciIoGetBarAttributes getBarAttributes; // 0x80
            void* setBarAttributes;             // 0x88
            uint64_t romSize;                   // 0x90
            void* romImage;                     // 0x98
        };

        static_assert(offsetof(PciIoProtocol, pciRead) == 0x30, "PciIo 的 Pci.Read 偏移已变");
        static_assert(offsetof(PciIoProtocol, pciWrite) == 0x38, "PciIo 的 Pci.Write 偏移已变");
        static_assert(offsetof(PciIoProtocol, getBarAttributes) == 0x80, "PciIo 的 GetBarAttributes 偏移已变");
        static_assert(sizeof(PciIoProtocol) == 0xA0, "PciIo 协议大小不是 0xA0");

        // 配置空间访问宽度：只用到 8 / 16 / 32 位
        constexpr uint32_t kPciIoWidthUint8 = 0;
        constexpr uint32_t kPciIoWidthUint16 = 1;
        constexpr uint32_t kPciIoWidthUint32 = 2;

        // ACPI 地址空间描述符（QWORD 形式）：GetBarAttributes 返回的资源描述
        // 布局按 ACPI 6.x 大资源头（1 字节标记 + 2 字节长度）打包，字段之间没有隐式填充
        struct PciBarResource {
            uint8_t desc;                // 0x8A：QWORD 地址空间描述符
            uint16_t length;             // 描述符数据长度（不含 3 字节头），本结构为 43
            uint8_t resourceType;        // 0 = 内存空间，1 = I/O 空间
            uint8_t generalFlags;        // 通用标志
            uint8_t typeSpecificFlags;   // 类型相关标志
            uint64_t granularity;        // 地址粒度
            uint64_t minimum;            // 地址范围下界
            uint64_t maximum;            // 地址范围上界
            uint64_t translation;        // 地址转换偏移
            uint64_t rangeBytes;         // 地址范围长度
        } __attribute__((packed));

        static_assert(sizeof(PciBarResource) == 46, "ACPI 地址空间描述符不是 46 字节");
        static_assert(offsetof(PciBarResource, resourceType) == 3, "BAR 资源的类型偏移已变");
        static_assert(offsetof(PciBarResource, minimum) == 14, "BAR 资源的地址下界偏移已变");
        static_assert(offsetof(PciBarResource, translation) == 30, "BAR 资源的转换偏移偏移已变");
        static_assert(offsetof(PciBarResource, rangeBytes) == 38, "BAR 资源的长度偏移已变");

        // 设备路径的硬件节点类型与 PCI 子类型：节点长度 6，随后是功能号与设备号
        constexpr uint8_t kDevicePathHardwareType = 0x01;
        constexpr uint8_t kDevicePathPciSubType = 0x01;

        // PCI 设备路径节点：类型 1 / 子类型 1，Function 在前、Device 在后
        struct PciDevicePathNode {
            DevicePath header;    // 类型、子类型与长度
            uint8_t function;     // 功能号
            uint8_t device;       // 设备号
        };

        static_assert(sizeof(PciDevicePathNode) == 6, "PCI 设备路径节点不是 6 字节");
        static_assert(offsetof(PciDevicePathNode, function) == 4, "PCI 设备路径节点的功能号偏移已变");

        // PCI 配置空间寄存器与位：命令寄存器、内存空间与总线主控
        constexpr uint32_t kPciCommandRegister = 0x04;
        constexpr uint16_t kPciCommandMemorySpace = 1u << 1;
        constexpr uint16_t kPciCommandBusMaster = 1u << 2;
        // PCI 类码寄存器：偏移 0x08 的 32 位里，bit31..24 类、bit23..16 子类、bit15..8 编程接口
        constexpr uint32_t kPciClassRegister = 0x08;
        constexpr uint32_t kPciClassShift = 24;
        constexpr uint32_t kPciSubclassShift = 16;
        constexpr uint32_t kPciProgIfShift = 8;
        // 受支持的控制器类码：AHCI（01/06/01）与 NVMe（01/08/02）
        constexpr uint8_t kPciClassMassStorage = 0x01;
        constexpr uint8_t kPciSubclassAhci = 0x06;
        constexpr uint8_t kPciProgIfAhci = 0x01;
        constexpr uint8_t kPciSubclassNvme = 0x08;
        constexpr uint8_t kPciProgIfNvme = 0x02;

        // PCI I/O 协议的标识
        constexpr Guid kPciIoProtocolGuid = {
            {0x00, 0xB2, 0xF5, 0x4C, 0xB8, 0x68, 0xA5, 0x4C, 0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A}};

        // 已加载映像协议的标识
        constexpr Guid kLoadedImageProtocolGuid = {
            {0xA1, 0x31, 0x1B, 0x5B, 0x62, 0x95, 0xD2, 0x11, 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
        // 块设备协议的标识
        constexpr Guid kBlockIoProtocolGuid = {
            {0x21, 0x5B, 0x4E, 0x96, 0x59, 0x64, 0xD2, 0x11, 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
        // 设备路径协议的标识
        constexpr Guid kDevicePathProtocolGuid = {
            {0x91, 0x6E, 0x57, 0x09, 0x3F, 0x6D, 0xD2, 0x11, 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
        // 图形输出协议（GOP）的标识：值 9042a9de-23dc-4a38-96fb-7aded080516a，前三个字段按小端存放
        constexpr Guid kGraphicsOutputProtocolGuid = {
            {0xDE, 0xA9, 0x42, 0x90, 0xDC, 0x23, 0x38, 0x4A, 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
    }
}
