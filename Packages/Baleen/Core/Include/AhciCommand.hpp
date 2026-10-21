/* AhciCommand.hpp
    AHCI 命令头与 ATA 传输的纯编码：字段位置与单位换算的单点定义
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    namespace Core {
        namespace AhciCommand {
            // H2D 寄存器 FIS 的长度：20 字节，即 5 个 DWORD
            constexpr uint32_t kFisDwords = 5;

            // 命令头 DW0：CFL 在 bit4:0、PRDTL 在 bit31:16
            constexpr uint32_t HeaderDw0(uint32_t fisDwords, uint32_t prdtEntries) {
                return (fisDwords & 0x1Fu) | (prdtEntries << 16);
            }

            // 命令头 DW1：PRDBC 由 HBA 写回，软件下发前置 0
            constexpr uint32_t HeaderDw1() { return 0; }

            // 一次 ATA 读的传输规划：LBA 与扇区数都按逻辑扇区换算
            struct Transfer {
                uint64_t lba = 0;        // 起始逻辑扇区号
                uint32_t sectors = 0;    // 逻辑扇区数
            };

            // 把字节偏移与长度换算成 LBA 与逻辑扇区数
            // sectorBytes 必须是 2 的幂；偏移或长度不是它的整数倍、长度为零、
            // 扇区数超过 LBA48 的 16 位计数都返回 false
            constexpr bool PlanTransfer(uint64_t byteOffset, uint32_t bytes, uint32_t sectorBytes, Transfer& transfer) {
                if (sectorBytes == 0 || (sectorBytes & (sectorBytes - 1)) != 0) return false;
                if (bytes == 0 || (byteOffset & (sectorBytes - 1)) != 0 || (bytes & (sectorBytes - 1)) != 0) return false;
                const uint32_t shift = static_cast<uint32_t>(__builtin_ctz(sectorBytes));
                const uint32_t sectors = bytes >> shift;
                if (sectors > 0xFFFFu) return false;
                transfer.lba = byteOffset >> shift;
                transfer.sectors = sectors;
                return true;
            }
        }
    }
}
