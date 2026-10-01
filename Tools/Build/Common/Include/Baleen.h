/* Baleen.h
    Baleen 引导结构的宿主侧编码：描述符区、El Torito 引导目录与 MBR 分区项
*/

#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <HostIo.h>

namespace hostbuild {
    // 描述符区：写进引导介质的约定位置，由段产物拼接而成，头部不含段表
    // 位置字段一律以 512 字节为单位写入（值 = 字节偏移 / 512），与目标设备的逻辑扇区单位无关；
    // 读取方按运行期探测到的单位右移换算成本地 LBA（512 不移，2048 右移 2，4096 右移 3），
    // 因此同一份字节在 512e、4Kn 与 2048 字节单位的光盘上都指向同一处物理位置
    // 头部之后是段序列，每段以统一的段头开头；读取方按段长遍历、按段标记认段，顺序不作要求
    struct DescriptorArea {
        static constexpr uint64_t kWriteOffset = 0x22000;    // 写入介质时的字节位置（512 字节基准 272）
        static constexpr uint32_t kSlotOffset = 440;         // 磁盘 IPL 里描述符区位置槽的偏移，与 Ipl/Const.inc 的 DESC_SLOT_OFF 一致
        static constexpr uint32_t kBaseShift = 9;            // 位置字段的基准单位：1 << 9 = 512 字节
        static constexpr uint32_t kMaxAreaBytes = 512;       // 描述符区必须落在一个 512 字节扇区之内，读取方只读一个扇区
        static constexpr uint32_t kHeadBytes = 16;           // 头部长度：标记 4 + 版本 2 + 头长 2 + 起止扇区 8
        static constexpr uint32_t kSegmentHeadBytes = 8;     // 段头长度：标记 4 + 版本 2 + 段长 2
        static constexpr uint32_t kAreaMagic = 0x43534442;   // 'BDSC'，头部标记
        static constexpr uint32_t kStubMagic = 0x52445342;   // 'BSDR'，Stub 段标记
        static constexpr uint32_t kCoreMagic = 0x52444342;   // 'BCDR'，Core 段标记
        static constexpr uint32_t kFieldDescLba = 8;         // 头部内：本区起始位置，512 字节基准
        static constexpr uint32_t kFieldDescEndLba = 12;     // 头部内：本区结束位置，512 字节基准，含
        static constexpr uint32_t kFieldSegmentLba = 8;      // 段内：载荷起始位置，512 字节基准
        static constexpr uint32_t kFieldSegmentEndLba = 12;  // 段内：载荷结束位置，512 字节基准，含
        static constexpr uint64_t kMaxStubBytes = 0x10000 - 0x7E00;  // Stub 装入空间[0x7E00,0x10000)，含整扇区读取
        static constexpr uint64_t kMaxCoreBytes = 0x400000;          // Core 文件字节数上限
    };

    // 按段产物拼装描述符区并回填起止位置，返回完整映像
    // descs 为段产物目录：Head.bin 是头部，其余 *.bin 按文件名排序即为段序列
    // 按段头的格式标记决定回填哪个载荷的起止位置；不认识的段原样保留，新增段不必改本函数
    // 位置字段按 512 字节基准写（见 DescriptorArea），载荷起点必须 4096 对齐
    std::vector<uint8_t> BuildDescriptorArea(const std::string& descs, const Placed* stub, const Placed* core);

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
