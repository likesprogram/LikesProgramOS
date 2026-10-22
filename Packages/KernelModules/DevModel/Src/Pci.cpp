/* Pci.cpp
    PCI 配置空间枚举与 BAR 解码实现：只经 PciAccess 读写，不直接碰端口

    枚举顺序固定：总线 0..255 → 设备 0..31 → 功能 0..7；单功能设备只读功能 0，
    多功能设备按功能 0 头类型 0x0E 的 bit7 展开；vendor 为 0xFFFF 的功能视为空槽
    BAR 长度用 PCI 规范的写全 1 回读法探测：先保存原值，再写 0xFFFFFFFF 读掩码，
    最后写回原值；探测只影响长度，不允许改变设备最终生效的基址
    解码结果里 bytes 为 0 表示该 BAR 未实现或类型保留，调用方不把它写进 BAR 表
*/

#include "Pci.hpp"

namespace {
    // 配置空间寄存器偏移
    constexpr uint32_t kRegisterVendor = 0x00;      // 厂商 ID（低 16 位）与设备 ID（高 16 位）
    constexpr uint32_t kRegisterClass = 0x08;       // 修订、编程接口、子类、类码
    constexpr uint32_t kRegisterHeaderType = 0x0C;  // 头类型在 bit16..23
    constexpr uint32_t kRegisterBarBase = 0x10;     // 前 6 个 BAR 从 0x10 起
    constexpr uint32_t kRegisterIrq = 0x3C;         // 中断线在低 8 位
    // 功能数上限与 BAR 数上限
    constexpr uint32_t kFunctionsPerDevice = 8;

    // 总线地址：bus << 16 | device << 11 | function << 8，即配置空间端口地址的高位
    uint32_t ConfigAddress(uint32_t bus, uint32_t device, uint32_t function) {
        return (bus << 16) | (device << 11) | (function << 8);
    }

    // 读一个 32 位配置寄存器：offset 必须按 4 字节对齐
    uint32_t ReadConfig32(const DevModel::PciAccess& access, uint32_t address, uint32_t offset) {
        return access.read(access.context, address, offset & 0xFCu);
    }

    // 写一个 32 位配置寄存器：offset 必须按 4 字节对齐
    void WriteConfig32(const DevModel::PciAccess& access, uint32_t address, uint32_t offset, uint32_t value) {
        access.write(access.context, address, offset & 0xFCu, value);
    }

    // 头类型对应的 BAR 数量：普通设备 6 个，PCI-PCI 桥 2 个，其余不解析
    uint32_t BarCountOf(uint32_t headerType) {
        switch (headerType & 0x7Fu) {
            case 0: return 6;
            case 1: return 2;
            default: return 0;
        }
    }

    // 探测一个 BAR 的回读掩码：写全 1 读回后恢复原值
    uint32_t ProbeMask(const DevModel::PciAccess& access, uint32_t address, uint32_t offset, uint32_t original) {
        WriteConfig32(access, address, offset, 0xFFFFFFFFu);
        const uint32_t mask = ReadConfig32(access, address, offset);
        WriteConfig32(access, address, offset, original);
        return mask;
    }
}

