#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace makeiso {
    // BootDescriptor：一级引导在物理 0x300 读的 32 字节说明，数值与
    // Packages/Baleen/Ipl/Include/Const.inc、Body.inc 保持一致
    struct BootDescriptor {
        static constexpr uint32_t kMagic = 0x52445342;               // 'BSDR'
        static constexpr uint16_t kVersion = 1;
        static constexpr uint16_t kHeaderBytes = 32;
        static constexpr uint64_t kImageOffset = 0x300;              // 镜像内的绝对字节偏移
        static constexpr uint64_t kMaxStubBytes = 0x10000 - 0x7E00;  // Stub 装入点[0x7E00,0x10000)

        uint64_t stub_offset = 0;  // 字节偏移，2048 对齐（两级扇区尺寸都要求对齐）
        uint64_t stub_bytes = 0;   // 非 0，且不超过 kMaxStubBytes
    };

    std::array<uint8_t, 32> EncodeBootDescriptor(const BootDescriptor& descriptor);

    // El Torito 引导目录
    struct BootCatalogEntry {
        uint32_t lba = 0;           // 引导镜像的绝对 LBA（2048 字节扇区）
        uint64_t bytes = 0;         // 引导镜像长度
        uint16_t load_segment = 0;  // BIOS 项用 0x07C0；EFI 项由固件自行装入，用 0
        // El Torito 的扇区计数只有 16 位。默认 0 = 由 bytes 算出，超过 65535 就报错
        // 镜像体积本身表达不了时（例如 FAT32 卷的下限），由调用方给出截断值并在上层提示
        uint16_t sector_count = 0;
    };

    // El Torito 引导目录扇区（2048 字节）规范：
    struct BootCatalogSpec {
        BootCatalogEntry bios;                   // 默认项：BIOS 无仿真引导
        std::optional<BootCatalogEntry> efi;     // 平台 0xEF 段：UEFI 引导（FAT 镜像）
        std::string signature = "LIKESPROGRAM";  // 校验项与段首部的标识串
    };

    // 生成 2048 字节的引导目录扇区（校验项、默认项、可选的 EFI 段）
    std::vector<uint8_t> EncodeBootCatalog(const BootCatalogSpec& spec);

    // 混合镜像的 MBR 分区项
    struct MbrPartition {
        uint8_t type = 0;
        uint64_t offset = 0;  // 字节偏移，须 512 对齐
        uint64_t bytes = 0;
    };

    // 把一个分区项写到指定槽位（0..3）：校验 0xAA55，起点/长度按 512 字节 LBA 写实
    // CHS 写无效值，强制读取方按 LBA 访问
    void WriteMbrEntry(std::span<uint8_t, 512> mbr, std::size_t index, const MbrPartition& partition);

    // 从 0 号槽起按顺序写入（ISO 混合镜像用：ESP、Ext4 依次落到 0、1 号槽）
    std::array<uint8_t, 512> PatchMbrPartitions(std::span<const uint8_t, 512> mbr, std::span<const MbrPartition> partitions);
}
