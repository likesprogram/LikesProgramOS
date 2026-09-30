/* Baleen.h
    Baleen 引导结构的宿主侧编码：BootDescriptor、CoreDescriptor、El Torito 引导目录与 MBR 分区项
*/

#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace makeiso {
    // BootDescriptor：一级引导在介质绝对字节偏移 0x22000 读的 32 字节说明，数值与
    // Packages/Baleen/Ipl/Include/Const.inc、Body.inc 保持一致
    // 该偏移是 512 / 2048 / 4096 三种逻辑扇区的公倍数并落在 GPT 保留区之后，
    // 同一份镜像在三种单位下的描述符字节位置一致
    struct BootDescriptor {
        static constexpr uint32_t kMagic = 0x52445342;               // 'BSDR'
        static constexpr uint16_t kVersion = 1;                      // 当前开发格式标记
        static constexpr uint16_t kHeaderBytes = 32;                 // 描述符固定总长
        static constexpr uint64_t kImageOffset = 0x22000;            // 镜像内的绝对字节偏移
        static constexpr uint64_t kMaxStubBytes = 0x10000 - 0x7E00;  // Stub 装入空间[0x7E00,0x10000)，含整扇区读取

        uint64_t stub_offset = 0;  // 低 32 位字节偏移，至少 0x1000 且 4096 对齐（工具统一策略）
        uint64_t stub_bytes = 0;   // 非 0，按介质扇区上取整后不超过 kMaxStubBytes；offset + bytes <= 0xFFFFFFFF
    };

    // 把 BootDescriptor 编码为 32 字节的二进制表示
    // sector_bytes 为本地介质扇区大小：HDD 512/4096、CD 2048；仅用于校验，不写入描述符
    std::array<uint8_t, 32> EncodeBootDescriptor(const BootDescriptor& descriptor, uint32_t sector_bytes);

    // CoreDescriptor：Stub 在介质绝对字节偏移 0x22020 读的 32 字节说明，与 BootDescriptor 落在同一扇区；
    // 数值与 Packages/Baleen/Common/Include/Contract.inc、Stub/Src/LoadCore.cpp 保持一致
    struct CoreDescriptor {
        static constexpr uint32_t kMagic = 0x52444342;        // 'BCDR'
        static constexpr uint16_t kVersion = 1;               // 当前开发格式标记
        static constexpr uint16_t kHeaderBytes = 32;          // 描述符固定总长
        static constexpr uint64_t kImageOffset = 0x22020;     // 镜像内的绝对字节偏移，紧随 BootDescriptor
        static constexpr uint64_t kMaxCoreBytes = 0x400000;   // Core 文件字节数上限

        uint64_t core_offset = 0;  // 低 32 位字节偏移，至少 0x1000 且 4096 对齐（工具统一策略）
        uint64_t core_bytes = 0;   // 非 0，且偏移加长度不超过 0xFFFFFFFF
    };

    // 把 CoreDescriptor 编码为 32 字节的二进制表示
    // sector_bytes 为本地介质扇区大小：HDD 512/4096、CD 2048；仅用于校验，不写入描述符
    std::array<uint8_t, 32> EncodeCoreDescriptor(const CoreDescriptor& descriptor, uint32_t sector_bytes);

    // El Torito 引导目录的一项；sector_count 只有 16 位，表达不了的由调用方给截断值
    struct BootCatalogEntry {
        uint32_t lba = 0;           // 引导镜像的绝对 LBA（2048 字节扇区）
        uint64_t bytes = 0;         // 引导镜像长度
        uint16_t load_segment = 0;  // BIOS 项用 0x07C0；EFI 项由固件自行装入，用 0
        uint16_t sector_count = 0;  // 0 = 按 bytes 算出；FAT32 这类超过上限的由调用方给值
    };

    // El Torito 引导目录扇区（2048 字节）
    struct BootCatalogSpec {
        BootCatalogEntry bios;                   // 默认项：BIOS 无仿真引导
        std::optional<BootCatalogEntry> efi;     // 平台 0xEF 段：UEFI 引导（FAT 镜像）
        std::string signature = "LIKESPROGRAM";  // 校验项与段首部的标识串
    };

    // 生成 2048 字节的引导目录扇区（校验项、默认项、可选的 EFI 段）
    std::vector<uint8_t> EncodeBootCatalog(const BootCatalogSpec& spec);

    // 混合镜像的 MBR 分区项
    struct MbrPartition {
        uint8_t type = 0;     // 分区类型（0xEF = ESP，0x83 = Ext4）
        uint64_t offset = 0;  // 字节偏移，须按 MBR 的 sector_bytes 对齐
        uint64_t bytes = 0;   // 分区长度
    };

    // 把一个分区项写到指定槽位（0..3）：校验 0xAA55，起点/长度按 sector_bytes LBA 写实
    // CHS 写无效值，强制读取方按 LBA 访问
    void WriteMbrEntry(std::span<uint8_t, 512> mbr, std::size_t index, const MbrPartition& partition, uint32_t sector_bytes = 512);

    // 从 0 号槽起按顺序写入（ISO 混合镜像用：ESP、Ext4 依次落到 0、1 号槽）
    std::array<uint8_t, 512> PatchMbrPartitions(std::span<const uint8_t, 512> mbr, std::span<const MbrPartition> partitions, uint32_t sector_bytes = 512);
}
