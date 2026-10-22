/* Pci.hpp
    PCI 配置空间机制 1 的设备枚举与 BAR 解码：配置访问经 PciAccess 注入

    目标侧由 PciPorts.cpp 实现 0xCF8/0xCFC 端口访问，宿主检查注入内存配置空间替身；
    本层不直接执行 IN/OUT 指令，枚举与解码逻辑因此可在宿主上逐项断言
    总线地址编码（DeviceFacts.busAddress 与 PciFunction.busAddress 共用）：
    低 16 位为 bus << 8 | device << 3 | function，即 PCI 设备在总线拓扑里的紧凑坐标
    类码编码（DeviceFacts.classCode）：class << 16 | subclass << 8 | programInterface，
    与 PCI 配置空间 0x08 寄存器的类码三字节一致
    BAR 标志位（DeviceBar.flags）由本文件定义：bit0 I/O 空间、bit1 64 位 BAR、
    bit2 可预取、bit3 未分配（固件没有分配基址）；长度按 PCI 规范用写全 1 回读的方式探测
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

namespace DevModel {
    // 总线类别：DeviceFacts.busKind 的取值，0 保留给匹配通配
    constexpr uint32_t kBusKindPci = 1;
    // BAR 标志位：DeviceBar.flags 的位定义
    constexpr uint32_t kBarFlagIo = 0x1;         // I/O 空间（置位）；否则是内存空间
    constexpr uint32_t kBarFlag64 = 0x2;         // 64 位 BAR，与相邻的下一项成对解码
    constexpr uint32_t kBarFlagPrefetch = 0x4;   // 可预取内存
    constexpr uint32_t kBarFlagUnassigned = 0x8; // 基址为 0，固件未分配
    // 中断线哨兵：配置空间 0x3C 读到 0 或 0xFF 表示没有可用中断线
    constexpr uint32_t kNoIrq = 0xFFFFFFFF;

    // PCI 配置空间访问：address 是 bus << 16 | device << 11 | function << 8，offset 是寄存器偏移
    // 两个回调都按 32 位寄存器读写，offset 必须是 4 的整数倍；write 只在 BAR 长度探测时使用
    struct PciAccess {
        uint32_t (*read)(void* context, uint32_t address, uint32_t offset);             // 读一个 32 位寄存器
        void (*write)(void* context, uint32_t address, uint32_t offset, uint32_t value); // 写一个 32 位寄存器
        void* context;                                                                  // 透传给两个回调的上下文
    };

    // 一个 PCI 功能的枚举事实：坐标、标识、类码、BAR 表与中断线
    struct PciFunction {
        uint32_t bus;                     // 总线号 0..255
        uint32_t device;                  // 设备号 0..31
        uint32_t function;                // 功能号 0..7
        uint32_t headerType;              // 头类型，取 0x0E 的低 7 位
        uint32_t vendorId;                // 厂商 ID
        uint32_t deviceId;                // 设备 ID
        uint32_t classCode;               // class << 16 | subclass << 8 | programInterface
        uint32_t busAddress;              // 总线地址：bus << 8 | device << 3 | function
        uint32_t barCount;                // BAR 表条目数
        DeviceBar bars[kDeviceMaxBars];   // BAR 表，按 BAR 索引升序
        uint32_t irq;                     // 中断线；kNoIrq 表示没有
    };

    // 枚举统计：写入的功能数与是否因容量不足截断
    struct PciEnumeration {
        uint32_t count;      // 写入 PciFunction 表的功能数
        uint32_t truncated;  // 容量不足导致截断时为 1
    };

    // 目标侧 0xCF8/0xCFC 机制 1 的配置访问实现：由 PciPorts.cpp 提供，宿主检查不链接它
    PciAccess KernelPciAccess();

    // 枚举全部总线（0..255）、设备（0..31）与功能（0..7），跳过 vendor 为 0xFFFF 的空槽
    // 单功能设备只读取功能 0，多功能设备按头类型 0x0E 的 bit7 展开八个功能
    // 输出按总线地址升序；超出 capacity 时截断并置 truncated
    PciEnumeration EnumeratePci(const PciAccess& access, PciFunction* functions, uint32_t capacity);

    // 解码第 index 个 BAR：64 位 BAR 成对合并，nextIndex 返回下一个要处理的 BAR 索引
    // 写全 1 探测长度后恢复原值；未实现或保留类型的 BAR 不写入，nextIndex 仍然前进
    void DecodeBar(const PciAccess& access, uint32_t address, uint32_t index, DeviceBar& bar, uint32_t& nextIndex);
}
