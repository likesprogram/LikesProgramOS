/* BootInfo.hpp
    引导交接契约 BootInfo 的结构定义：引导期各阶段之间传递的介质与内存事实

    内核入口先用到的是尺寸、版本、固件启动服务状态、启动介质、输出通道与内存图；
    启动来源（介质、系统卷、内核路径与引导器占用区间）是内核与运行期更新的定位依据：
    更新要写到**同一个卷**、发布到**同一条路径下的集根**、把状态记录写回**同一处状态区域**，
    因此这些事实由引导器在交接时给全，不让内核去猜设备

    屏幕事实与 FATAL 等待配置：引导器只报告“屏幕当前是什么形态”与等待毫秒数，
    内核按事实装配自绘目标（BIOS 文本模式直写 0xB8000 / UEFI 线性帧缓冲自绘），不猜显示模式；
    等待 0 表示默认 10 秒、超过上限也按默认——两者都只影响失败可见性，不改变启动判定

    内核集标识与尝试身份、启动来源各字段属接口未发布期间的设计调整：直接加进 Version=1 的结构，
    不抬版本，尺寸随字段增加；
    内核要求版本与尺寸都达到本头定义的值，短于本头的 BootInfo 直接拒绝——内核不为
    第三方引导协议提供适配层，也不把缺失字段解释成“未提供”
    结构只含定宽整数与物理地址，不含指针：32 位引导侧与 64 位内核侧对同一份字节的解释一致
    物理地址指向的内存不归内核所有，由引导器在交接期间保持有效；内核读到即用，不长期持有

    引导闭包交接包（0xE0..0x100）：清单副本、列表副本与
    预装模块记录数组三类缓冲都由引导器在交权前准备好，地址必须在低 4GiB 的恒等映射内；
    清单副本是验签通过时签名值 64 字节已归零的那份字节，内核不重验签名、不重算清单摘要；
    预装模块记录数组以 8 字节头（count u32 + 保留 u32，保留必须为 0）开始，随后是 count 条
    16 字节记录（phys u64 / bytes u32 / artifactIndex u16 / reserved u16），记录数等于引导闭包成员数；
    该区占用原 reserved1 区，既有字段偏移与结构长度 kBytes=0x100 都不变

    BootInfo 是 ABI：字段只追加、不移动，既有字段的偏移由 static_assert 钉住
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Boot {
    // 启动介质类型：取值与 IPL 交权时 DH 的编码一致
    enum class Media : uint32_t {
        Hdd = 2,        // 硬盘 / USB-HDD
        Cdrom = 4,      // 光盘
    };

    // 固件启动服务状态：交接时的事实，不是内核希望的状态，失效动作由内核入口完成
    // UefiActive 分支当前阶段登记为不适用：内核入口只拿到 BootInfo，没有 SystemTable / ImageHandle /
    // MapKey，无法调用 ExitBootServices（MapKey 过期重取更无从谈起），因此内核遇到该状态时
    // 保留 E-KRN-07 报 FATAL 并停机，不建立“退回固件”的分支。要落地该分支必须先扩本契约
    // （启动服务表指针、映像句柄与 MapKey 三个字段），属接口首次发布前的契约变更，不在当前阶段
    enum class Firmware : uint32_t {
        None = 0,        // 无固件服务可用：BIOS 路径进入长模式后本就不具备调用条件
        UefiExited = 1,  // UEFI 启动服务已在交接前退出，内核不重复调用
        UefiActive = 2,  // UEFI 启动服务仍有效；当前阶段不适用，内核按 E-KRN-07 拒绝并停机
    };

    // 可用输出通道位：引导器只报告事实，目标实例由内核侧按可用项装配
    enum class Channel : uint32_t {
        Serial = 1u << 0,     // 16550 兼容串口（COM1）
        DebugPort = 1u << 1,  // 0xE9 调试口，模拟器专有通道
    };

    // 屏幕事实类型：引导器只报告屏幕当前是什么形态，内核按它装配自绘目标
    // 屏幕是诊断增强，不是启动前提：事实不成立或内核不支持时按“屏幕不可用”处理，不停机
    enum class ScreenKind : uint32_t {
        None = 0,         // 没有屏幕事实：只走流式通道
        VgaText = 1,      // BIOS 80×25 文本模式：0xB8000 文本页，内核直写
        Framebuffer = 2,  // UEFI GOP 线性帧缓冲：按 address / stride / format 自绘
    };

    // 屏幕像素格式：只列出已支持的打包格式，其他值按“屏幕不可用”处理
    enum class ScreenFormat : uint32_t {
        VgaText = 0,    // 文本模式：每字符 2 字节（字符 + 属性），只有 VgaText 使用
        Bgrx8888 = 1,   // 32 位打包，小端字节序 B-G-R-X
        Rgbx8888 = 2,   // 32 位打包，小端字节序 R-G-B-X
    };

    // 启动设备线索的类型：决定 devicePath 指向的字节怎么解释
    // BIOS 的驱动器号只在固件环境里有意义，内核拿到它只为诊断与人工定位；
    // 通行的定位手段是卷标识：按 device 线索找到候选设备后，必须用卷标识核对身份
    enum class DeviceKind : uint32_t {
        None = 0,          // 未提供线索：只能用卷标识在设备里核对（UEFI 设备路径过长时会退到这一档）
        UefiDevicePath = 1,  // devicePath 指向 EFI_DEVICE_PATH_PROTOCOL 的字节，长度见 devicePathBytes
        BiosDrive = 2,     // devicePath 指向 1 字节的 BIOS 驱动器号（IPL 交权时的 DL）
    };

    // 内核路径上限：与布局描述的内核路径字段同宽（见 LayoutDescriptor::kKernelPathMaxBytes）
    constexpr uint32_t kKernelPathMaxBytes = 255;

    // 启动设备线索的缓冲上限：引导器按它截断，超出即按“未提供”交接（内核只读这么多个字节）
    constexpr uint32_t kDeviceClueMaxBytes = 512;

    // E820 内存图条目：固件给出的一段物理区间，字段与固件的 24 字节条目一一对应
    // 属性字段不是可有可无的填充：32 位目标上 uint64_t 只按 4 字节对齐，少了它结构体会缩到
    // 20 字节，与固件写入的 24 字节步长错位，数组从第二条起全部读错
    struct MemoryMapEntry {
        uint64_t base;          // 区间起址，含
        uint64_t length;        // 区间长度，字节
        uint32_t type;          // 区间类型，1 为可用
        uint32_t attributes;    // ACPI 3.0 起的扩展属性，bit0 为 0 表示该条目应被忽略
    };

    // BootInfo 的固定取值与字段偏移：尺寸与版本是内核入口最先核对的两项
    struct InfoLayout {
        static constexpr uint32_t kVersion = 1;    // 当前契约版本
        static constexpr uint32_t kBytes = 0x100;  // 当前结构长度
        // 字段偏移，相对结构起点
        static constexpr uint32_t kFieldBytes = 0x00;              // 结构长度
        static constexpr uint32_t kFieldVersion = 0x04;            // 契约版本
        static constexpr uint32_t kFieldFlags = 0x08;              // 保留标志
        static constexpr uint32_t kFieldFirmware = 0x0C;           // 固件启动服务状态
        static constexpr uint32_t kFieldMedia = 0x10;              // 启动介质
        static constexpr uint32_t kFieldSectorBytes = 0x14;        // 介质本地扇区大小
        static constexpr uint32_t kFieldChannels = 0x18;           // 可用输出通道位
        static constexpr uint32_t kFieldMemoryMapCount = 0x1C;     // 内存图条数
        static constexpr uint32_t kFieldMemoryMapTruncated = 0x20; // 内存图截断标志
        static constexpr uint32_t kFieldFatalWaitMs = 0x24;        // FATAL 后等待毫秒数（0 = 默认）
        static constexpr uint32_t kFieldMemoryMap = 0x28;          // 内存图数组的物理地址
        // 屏幕事实：引导器报告屏幕当前形态，内核按它装配自绘目标
        static constexpr uint32_t kFieldScreenAddress = 0x30;      // 屏幕 / 帧缓冲的物理地址
        static constexpr uint32_t kFieldScreenWidth = 0x38;        // 宽度：文本列数 / 像素
        static constexpr uint32_t kFieldScreenHeight = 0x3C;       // 高度：文本行数 / 像素
        static constexpr uint32_t kFieldScreenStride = 0x40;       // 每行字节数
        static constexpr uint32_t kFieldScreenBytes = 0x44;        // 可写字节数
        static constexpr uint32_t kFieldScreenKind = 0x48;         // 屏幕事实类型
        static constexpr uint32_t kFieldScreenFormat = 0x4C;       // 像素格式
        static constexpr uint32_t kFieldKernelSetBuildId = 0x50;   // 内核集标识（32 字节）
        static constexpr uint32_t kFieldAttemptId = 0x70;          // 试启动尝试身份（8 字节）
        // 启动来源：内核与运行期更新据此定位“自己是从哪儿起来的”
        static constexpr uint32_t kFieldLayoutOffset = 0x78;       // 布局描述在介质上的字节偏移
        static constexpr uint32_t kFieldVolumeOffset = 0x80;       // 系统卷在介质上的字节偏移
        static constexpr uint32_t kFieldVolumeBytes = 0x88;        // 系统卷长度
        static constexpr uint32_t kFieldStateOffset = 0x90;        // 引导状态区域在介质上的字节偏移
        static constexpr uint32_t kFieldMediumBytes = 0x98;        // 启动介质容量
        static constexpr uint32_t kFieldKernelPath = 0xA0;         // 内核路径字符串的物理地址
        static constexpr uint32_t kFieldDevicePath = 0xA8;         // 启动设备线索的物理地址
        static constexpr uint32_t kFieldLoaderBegin = 0xB0;        // 引导器占用区间起址
        static constexpr uint32_t kFieldLoaderBytes = 0xB8;        // 引导器占用区间长度
        static constexpr uint32_t kFieldStateBytes = 0xC0;         // 引导状态区域长度
        static constexpr uint32_t kFieldKernelPathBytes = 0xC4;    // 内核路径字节数
        static constexpr uint32_t kFieldDevicePathBytes = 0xC8;    // 设备线索字节数
        static constexpr uint32_t kFieldDeviceKind = 0xCC;         // 设备线索类型
        static constexpr uint32_t kFieldVolumeUuid = 0xD0;         // 系统卷标识（16 字节）
        // 引导闭包交接包：清单副本、列表副本与预装模块记录数组（三类缓冲都在低 4GiB）
        static constexpr uint32_t kFieldKernelSetManifest = 0xE0;        // 清单副本的物理地址
        static constexpr uint32_t kFieldBootList = 0xE8;                 // 列表副本的物理地址
        static constexpr uint32_t kFieldBootModules = 0xF0;              // 预装模块记录数组的物理地址
        static constexpr uint32_t kFieldKernelSetManifestBytes = 0xF8;   // 清单副本字节数
        static constexpr uint32_t kFieldBootListBytes = 0xFC;            // 列表副本字节数
        static constexpr uint32_t kKernelSetBuildIdBytes = 32;     // 内核集标识长度
        static constexpr uint32_t kVolumeUuidBytes = 16;           // 卷标识长度
        // FATAL 后等待的默认值与上限：0 表示按默认，超过上限也按默认，等待不是安全边界
        static constexpr uint32_t kFatalWaitMsDefault = 10000;     // 默认等待 10 秒
        static constexpr uint32_t kFatalWaitMsMax = 600000;        // 上限 10 分钟
    };

    // 交接事实：引导器在交权前填好，内核入口按物理地址读取
    struct Info {
        uint32_t bytes;                // 本结构长度，最早的字段，供版本协商
        uint32_t version;              // 契约版本，取值 InfoLayout::kVersion
        uint32_t flags;                // 保留，当前必须为 0
        uint32_t firmware;             // 固件启动服务状态，取值同 Boot::Firmware
        uint32_t media;                // 启动介质，取值同 Boot::Media
        uint32_t sectorBytes;          // 启动介质的本地扇区大小，字节
        uint32_t channels;             // 可用输出通道位，取值同 Boot::Channel
        uint32_t memoryMapCount;       // 内存图条数
        uint32_t memoryMapTruncated;   // 缓冲装不下后续条目时为 1
        uint32_t fatalWaitMs;          // FATAL 后等待毫秒数；0 = 默认 10 秒，超过上限也按默认
        uint64_t memoryMap;            // 内存图数组的物理地址，条目布局见 MemoryMapEntry
        // —— 屏幕事实：引导器报告屏幕当前形态，内核按它装配自绘目标 ——
        // 屏幕是诊断增强不是启动前提：事实不成立或内核不支持时按“屏幕不可用”处理，不停机
        uint64_t screenAddress;        // 屏幕 / 帧缓冲的物理地址（低 4GiB）；0 表示未提供
        uint32_t screenWidth;          // 文本模式为字符列数，帧缓冲为像素宽
        uint32_t screenHeight;         // 文本模式为字符行数，帧缓冲为像素高
        uint32_t screenStride;         // 每行字节数（文本模式为 160）
        uint32_t screenBytes;          // 可写字节数（文本模式为 4000）
        uint32_t screenKind;           // 屏幕事实类型，取值同 Boot::ScreenKind
        uint32_t screenFormat;         // 像素格式，取值同 Boot::ScreenFormat
        uint8_t kernelSetBuildId[32];  // 本次启动的内核集标识，全零表示未提供（占位件或第三方引导来源）
        uint64_t attemptId;            // 本次试启动的尝试身份，0 表示本次不是试启动（直接启动已知良好集或缺省集）

        // —— 启动来源：引导器读到的事实，内核与运行期更新按它定位引导介质、系统卷与内核路径 ——
        uint64_t layoutOffset;         // 布局描述在**启动介质**上的字节偏移；0 表示未提供
        uint64_t volumeOffset;         // 系统卷在启动介质上的字节偏移
        uint64_t volumeBytes;          // 系统卷长度，字节；0 表示未提供
        uint64_t stateOffset;          // 引导状态区域在启动介质上的字节偏移；0 表示没有状态区域
        uint64_t mediumBytes;          // 启动介质容量，字节；0 表示引导器不知道（BIOS 交权块没有这个事实）
        uint64_t kernelPath;           // 内核路径字符串的物理地址，ASCII；长度见 kernelPathBytes
        uint64_t devicePath;           // 启动设备线索的物理地址；解释方式见 deviceKind
        uint64_t loaderBegin;          // 引导器自身占用区间的物理起址；0 表示未提供
        uint64_t loaderBytes;          // 引导器自身占用区间长度，字节
        uint32_t stateBytes;           // 引导状态区域长度，字节
        uint32_t kernelPathBytes;      // 内核路径字节数，不含结尾空字符
        uint32_t devicePathBytes;      // 设备线索字节数
        uint32_t deviceKind;           // 设备线索类型，取值同 Boot::DeviceKind
        uint8_t volumeUuid[16];        // 系统卷标识（Ext4 s_uuid），全零表示未提供
        // —— 引导闭包交接包：Baleen 预装清单、列表与模块记录的交接事实 ——
        // 三类缓冲都必须在低 4GiB 的恒等映射内；0 表示未提供，非零时字节数与地址须成对有效
        uint64_t kernelSetManifest;    // 验签通过的清单副本物理地址；签名值已归零，内核不重验签名
        uint64_t bootList;             // Modules.list 副本的物理地址
        uint64_t bootModules;          // 预装模块记录数组的物理地址；8 字节头 + count 条 16 字节记录
        uint32_t kernelSetManifestBytes;  // 清单副本字节数
        uint32_t bootListBytes;        // Modules.list 副本字节数
    };

    // 条目布局是固件与各阶段之间的约定，长度与类型字段偏移在编译期固定
    static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry 必须与固件的 24 字节 E820 条目同长");
    static_assert(offsetof(MemoryMapEntry, type) == 16, "MemoryMapEntry 的类型字段必须落在 E820 条目的偏移 16");

    // BootInfo 是 ABI，长度与字段偏移在编译期固定；32 位与 64 位对同一份字节的解释必须一致
    static_assert(sizeof(Info) == InfoLayout::kBytes, "BootInfo 长度必须是 InfoLayout::kBytes");
    static_assert(offsetof(Info, version) == InfoLayout::kFieldVersion, "BootInfo 的版本字段偏移已变");
    static_assert(offsetof(Info, firmware) == InfoLayout::kFieldFirmware, "BootInfo 的固件状态字段偏移已变");
    static_assert(offsetof(Info, media) == InfoLayout::kFieldMedia, "BootInfo 的介质字段偏移已变");
    static_assert(offsetof(Info, sectorBytes) == InfoLayout::kFieldSectorBytes, "BootInfo 的扇区大小字段偏移已变");
    static_assert(offsetof(Info, channels) == InfoLayout::kFieldChannels, "BootInfo 的输出通道字段偏移已变");
    static_assert(offsetof(Info, memoryMapCount) == InfoLayout::kFieldMemoryMapCount, "BootInfo 的内存图条数字段偏移已变");
    static_assert(offsetof(Info, fatalWaitMs) == InfoLayout::kFieldFatalWaitMs, "BootInfo 的 FATAL 等待字段偏移已变");
    static_assert(offsetof(Info, memoryMap) == InfoLayout::kFieldMemoryMap, "BootInfo 的内存图指针字段偏移已变");
    static_assert(offsetof(Info, screenAddress) == InfoLayout::kFieldScreenAddress, "BootInfo 的屏幕地址字段偏移已变");
    static_assert(offsetof(Info, screenWidth) == InfoLayout::kFieldScreenWidth, "BootInfo 的屏幕宽度字段偏移已变");
    static_assert(offsetof(Info, screenHeight) == InfoLayout::kFieldScreenHeight, "BootInfo 的屏幕高度字段偏移已变");
    static_assert(offsetof(Info, screenStride) == InfoLayout::kFieldScreenStride, "BootInfo 的屏幕步长字段偏移已变");
    static_assert(offsetof(Info, screenBytes) == InfoLayout::kFieldScreenBytes, "BootInfo 的屏幕字节数字段偏移已变");
    static_assert(offsetof(Info, screenKind) == InfoLayout::kFieldScreenKind, "BootInfo 的屏幕类型字段偏移已变");
    static_assert(offsetof(Info, screenFormat) == InfoLayout::kFieldScreenFormat, "BootInfo 的屏幕像素格式字段偏移已变");
    static_assert(offsetof(Info, kernelSetBuildId) == InfoLayout::kFieldKernelSetBuildId, "BootInfo 的内核集标识字段偏移已变");
    static_assert(offsetof(Info, attemptId) == InfoLayout::kFieldAttemptId, "BootInfo 的尝试身份字段偏移已变");
    static_assert(offsetof(Info, layoutOffset) == InfoLayout::kFieldLayoutOffset, "BootInfo 的布局描述偏移字段已变");
    static_assert(offsetof(Info, volumeOffset) == InfoLayout::kFieldVolumeOffset, "BootInfo 的系统卷偏移字段已变");
    static_assert(offsetof(Info, volumeBytes) == InfoLayout::kFieldVolumeBytes, "BootInfo 的系统卷长度字段已变");
    static_assert(offsetof(Info, stateOffset) == InfoLayout::kFieldStateOffset, "BootInfo 的状态区域偏移字段已变");
    static_assert(offsetof(Info, mediumBytes) == InfoLayout::kFieldMediumBytes, "BootInfo 的介质容量字段已变");
    static_assert(offsetof(Info, kernelPath) == InfoLayout::kFieldKernelPath, "BootInfo 的内核路径字段已变");
    static_assert(offsetof(Info, devicePath) == InfoLayout::kFieldDevicePath, "BootInfo 的设备线索字段已变");
    static_assert(offsetof(Info, loaderBegin) == InfoLayout::kFieldLoaderBegin, "BootInfo 的引导器占用起址字段已变");
    static_assert(offsetof(Info, loaderBytes) == InfoLayout::kFieldLoaderBytes, "BootInfo 的引导器占用长度字段已变");
    static_assert(offsetof(Info, stateBytes) == InfoLayout::kFieldStateBytes, "BootInfo 的状态区域长度字段已变");
    static_assert(offsetof(Info, kernelPathBytes) == InfoLayout::kFieldKernelPathBytes, "BootInfo 的内核路径长度字段已变");
    static_assert(offsetof(Info, devicePathBytes) == InfoLayout::kFieldDevicePathBytes, "BootInfo 的设备线索长度字段已变");
    static_assert(offsetof(Info, deviceKind) == InfoLayout::kFieldDeviceKind, "BootInfo 的设备线索类型字段已变");
    static_assert(offsetof(Info, volumeUuid) == InfoLayout::kFieldVolumeUuid, "BootInfo 的卷标识字段已变");
    static_assert(offsetof(Info, kernelSetManifest) == InfoLayout::kFieldKernelSetManifest, "BootInfo 的清单副本地址字段偏移已变");
    static_assert(offsetof(Info, bootList) == InfoLayout::kFieldBootList, "BootInfo 的列表副本地址字段偏移已变");
    static_assert(offsetof(Info, bootModules) == InfoLayout::kFieldBootModules, "BootInfo 的预装记录数组地址字段偏移已变");
    static_assert(offsetof(Info, kernelSetManifestBytes) == InfoLayout::kFieldKernelSetManifestBytes, "BootInfo 的清单副本长度字段偏移已变");
    static_assert(offsetof(Info, bootListBytes) == InfoLayout::kFieldBootListBytes, "BootInfo 的列表副本长度字段偏移已变");
}
