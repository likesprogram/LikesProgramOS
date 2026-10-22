/* AtaPio.hpp
    传统 ATA / ATAPI 的 PIO 只读驱动：探测、IDENTIFY、扇区读取

    中断不用：状态轮询加有界次数上限，超时只报失败不静机（本阶段 I/O 是同步的）
    端口 I/O 用内联汇编直接访问；模块与内核同处 ring 0，端口 I/O 不需要额外契约接口
    两个通道各两个盘位：命令块 0x1F0/0x170、控制块 0x3F6/0x376；ATAPI 走 PACKET 命令
    与 READ(10)/READ(12)，ATA 走 READ SECTORS / READ SECTORS EXT（LBA48）
    逻辑扇区大小由设备报告：ATA 传统盘 512，ATAPI 光盘按 READ CAPACITY 的块长（通常 2048）
    本文件只做硬件访问与错误映射，不碰契约表与注册流程
*/

#pragma once
#include <stdint.h>

#include <Storage/Error.hpp>

namespace AtaPio {
    // 最多四个盘位：两个通道 × 主/从
    constexpr uint32_t kMaxDrives = 4;
    // 设备类型：由 IDENTIFY 结果区分
    constexpr uint32_t kKindNone = 0;
    constexpr uint32_t kKindAta = 1;
    constexpr uint32_t kKindAtapi = 2;

    // 一个盘位的身份与容量事实：读取只依赖这里，不再回查端口
    struct Drive {
        uint16_t commandBase = 0;   // 命令块端口基址
        uint16_t controlBase = 0;   // 控制块端口基址（替代状态寄存器）
        uint32_t kind = kKindNone;  // 设备类型，取值 kKindAta / kKindAtapi
        uint32_t driveIndex = 0;    // 盘位：0 主盘、1 从盘
        uint32_t sectorBytes = 0;   // 逻辑扇区大小，字节
        uint64_t sectors = 0;       // 逻辑扇区数
        uint32_t lba48 = 0;         // 非零表示支持 LBA48 寻址
        char model[41] = {};        // IDENTIFY 的型号字符串，结尾零
    };

    // 探测两条通道的四个盘位：发现的设备写进 drives，返回发现数（≤ kMaxDrives）
    // 识别失败或空盘位跳过；不改变 drives[kMaxDrives] 之外的内存
    uint32_t Probe(Drive* drives, uint32_t capacity);

    // 从设备读一段介质：偏移与长度按设备扇区对齐、长度非零；失败返回 Io，参数非法返回 InvalidArgument
    Storage::Error Read(const Drive& drive, uint64_t offset, void* buffer, uint32_t bytes);
}
