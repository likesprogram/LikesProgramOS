/* Baleen.cpp
    Baleen 引导结构的宿主侧编码实现：描述符、El Torito 引导目录与 MBR 分区项
*/

#include <Baleen.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace makeiso {
    namespace {
        // 按小端序写入 16 位整数
        void Put16Le(uint8_t* p, uint16_t v) {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>(v >> 8);
        }

        // 按小端序写入 32 位整数
        void Put32Le(uint8_t* p, uint32_t v) {
            for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }

        // 按小端序写入 64 位整数
        void Put64Le(uint8_t* p, uint64_t v) {
            for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }

        // 按 512 字节扇区计数计算长度，超过 65535 扇区报错
        uint16_t SectorCount512(uint64_t bytes) {
            const uint64_t count = (bytes + 511) / 512;
            if (bytes == 0 || count == 0 || count > 0xFFFF) throw std::runtime_error("引导镜像超过 El Torito 的 16 位扇区计数上限（65535 × 512 = 33553920 字节）");
            return static_cast<uint16_t>(count);
        }

        // 写入固定长度的字符串，末尾不足补 0
        void PutIdString(uint8_t* p, std::size_t size, const std::string& text) {
            std::memset(p, 0, size);
            std::memcpy(p, text.data(), std::min(size, text.size()));
        }
    }

    std::array<uint8_t, 32> EncodeBootDescriptor(const BootDescriptor& descriptor, uint32_t sector_bytes) {
        if (sector_bytes != 512 && sector_bytes != 2048) throw std::runtime_error("引导介质扇区大小必须为 512 或 2048 字节");
        // 先限制原始长度，再上取整，避免长度加法溢出；CD 最多读 0x8000，HDD 可读 0x8200
        if (descriptor.stub_bytes == 0 || descriptor.stub_bytes > BootDescriptor::kMaxStubBytes) throw std::runtime_error("Stub 长度必须非 0 且不超过 0x10000-0x7E00");
        const uint64_t sectors = (descriptor.stub_bytes + sector_bytes - 1) / sector_bytes;
        if (sectors > BootDescriptor::kMaxStubBytes / sector_bytes) throw std::runtime_error("Stub 按介质扇区上取整后超过 0x10000-0x7E00");
        if (descriptor.stub_offset < 0x800) throw std::runtime_error("Stub 偏移必须至少为 0x800，避开描述符所在扇区");
        if (descriptor.stub_offset % 2048 != 0) throw std::runtime_error("Stub 偏移必须 2048 对齐（工具对两种介质的统一策略）");
        // 对应 IPL 的 32 位 ADD/JC：结束偏移恰好为 0x100000000 也必须拒绝
        if (descriptor.stub_offset > 0xFFFFFFFFull || descriptor.stub_bytes > 0xFFFFFFFFull - descriptor.stub_offset) throw std::runtime_error("Stub 偏移及偏移加长度必须不超过 0xFFFFFFFF");
        std::array<uint8_t, 32> out{};
        Put32Le(out.data() + 0, BootDescriptor::kMagic);
        Put16Le(out.data() + 4, BootDescriptor::kVersion);
        Put16Le(out.data() + 6, BootDescriptor::kHeaderBytes);
        Put64Le(out.data() + 8, descriptor.stub_offset);
        Put64Le(out.data() + 16, descriptor.stub_bytes);
        // 24..31 保留，必须为 0
        return out;
    }

    std::array<uint8_t, 32> EncodeCoreDescriptor(const CoreDescriptor& descriptor, uint32_t sector_bytes) {
        if (sector_bytes != 512 && sector_bytes != 2048) throw std::runtime_error("引导介质扇区大小必须为 512 或 2048 字节");
        if (descriptor.core_bytes == 0 || descriptor.core_bytes > CoreDescriptor::kMaxCoreBytes) throw std::runtime_error("Core 长度必须非 0 且不超过 0x400000");
        if (descriptor.core_offset < 0x800) throw std::runtime_error("Core 偏移必须至少为 0x800，避开描述符所在扇区");
        if (descriptor.core_offset % 2048 != 0) throw std::runtime_error("Core 偏移必须 2048 对齐（工具对两种介质的统一策略）");
        // 对应 Stub 读盘路径的 32 位寻址：偏移加长度不得进位
        if (descriptor.core_offset > 0xFFFFFFFFull || descriptor.core_bytes > 0xFFFFFFFFull - descriptor.core_offset) throw std::runtime_error("Core 偏移及偏移加长度必须不超过 0xFFFFFFFF");
        std::array<uint8_t, 32> out{};
        Put32Le(out.data() + 0, CoreDescriptor::kMagic);
        Put16Le(out.data() + 4, CoreDescriptor::kVersion);
        Put16Le(out.data() + 6, CoreDescriptor::kHeaderBytes);
        Put64Le(out.data() + 8, descriptor.core_offset);
        Put64Le(out.data() + 16, descriptor.core_bytes);
        // 24..31 保留，必须为 0
        return out;
    }

    std::vector<uint8_t> EncodeBootCatalog(const BootCatalogSpec& spec) {
        std::vector<uint8_t> catalog(2048, 0);

        // 校验项（32 字节）：平台 0x00 = 80x86 BIOS
        catalog[0] = 1;
        catalog[1] = 0;
        PutIdString(catalog.data() + 4, 24, spec.signature);
        catalog[30] = 0x55;
        catalog[31] = 0xAA;

        // 校验和：连同 0x55AA 结束标记在内，16 个字之和必须为 0
        uint16_t sum = 0;
        for (int i = 0; i < 16; ++i) sum = static_cast<uint16_t>(sum + (catalog[2 * i] | (catalog[2 * i + 1] << 8)));
        Put16Le(catalog.data() + 28, static_cast<uint16_t>(0u - sum));

        // 把一个引导项写到目录项位置
        const auto writeEntry = [](uint8_t* entry, const BootCatalogEntry& boot) {
            entry[0] = 0x88;  // 可引导
            entry[1] = 0;     // 无仿真
            Put16Le(entry + 2, boot.load_segment);
            entry[4] = 0;  // 系统类型
            entry[5] = 0;
            Put16Le(entry + 6, boot.sector_count != 0 ? boot.sector_count : SectorCount512(boot.bytes));
            Put32Le(entry + 8, boot.lba);
        };

        writeEntry(catalog.data() + 32, spec.bios);

        if (spec.efi.has_value()) {
            // 段首部：平台 0xEF（EFI），一个段项，0x91 表示最后一段
            catalog[64] = 0x91;
            catalog[65] = 0xEF;
            Put16Le(catalog.data() + 66, 1);
            PutIdString(catalog.data() + 68, 24, spec.signature);
            writeEntry(catalog.data() + 96, *spec.efi);
        }
        return catalog;
    }

    void WriteMbrEntry(std::span<uint8_t, 512> mbr, std::size_t index, const MbrPartition& partition) {
        if (index > 3) throw std::runtime_error("MBR 分区项下标只能是 0..3");
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) throw std::runtime_error("MBR 缺少 0xAA55 引导签名");
        if (partition.bytes == 0) throw std::runtime_error("分区长度不能为 0");
        if (partition.offset % 512 != 0) throw std::runtime_error("分区起始偏移必须 512 对齐");
        const uint64_t sectors = (partition.bytes + 511) / 512;
        if (partition.offset / 512 > 0xFFFFFFFFull || sectors > 0xFFFFFFFFull) throw std::runtime_error("分区超出 MBR 的 32 位 LBA 范围");
        uint8_t* entry = mbr.data() + 446 + 16 * index;
        entry[0] = 0x00;  // 不设活动标志
        entry[1] = 0xFE;  // 起始 CHS：无效值，强制按 LBA 访问
        entry[2] = 0xFF;
        entry[3] = 0xFF;
        entry[4] = partition.type;
        entry[5] = 0xFE;  // 结束 CHS：同上
        entry[6] = 0xFF;
        entry[7] = 0xFF;
        Put32Le(entry + 8, static_cast<uint32_t>(partition.offset / 512));
        Put32Le(entry + 12, static_cast<uint32_t>(sectors));
    }

    std::array<uint8_t, 512> PatchMbrPartitions(std::span<const uint8_t, 512> mbr, std::span<const MbrPartition> partitions) {
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) throw std::runtime_error("MBR 缺少 0xAA55 引导签名");
        if (partitions.size() > 4) throw std::runtime_error("MBR 分区项最多 4 个");
        std::array<uint8_t, 512> out{};
        std::copy(mbr.begin(), mbr.end(), out.begin());
        for (std::size_t i = 0; i < partitions.size(); ++i) WriteMbrEntry(out, i, partitions[i]);
        return out;
    }
}
