/* Fat.cpp
    FAT 引导扇区类型判定与 FAT32 校验实现
*/

#include <Fat.h>
#include <HostIo.h>

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

    FatType DetectFatType(std::span<const uint8_t> bootSector, std::string& detail) {
        if (bootSector.size() < 512) {
            detail = "引导扇区不足 512 字节";
            return FatType::Unknown;
        }
        if (bootSector[510] != 0x55 || bootSector[511] != 0xAA) {
            detail = "偏移 510 不是 0x55AA";
            return FatType::Unknown;
        }

        const uint32_t bytesPerSector = GetLe16(bootSector, 11);
        const uint32_t sectorsPerCluster = bootSector[13];
        const uint32_t reservedSectors = GetLe16(bootSector, 14);
        const uint32_t fatCount = bootSector[16];
        const uint32_t rootEntries = GetLe16(bootSector, 17);
        const uint32_t total16 = GetLe16(bootSector, 19);
        const uint32_t fat16 = GetLe16(bootSector, 22);
        const uint32_t total32 = GetLe32(bootSector, 32);
        const uint32_t fat32 = GetLe32(bootSector, 36);

        // 各字段首先要像 FAT 引导扇区：扇区与簇大小为 2 的幂，FAT 与保留区非空
        if (bytesPerSector < 512 || bytesPerSector > 4096 || !IsPowerOfTwo(bytesPerSector)) {
            detail = "每扇区字节数非法（" + std::to_string(bytesPerSector) + "）";
            return FatType::Unknown;
        }
        if (!IsPowerOfTwo(sectorsPerCluster)) {
            detail = "每簇扇区数非法（" + std::to_string(sectorsPerCluster) + "）";
            return FatType::Unknown;
        }
        if (reservedSectors == 0 || fatCount == 0) {
            detail = "保留扇区数或 FAT 个数为 0";
            return FatType::Unknown;
        }
        const uint32_t totalSectors = total16 != 0 ? total16 : total32;
        const uint32_t fatSectors = fat16 != 0 ? fat16 : fat32;
        if (totalSectors == 0 || fatSectors == 0) {
            detail = "总扇区数或 FAT 大小为 0";
            return FatType::Unknown;
        }

        // 数据区簇数决定类型（Microsoft FAT 规范：<4085 FAT12，<65525 FAT16，其余 FAT32）
        const uint32_t rootDirSectors = (rootEntries * 32 + bytesPerSector - 1) / bytesPerSector;
        const uint32_t overhead = reservedSectors + fatCount * fatSectors + rootDirSectors;
        if (totalSectors <= overhead) {
            detail = "总扇区数不足以容纳元数据区";
            return FatType::Unknown;
        }
        const uint32_t clusters = (totalSectors - overhead) / sectorsPerCluster;

        if (clusters < 4085) {
            detail = "簇数 " + std::to_string(clusters);
            return FatType::Fat12;
        }
        if (clusters < 65525) {
            detail = "簇数 " + std::to_string(clusters);
            return FatType::Fat16;
        }
        if (rootEntries != 0 || fat16 != 0) {
            detail = "簇数 " + std::to_string(clusters) + " 落在 FAT32 范围，但根目录项数或 FATSz16 非 0，卷结构不一致";
            return FatType::Unknown;
        }
        detail = "簇数 " + std::to_string(clusters);
        return FatType::Fat32;
    }

    void RequireFat32Image(const std::string& path, const std::string& role) {
        std::string detail;
        const FatType type = DetectFatType(ReadHead(path, 512), detail);
        if (type == FatType::Fat32) return;
        throw std::runtime_error(role + "必须是 FAT32（" + TypeName(type) + "：" + detail + "）：" + path);
    }
}
