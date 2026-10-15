/* BootInfo.hpp
    引导交接契约 BootInfo 的结构定义：引导期各阶段之间传递的介质与内存事实

    当前定义的是内核入口先用到的最小字段集：尺寸、版本、固件启动服务状态、启动介质、
    输出通道、内存图与内核集身份，内核入口按前两项决定是否继续；所有权与系统卷标识的
    扩展字段随相应阶段补齐

    内核集标识与尝试身份属接口未发布期间的设计调整：直接加进 Version=1 的结构，不抬版本，
    与 Docs/Specs/内核集清单与签名.md 第七节一致；内核只核对尺寸与版本、把两个字段原样转交
    结构只含定宽整数与物理地址，不含指针：32 位引导侧与 64 位内核侧对同一份字节的解释一致
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
    enum class Firmware : uint32_t {
        None = 0,        // 无固件服务可用：BIOS 路径进入长模式后本就不具备调用条件
        UefiExited = 1,  // UEFI 启动服务已在交接前退出，内核不重复调用
        UefiActive = 2,  // UEFI 启动服务仍有效，内核须先退出才能继续
    };

    // 可用输出通道位：引导器只报告事实，目标实例由内核侧按可用项装配
    enum class Channel : uint32_t {
        Serial = 1u << 0,     // 16550 兼容串口（COM1）
        DebugPort = 1u << 1,  // 0xE9 调试口，模拟器专有通道
    };

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
        static constexpr uint32_t kBytes = 0x78;   // 当前结构长度
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
        static constexpr uint32_t kFieldReserved0 = 0x24;          // 保留
        static constexpr uint32_t kFieldMemoryMap = 0x28;          // 内存图数组的物理地址
        static constexpr uint32_t kFieldKernelSetBuildId = 0x50;   // 内核集标识（32 字节）
        static constexpr uint32_t kFieldAttemptId = 0x70;          // 试启动尝试身份（8 字节）
        static constexpr uint32_t kKernelSetBuildIdBytes = 32;     // 内核集标识长度
        static constexpr uint32_t kReservedBytes = 0x20;           // reserved[4] 的长度
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
        uint32_t reserved0;            // 保留，必须为 0
        uint64_t memoryMap;            // 内存图数组的物理地址，条目布局见 MemoryMapEntry
        uint64_t reserved[4];          // 保留，必须为 0
        uint8_t kernelSetBuildId[32];  // 本次启动的内核集标识，全零表示未提供（占位件或第三方引导来源）
        uint64_t attemptId;            // 本次试启动的尝试身份，0 表示本次不是试启动（直接启动已知良好集或缺省集）
    };

    // 条目布局是固件与各阶段之间的约定，长度与类型字段偏移在编译期固定
    static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry 必须与固件的 24 字节 E820 条目同长");
    static_assert(offsetof(MemoryMapEntry, type) == 16, "MemoryMapEntry 的类型字段必须落在 E820 条目的偏移 16");

    // BootInfo 是 ABI，长度与字段偏移在编译期固定；32 位与 64 位对同一份字节的解释必须一致
    static_assert(sizeof(Info) == InfoLayout::kBytes, "BootInfo 长度必须是 0x78 字节，见 InfoLayout::kBytes");
    static_assert(offsetof(Info, version) == InfoLayout::kFieldVersion, "BootInfo 的版本字段偏移已变");
    static_assert(offsetof(Info, firmware) == InfoLayout::kFieldFirmware, "BootInfo 的固件状态字段偏移已变");
    static_assert(offsetof(Info, media) == InfoLayout::kFieldMedia, "BootInfo 的介质字段偏移已变");
    static_assert(offsetof(Info, sectorBytes) == InfoLayout::kFieldSectorBytes, "BootInfo 的扇区大小字段偏移已变");
    static_assert(offsetof(Info, channels) == InfoLayout::kFieldChannels, "BootInfo 的输出通道字段偏移已变");
    static_assert(offsetof(Info, memoryMapCount) == InfoLayout::kFieldMemoryMapCount, "BootInfo 的内存图条数字段偏移已变");
    static_assert(offsetof(Info, memoryMap) == InfoLayout::kFieldMemoryMap, "BootInfo 的内存图指针字段偏移已变");
    static_assert(offsetof(Info, kernelSetBuildId) == InfoLayout::kFieldKernelSetBuildId, "BootInfo 的内核集标识字段偏移已变");
    static_assert(offsetof(Info, attemptId) == InfoLayout::kFieldAttemptId, "BootInfo 的尝试身份字段偏移已变");
}
