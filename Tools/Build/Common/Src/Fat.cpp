#include "Fat.h"
#include "HostIo.h"

#include <stdexcept>

namespace makeiso {
    namespace {
        // 按小端序读取整数
        uint16_t GetLe16(std::span<const uint8_t> bytes, std::size_t offset) {
            return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
        }

        // 按小端序读取整数
        uint32_t GetLe32(std::span<const uint8_t> bytes, std::size_t offset) {
            return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) | (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        }

        // 判断是否为 2 的幂
        bool IsPowerOfTwo(uint32_t value) {
            return value != 0 && (value & (value - 1)) == 0;
        }

        // 返回 FAT 类型的字符串表示，用于错误信息
        const char* TypeName(FatType type) {
            switch (type) {
                case FatType::Fat12: return "FAT12";
                case FatType::Fat16: return "FAT16";
                case FatType::Fat32: return "FAT32";
                case FatType::Unknown: break;
            }
            return "非 FAT 或结构非法";
        }
    }

    // 按 Microsoft FAT 规范的簇数规则判定引导扇区所属的 FAT 类型（不依赖卷标字符串）
    // detail 写出诊断信息：成功时给簇数，失败时给原因
    FatType DetectFatType(std::span<const uint8_t> boot_sector, std::string& detail) {
        if (boot_sector.size() < 512) {
            detail = "引导扇区不足 512 字节";
            return FatType::Unknown;
        }
        if (boot_sector[510] != 0x55 || boot_sector[511] != 0xAA) {
            detail = "偏移 510 不是 0x55AA";
            return FatType::Unknown;
        }

        const uint32_t bytes_per_sector = GetLe16(boot_sector, 11);
        const uint32_t sectors_per_cluster = boot_sector[13];
        const uint32_t reserved_sectors = GetLe16(boot_sector, 14);
        const uint32_t fat_count = boot_sector[16];
        const uint32_t root_entries = GetLe16(boot_sector, 17);
        const uint32_t total16 = GetLe16(boot_sector, 19);
        const uint32_t fat16 = GetLe16(boot_sector, 22);
        const uint32_t total32 = GetLe32(boot_sector, 32);
        const uint32_t fat32 = GetLe32(boot_sector, 36);

        // 各字段首先要像 FAT 引导扇区：扇区与簇大小为 2 的幂，FAT 与保留区非空。
        if (bytes_per_sector < 512 || bytes_per_sector > 4096 || !IsPowerOfTwo(bytes_per_sector)) {
            detail = "每扇区字节数非法（" + std::to_string(bytes_per_sector) + "）";
            return FatType::Unknown;
        }
        if (!IsPowerOfTwo(sectors_per_cluster)) {
            detail = "每簇扇区数非法（" + std::to_string(sectors_per_cluster) + "）";
            return FatType::Unknown;
        }
        if (reserved_sectors == 0 || fat_count == 0) {
            detail = "保留扇区数或 FAT 个数为 0";
            return FatType::Unknown;
        }
        const uint32_t total_sectors = total16 != 0 ? total16 : total32;
        const uint32_t fat_sectors = fat16 != 0 ? fat16 : fat32;
        if (total_sectors == 0 || fat_sectors == 0) {
            detail = "总扇区数或 FAT 大小为 0";
            return FatType::Unknown;
        }

        // 数据区簇数决定类型（Microsoft FAT 规范：<4085 FAT12，<65525 FAT16，其余 FAT32）。
        const uint32_t root_dir_sectors = (root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;
        const uint32_t overhead = reserved_sectors + fat_count * fat_sectors + root_dir_sectors;
        if (total_sectors <= overhead) {
            detail = "总扇区数不足以容纳元数据区";
            return FatType::Unknown;
        }
        const uint32_t clusters = (total_sectors - overhead) / sectors_per_cluster;

        if (clusters < 4085) {
            detail = "簇数 " + std::to_string(clusters);
            return FatType::Fat12;
        }
        if (clusters < 65525) {
            detail = "簇数 " + std::to_string(clusters);
            return FatType::Fat16;
        }
        if (root_entries != 0 || fat16 != 0) {
            detail = "簇数 " + std::to_string(clusters) + " 落在 FAT32 范围，但根目录项数或 FATSz16 非 0，卷结构不一致";
            return FatType::Unknown;
        }
        detail = "簇数 " + std::to_string(clusters);
        return FatType::Fat32;
    }

    // 校验镜像文件的引导扇区必须是 FAT32，否则抛异常。role 用于错误信息，如 "ESP 镜像"
    // ESP 写死为 FAT32：UEFI 固件读它，内核必备的 FAT32 实现也读它
    void RequireFat32Image(const std::string& path, const std::string& role) {
        std::string detail;
        const FatType type = DetectFatType(ReadHead(path, 512), detail);
        if (type == FatType::Fat32) return;
        throw std::runtime_error(role + "必须是 FAT32（" + TypeName(type) + "：" + detail + "）：" + path);
    }
}
