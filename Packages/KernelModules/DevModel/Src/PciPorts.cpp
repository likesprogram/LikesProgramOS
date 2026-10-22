/* PciPorts.cpp
    PCI 配置空间机制 1 的目标侧访问：0xCF8 写配置地址、0xCFC 读写配置数据

    只在这里出现 IN/OUT 指令：其余枚举与 BAR 解码逻辑在 Pci.cpp，宿主侧用内存配置空间替身
    即可逐项断言；本文件只在模块 ELF 里参与链接，宿主夹具不编译它
    地址端口格式固定为 bit31 使能、bit23..16 总线、bit15..11 设备、bit10..8 功能、
    bit7..2 寄存器偏移；配置访问只对 32 位对齐寄存器进行，与 PciAccess 的约定一致
*/

#include "Pci.hpp"

namespace {
    // 配置地址端口与配置数据端口：PCI 机制 1 的固定端口
    constexpr uint16_t kConfigAddressPort = 0xCF8;
    constexpr uint16_t kConfigDataPort = 0xCFC;
    // 地址端口的使能位
    constexpr uint32_t kConfigEnable = 0x80000000u;

    // 写 32 位 I/O 端口
    void OutDword(uint16_t port, uint32_t value) {
        asm volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }

    // 读 32 位 I/O 端口
    uint32_t InDword(uint16_t port) {
        uint32_t value = 0;
        asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }

    // 读一个 32 位配置寄存器：address 是总线坐标，offset 是寄存器偏移
    uint32_t PortRead(void* context, uint32_t address, uint32_t offset) {
        (void)context;
        OutDword(kConfigAddressPort, kConfigEnable | address | (offset & 0xFCu));
        return InDword(kConfigDataPort);
    }

    // 写一个 32 位配置寄存器：只用于 BAR 长度探测与恢复
    void PortWrite(void* context, uint32_t address, uint32_t offset, uint32_t value) {
        (void)context;
        OutDword(kConfigAddressPort, kConfigEnable | address | (offset & 0xFCu));
        OutDword(kConfigDataPort, value);
    }
}

namespace DevModel {
    PciAccess KernelPciAccess() {
        PciAccess access;
        access.read = &PortRead;
        access.write = &PortWrite;
        access.context = nullptr;
        return access;
    }
}
