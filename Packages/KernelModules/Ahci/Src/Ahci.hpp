/* Ahci.hpp
    AHCI 读路径：端口选择与初始化、命令列表 + PRDT、IDENTIFY 与 READ DMA EXT

    中断不用：命令完成用 PxCI 位轮询，次数有界；超时先尝试端口复位，再报失败
    寄存器按 32 位 MMIO 访问；命令列表、FIS 接收区与命令表由调用方按规范分配
    （连续物理页 + 4KiB 页内布局），本文件只写字段，不申请内存
    命令头与传输规划复用 Common/Storage/AhciCommand.hpp 的语义（Baleen 刷盘路径同一份）
*/

#pragma once
#include <stdint.h>

#include <Storage/Error.hpp>

namespace Ahci {
    // 端口数量上限：CAP.PI 是 32 位
    constexpr uint32_t kMaxPorts = 32;
    // 命令槽数量：CAP.NCS 最多 32
    constexpr uint32_t kMaxCommandSlots = 32;
    // ATA 逻辑扇区大小：IDENTIFY 路径固定按 512 字节字节计数
    constexpr uint32_t kSectorBytes = 512;

    // 控制器与端口的运行期事实：DMA 缓冲由调用方分配并在这里登记
    struct Controller {
        volatile uint32_t* abar = nullptr;   // ABAR 虚拟地址（MMIO）
        uint8_t* commandList = nullptr;      // 命令列表虚拟地址（1KiB 对齐）
        uint8_t* fis = nullptr;              // FIS 接收区虚拟地址（256 字节对齐）
        uint8_t* commandTable = nullptr;     // 命令表虚拟地址（128 字节对齐）
        uint8_t* data = nullptr;             // 数据弹跳页虚拟地址
        uint64_t commandListPhysical = 0;    // 命令列表物理地址
        uint64_t fisPhysical = 0;            // FIS 接收区物理地址
        uint64_t commandTablePhysical = 0;   // 命令表物理地址
        uint64_t dataPhysical = 0;           // 数据弹跳页物理地址
        uint32_t pageBytes = 0;              // 页大小，数据弹跳页按它分块
        uint32_t port = 0;                   // 选中的端口号
        uint32_t sectorBytes = kSectorBytes; // 逻辑扇区大小
        uint64_t sectors = 0;                // 设备逻辑扇区数
        char model[41] = {};                 // IDENTIFY 的型号字符串
    };

    // 选择第一个接有 ATA 设备的端口并完成端口初始化与 IDENTIFY；失败返回 false
    bool Initialize(Controller& controller);

    // 从设备读一段介质：偏移与长度按扇区对齐；分块经数据弹跳页搬运
    Storage::Error Read(Controller& controller, uint64_t offset, void* buffer, uint32_t bytes);

    // 停止端口（PxCMD.ST 清零并等 CR/FR 清零）；初始化失败与 Abort 共用
    void StopPort(Controller& controller);
}
