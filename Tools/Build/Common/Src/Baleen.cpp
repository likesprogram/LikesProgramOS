/* Baleen.cpp
    Baleen 引导结构的宿主侧编码实现：描述符、El Torito 引导目录与 MBR 分区项
*/

#include <Baleen.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace hostbuild {
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

    namespace {
        // 按小端序读取 16 位
        uint16_t Get16(const uint8_t* p) {
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        // 按小端序读取 32 位
        uint32_t Get32(const uint8_t* p) {
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (8 * i);
            return value;
        }

        // 载荷在 512 字节基准下的起止位置（含）；越界与对齐不合法即抛异常
        // 基准值与目标设备的逻辑扇区单位无关：读取方按运行期单位右移得到本地 LBA，
        // 所以起点必须按最细的 4096 对齐，换算到 512 / 2048 / 4096 都不会错位
        std::pair<uint32_t, uint32_t> SectorRange(const Placed& placed, uint64_t maxBytes, const char* label) {
            if (placed.bytes == 0 || placed.bytes > maxBytes) throw std::runtime_error(std::string(label) + " 的长度必须非 0 且不超过上限");
            if (placed.offset % 4096 != 0) throw std::runtime_error(std::string(label) + " 的起点必须按 4096 对齐");
            constexpr uint32_t kBase = 1u << DescriptorArea::kBaseShift;
            const uint32_t first = static_cast<uint32_t>(placed.offset / kBase);
            const uint32_t sectors = static_cast<uint32_t>((placed.bytes + kBase - 1) / kBase);
            if (static_cast<uint64_t>(first) + sectors > 0x100000000ull) throw std::runtime_error(std::string(label) + " 的位置范围必须不超过 0xFFFFFFFF");
            return {first, first + sectors - 1};
        }
    }

    std::vector<uint8_t> BuildDescriptorArea(const std::string& descs, const Placed* stub, const Placed* core) {
        std::vector<uint8_t> area = ReadFile(descs + "/Head.bin");
        if (area.size() != DescriptorArea::kHeadBytes) throw std::runtime_error("描述符区头部长度不是 " + std::to_string(DescriptorArea::kHeadBytes) + " 字节");
        if (Get32(area.data()) != DescriptorArea::kAreaMagic) throw std::runtime_error("描述符区头部标记不符：" + descs + "/Head.bin");

        // 段按文件名排序拼接：顺序只保证可复现，读取方按段长遍历、按段标记认段
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(descs)) {
            if (entry.path().extension() != ".bin") continue;
            const std::string name = entry.path().filename().string();
            if (name != "Head.bin") names.push_back(name);
        }
        std::sort(names.begin(), names.end());

        for (const std::string& name : names) {
            std::vector<uint8_t> segment = ReadFile(descs + "/" + name);
            if (segment.size() < DescriptorArea::kSegmentHeadBytes) throw std::runtime_error("描述符段短于段头：" + name);
            if (Get16(segment.data() + 6) != segment.size()) throw std::runtime_error("段长与文件大小不一致：" + name);
            // 按段标记回填载荷的起止位置；不认识的段原样保留，新增段不必改本函数
            switch (Get32(segment.data())) {
                case DescriptorArea::kStubMagic: {
                    if (stub == nullptr) throw std::runtime_error("有 Stub 段却没有 Stub 载荷：" + name);
                    const auto [first, last] = SectorRange(*stub, DescriptorArea::kMaxStubBytes, "Stub");
                    Put32Le(segment.data() + DescriptorArea::kFieldSegmentLba, first);
                    Put32Le(segment.data() + DescriptorArea::kFieldSegmentEndLba, last);
                    break;
                }
                case DescriptorArea::kCoreMagic: {
                    if (core == nullptr) throw std::runtime_error("有 Core 段却没有 Core 载荷：" + name);
                    const auto [first, last] = SectorRange(*core, DescriptorArea::kMaxCoreBytes, "Core");
                    Put32Le(segment.data() + DescriptorArea::kFieldSegmentLba, first);
                    Put32Le(segment.data() + DescriptorArea::kFieldSegmentEndLba, last);
                    break;
                }
                default: break;
            }
            area.insert(area.end(), segment.begin(), segment.end());
        }

        // 本区的起止位置（512 字节基准）：装载方按运行期单位换算成本地 LBA 后判定
        constexpr uint32_t kBase = 1u << DescriptorArea::kBaseShift;
        if (area.size() > DescriptorArea::kMaxAreaBytes) throw std::runtime_error("描述符区超过 " + std::to_string(DescriptorArea::kMaxAreaBytes) + " 字节：装载方只读一个扇区");
        const uint32_t first = static_cast<uint32_t>(DescriptorArea::kWriteOffset / kBase);
        const uint64_t endBytes = static_cast<uint64_t>(first) * kBase + area.size();
        if (stub != nullptr && endBytes > stub->offset) throw std::runtime_error("描述符区与 Stub 载荷重叠");
        if (core != nullptr && endBytes > core->offset) throw std::runtime_error("描述符区与 Core 载荷重叠");
        const uint32_t end = first + static_cast<uint32_t>((area.size() + kBase - 1) / kBase) - 1;
        Put32Le(area.data() + DescriptorArea::kFieldDescLba, first);
        Put32Le(area.data() + DescriptorArea::kFieldDescEndLba, end);
        return area;
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

    void WriteMbrEntry(std::span<uint8_t, 512> mbr, std::size_t index, const MbrPartition& partition, uint32_t sector_bytes) {
        if (index > 3) throw std::runtime_error("MBR 分区项下标只能是 0..3");
        if (sector_bytes != 512 && sector_bytes != 4096) throw std::runtime_error("MBR 分区 LBA 单位必须为 512 或 4096 字节");
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) throw std::runtime_error("MBR 缺少 0xAA55 引导签名");
        if (partition.bytes == 0) throw std::runtime_error("分区长度不能为 0");
        if (partition.offset % sector_bytes != 0) throw std::runtime_error("分区起始偏移必须按 MBR 的 LBA 单位对齐");
        const uint64_t sectors = (partition.bytes + sector_bytes - 1) / sector_bytes;
        if (partition.offset / sector_bytes > 0xFFFFFFFFull || sectors > 0xFFFFFFFFull) throw std::runtime_error("分区超出 MBR 的 32 位 LBA 范围");
        uint8_t* entry = mbr.data() + 446 + 16 * index;
        entry[0] = 0x00;  // 不设活动标志
        entry[1] = 0xFE;  // 起始 CHS：无效值，强制按 LBA 访问
        entry[2] = 0xFF;
        entry[3] = 0xFF;
        entry[4] = partition.type;
        entry[5] = 0xFE;  // 结束 CHS：同上
        entry[6] = 0xFF;
        entry[7] = 0xFF;
        Put32Le(entry + 8, static_cast<uint32_t>(partition.offset / sector_bytes));
        Put32Le(entry + 12, static_cast<uint32_t>(sectors));
    }

    std::array<uint8_t, 512> PatchMbrPartitions(std::span<const uint8_t, 512> mbr, std::span<const MbrPartition> partitions, uint32_t sector_bytes) {
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) throw std::runtime_error("MBR 缺少 0xAA55 引导签名");
        if (partitions.size() > 4) throw std::runtime_error("MBR 分区项最多 4 个");
        std::array<uint8_t, 512> out{};
        std::copy(mbr.begin(), mbr.end(), out.begin());
        for (std::size_t i = 0; i < partitions.size(); ++i) WriteMbrEntry(out, i, partitions[i], sector_bytes);
        return out;
    }
}