namespace DevModel {
    void DecodeBar(const PciAccess& access, uint32_t address, uint32_t index, DeviceBar& bar, uint32_t& nextIndex) {
        bar = DeviceBar{};
        bar.index = index;
        const uint32_t offset = kRegisterBarBase + 4 * index;
        const uint32_t original = ReadConfig32(access, address, offset);
        // I/O 空间 BAR：位 0 置位，基址按 4 字节对齐，长度为 4 的倍数
        if ((original & 0x1u) != 0) {
            const uint32_t mask = ProbeMask(access, address, offset, original);
            const uint32_t base = original & ~0x3u;
            const uint32_t bytes = (~(mask & ~0x3u)) + 1u;
            bar.flags = kBarFlagIo;
            if (base == 0) bar.flags |= kBarFlagUnassigned;
            bar.base = base;
            bar.bytes = bytes;
            nextIndex = index + 1;
            return;
        }
        // 内存空间 BAR 的类型字段：0 是 32 位，2 是 64 位，1 与 3 保留
        const uint32_t type = (original >> 1) & 0x3u;
        if (type == 0) {
            const uint32_t mask = ProbeMask(access, address, offset, original);
            const uint32_t base = original & ~0xFu;
            const uint32_t bytes = (~(mask & ~0xFu)) + 1u;
            if ((original & 0x8u) != 0) bar.flags |= kBarFlagPrefetch;
            if (base == 0) bar.flags |= kBarFlagUnassigned;
            bar.base = base;
            bar.bytes = bytes;
            nextIndex = index + 1;
            return;
        }
        if (type == 2) {
            const uint32_t highOffset = offset + 4;
            const uint32_t originalHigh = ReadConfig32(access, address, highOffset);
            // 成对探测：两半都写全 1，分别回读后恢复
            const uint32_t maskLow = ProbeMask(access, address, offset, original);
            const uint32_t maskHigh = ProbeMask(access, address, highOffset, originalHigh);
            const uint64_t mask = (static_cast<uint64_t>(maskLow) & ~0xFull) | (static_cast<uint64_t>(maskHigh) << 32);
            const uint64_t base = (static_cast<uint64_t>(original) & ~0xFull) | (static_cast<uint64_t>(originalHigh) << 32);
            const uint64_t bytes = (~mask) + 1ull;
            bar.flags = kBarFlag64;
            if ((original & 0x8u) != 0) bar.flags |= kBarFlagPrefetch;
            if (base == 0) bar.flags |= kBarFlagUnassigned;
            bar.base = base;
            bar.bytes = bytes;
            nextIndex = index + 2;
            return;
        }
        // 保留类型：不解析，标记未分配并从下一个 BAR 继续
        bar.flags = kBarFlagUnassigned;
        nextIndex = index + 1;
    }

    PciEnumeration EnumeratePci(const PciAccess& access, PciFunction* functions, uint32_t capacity) {
        PciEnumeration result{0, 0};
        if (access.read == nullptr || functions == nullptr || capacity == 0) return result;
        for (uint32_t bus = 0; bus <= 0xFF; ++bus) {
            for (uint32_t device = 0; device < 32; ++device) {
                const uint32_t function0Address = ConfigAddress(bus, device, 0);
                const uint32_t function0Id = ReadConfig32(access, function0Address, kRegisterVendor);
                if ((function0Id & 0xFFFFu) == 0xFFFFu) continue;
                const uint32_t function0Header = (ReadConfig32(access, function0Address, kRegisterHeaderType) >> 16) & 0xFFu;
                // 多功能位只在功能 0 上有效；未置位时只读功能 0
                const uint32_t functionCount = ((function0Header & 0x80u) != 0) ? kFunctionsPerDevice : 1;
                for (uint32_t function = 0; function < functionCount; ++function) {
                    const uint32_t address = ConfigAddress(bus, device, function);
                    const uint32_t identifier = ReadConfig32(access, address, kRegisterVendor);
                    if ((identifier & 0xFFFFu) == 0xFFFFu) continue;
                    if (result.count >= capacity) {
                        result.truncated = 1;
                        return result;
                    }
                    PciFunction& entry = functions[result.count];
                    entry = PciFunction{};
                    entry.bus = bus;
                    entry.device = device;
                    entry.function = function;
                    entry.vendorId = identifier & 0xFFFFu;
                    entry.deviceId = identifier >> 16;
                    const uint32_t classRegister = ReadConfig32(access, address, kRegisterClass);
                    entry.classCode = (classRegister >> 8) & 0xFFFFFFu;
                    entry.headerType = (ReadConfig32(access, address, kRegisterHeaderType) >> 16) & 0xFFu;
                    entry.busAddress = (bus << 8) | (device << 3) | function;
                    const uint32_t barCount = BarCountOf(entry.headerType);
                    uint32_t index = 0;
                    while (index < barCount && entry.barCount < kDeviceMaxBars) {
                        DeviceBar bar{};
                        uint32_t nextIndex = index + 1;
                        DecodeBar(access, address, index, bar, nextIndex);
                        if (bar.bytes != 0) entry.bars[entry.barCount++] = bar;
                        index = nextIndex;
                    }
                    const uint32_t irqRegister = ReadConfig32(access, address, kRegisterIrq);
                    const uint32_t line = irqRegister & 0xFFu;
                    entry.irq = (line == 0 || line == 0xFFu) ? kNoIrq : line;
                    ++result.count;
                }
            }
        }
        return result;
    }
}
