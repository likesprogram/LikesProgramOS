/* main.cpp
    MakeIso：把一级引导、实模式服务层、核心阶段、EFI 引导镜像与系统卷组装成可引导的光盘镜像，并同时满足写入 U 盘后的磁盘引导记录要求（混合镜像）
*/

#include <Baleen.h>
#include <Fat.h>
#include <HostIo.h>
#include <Image.h>
#include <Iso9660.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    using makeiso::AlignUp;
    using makeiso::BaseName;
    using makeiso::BootCatalogEntry;
    using makeiso::BootCatalogSpec;
    using makeiso::BootDescriptor;
    using makeiso::CoreDescriptor;
    using makeiso::FileSize;
    using makeiso::RequireFat32Image;
    using makeiso::Image;
    using makeiso::Iso9660;
    using makeiso::IsoExtent;
    using makeiso::IsoFile;
    using makeiso::IsoLayout;
    using makeiso::IsoVolume;
    using makeiso::JsonEscape;
    using makeiso::JsonPlaced;
    using makeiso::MbrPartition;
    using makeiso::ParseNumber;
    using makeiso::Placed;
    using makeiso::ReadFile;
    using makeiso::Reservations;

    constexpr uint64_t kBlock = 2048;                    // ISO9660 逻辑块大小
    constexpr uint64_t kSystemAreaBytes = 16 * kBlock;   // 系统区 16 个逻辑块
    // 与 Ipl/Src/Mbr.asm 分区项 0 的占位 LBA 2048（512 字节单位）一致：ESP 优先落在 1 MiB
    constexpr uint64_t kEspPreferredOffset = 1u << 20;

    // 命令行选项
    struct Options {
        std::string out;                                                   // 输出镜像路径
        std::string volume_id = "LIKESPROGRAM";                            // ISO 卷标识
        std::string boot_image;                                            // El Torito 默认引导镜像
        uint16_t load_segment = 0x07C0;                                    // 引导镜像装入段
        std::string mbr;                                                   // 混合镜像的 MBR
        std::string stub;                                                  // 实模式服务层
        std::string core;                                                  // 核心阶段
        std::string efi;                                                   // El Torito 平台 0xEF 的引导镜像（小 FAT 镜像）
        std::string esp;                                                   // 混合镜像的 ESP 分区内容（必须 FAT32）
        std::string system_volume;                                         // 系统卷镜像（Ext4）
        std::string iso_name;                                              // 系统卷在 ISO 内的文件名
        std::vector<std::pair<std::string, std::string>> files;            // ISO 路径 -> 宿主路径
        std::vector<std::pair<std::string, std::optional<uint64_t>>> raw;  // 宿主路径 -> 偏移
        std::string manifest;                                              // 构建清单输出路径
        std::optional<uint64_t> timestamp;                                 // 卷时间戳；空 = 写全零
        bool help = false;                                                 // 是否只要帮助
    };

    // 取卷时间戳：命令行优先，其次 SOURCE_DATE_EPOCH，都没有就写全零
    uint64_t ResolveTimestamp(const Options& options) {
        if (options.timestamp.has_value()) {
            return *options.timestamp;
        }
        if (const char* env = std::getenv("SOURCE_DATE_EPOCH"); env != nullptr && *env != '\0') {
            try {
                return std::stoull(env);
            } catch (const std::exception&) {
                throw std::runtime_error("SOURCE_DATE_EPOCH 不是合法秒数");
            }
        }
        return 0;  // 不指定：日期写全零，保证同一输入产出同样的字节
    }

    // 打印用法
    void Usage() {
        std::cout <<
            "MakeIso — Baleen 引导介质（ISO9660 + El Torito + 混合 MBR）组装器\n"
            "\n"
            "用法：MakeIso --out <镜像> --boot-image <BaleenIPLCd.bin> [选项]\n"
            "\n"
            "引导与载荷：\n"
            "  --boot-image PATH        El Torito 默认（BIOS）引导镜像，如 BaleenIPLCd.bin\n"
            "  --load-segment N         引导镜像装入段，默认 0x7C0（与一级引导的寻址一致）\n"
            "  --mbr PATH               混合镜像用 MBR（BaleenIPL.bin，512 字节），写到镜像偏移 0\n"
            "  --stub PATH              实模式服务层，原始扇区放置并写入 BootDescriptor\n"
            "  --core PATH              核心阶段，原始扇区放置并写 CoreDescriptor（位置进清单）\n"
            "  --efi PATH               El Torito 的 EFI 引导镜像（FAT32，供光盘 UEFI 引导）\n"
            "  --esp PATH               混合镜像的 ESP 分区内容（必须 FAT32）\n"
            "  --system-volume PATH     系统卷镜像（Ext4）：作为连续 ISO 文件存放，并写 Ext4 分区项\n"
            "  --iso-name NAME          系统卷在 ISO 内的文件名，默认取源文件名\n"
            "  --file ISO路径=宿主路径   附加文件，可重复（中间目录自动建立）\n"
            "  --raw 宿主路径[@偏移]     原始载荷，可重复；偏移须 2048 对齐，省略则自动分配\n"
            "\n"
            "ISO 元数据：\n"
            "  --volume-id ID           卷标识，默认 LIKESPROGRAM\n"
            "  --timestamp N            卷时间戳（UTC 秒）；默认取 SOURCE_DATE_EPOCH，未设则写全零\n"
            "  --manifest PATH          输出构建清单（JSON）\n"
            "  -h, --help               显示本说明\n";
    }

    // 参数解析
    Options ParseArgs(int argc, char** argv) {
        Options options;
        // 取当前选项的值，缺值即报错
        const auto needValue = [&](int& i, const std::string& flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(flag + " 缺少取值");
            return argv[++i];
        };
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") options.help = true;
            else if (arg == "--out") options.out = needValue(i, arg);
            else if (arg == "--volume-id") options.volume_id = needValue(i, arg);
            else if (arg == "--boot-image") options.boot_image = needValue(i, arg);
            else if (arg == "--load-segment") options.load_segment = static_cast<uint16_t>(ParseNumber(needValue(i, arg), "装入段"));
            else if (arg == "--mbr") options.mbr = needValue(i, arg);
            else if (arg == "--stub") options.stub = needValue(i, arg);
            else if (arg == "--core") options.core = needValue(i, arg);
            else if (arg == "--efi") options.efi = needValue(i, arg);
            else if (arg == "--esp") options.esp = needValue(i, arg);
            else if (arg == "--system-volume") options.system_volume = needValue(i, arg);
            else if (arg == "--iso-name") options.iso_name = needValue(i, arg);
            else if (arg == "--file") {
                const std::string value = needValue(i, arg);
                const auto eq = value.find('=');
                if (eq == std::string::npos || eq == 0 || eq + 1 == value.size()) throw std::runtime_error("--file 需要 ISO路径=宿主路径：" + value);
                options.files.emplace_back(value.substr(0, eq), value.substr(eq + 1));
            } else if (arg == "--raw") {
                const std::string value = needValue(i, arg);
                const auto at = value.rfind('@');
                if (at != std::string::npos && at + 1 < value.size()) options.raw.emplace_back(value.substr(0, at), ParseNumber(value.substr(at + 1), "原始载荷偏移"));
                else options.raw.emplace_back(value, std::nullopt);
            } else if (arg == "--manifest") options.manifest = needValue(i, arg);
            else if (arg == "--timestamp") options.timestamp = ParseNumber(needValue(i, arg), "时间戳");
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        return options;
    }

    // 待组装的布局与已放置的载荷
    struct Build {
        Options options;                                    // 生效的命令行选项
        IsoLayout layout;                                   // ISO 元数据布局
        uint32_t boot_lba = 0;                              // 引导镜像所在 LBA（混合镜像为 1）
        uint16_t boot_sectors_512 = 0;                      // 引导镜像的 LoadSize（512 字节单位）
        std::optional<Placed> stub;                         // 实模式服务层
        std::optional<Placed> core;                         // 核心阶段
        std::optional<Placed> efi;                          // El Torito EFI 引导镜像
        std::optional<Placed> esp;                          // ESP 的 FAT32 镜像
        std::optional<Placed> volume;                       // 系统卷镜像
        std::optional<std::array<uint8_t, 32>> descriptor;  // 已写入的 BootDescriptor
        std::optional<std::array<uint8_t, 32>> core_descriptor;  // 已写入的 CoreDescriptor
        std::vector<Placed> raw;                            // 原始载荷（--raw）
        std::vector<Placed> files;                          // 附加文件（--file）
        std::string volume_iso_path;                        // 系统卷在 ISO 内的路径
    };

    // 校验选项并固定引导镜像的位置，返回待组装的布局骨架
    Build Prepare(const Options& options) {
        Build build;
        build.options = options;

        const uint64_t bootBytes = FileSize(options.boot_image);
        if (bootBytes == 0) throw std::runtime_error("引导镜像为空：" + options.boot_image);
        if ((bootBytes + 511) / 512 > 0xFFFF) throw std::runtime_error("引导镜像超过 El Torito 的 16 位扇区计数上限");

        std::optional<std::vector<uint8_t>> mbr;
        if (!options.mbr.empty()) {
            mbr = ReadFile(options.mbr);
            if (mbr->size() != 512) throw std::runtime_error("MBR 必须恰好 512 字节：" + options.mbr);
        }
        build.boot_lba = mbr.has_value() ? 1 : 0;
        const uint64_t systemAreaLimit = build.boot_lba == 1 ? kSystemAreaBytes - kBlock : kSystemAreaBytes;
        if (bootBytes > systemAreaLimit) throw std::runtime_error("引导镜像放不进 16 扇区的系统区");
        build.boot_sectors_512 = static_cast<uint16_t>((bootBytes + 511) / 512);

        if (!options.stub.empty()) {
            const uint64_t bytes = FileSize(options.stub);
            // CD 按 2048 字节整块读取，装入空间最多容纳 0x8000 字节
            constexpr uint64_t maxBytes = BootDescriptor::kMaxStubBytes / kBlock * kBlock;
            if (bytes == 0 || bytes > maxBytes) throw std::runtime_error("Stub 长度必须非 0 且不超过 CD 整块读取上限 0x8000：" + options.stub);
        }
        if (options.core.empty() == false) if (FileSize(options.core) == 0) throw std::runtime_error("核心阶段载荷为空：" + options.core);
        if (!options.efi.empty()) {
            const uint64_t bytes = FileSize(options.efi);
            if (bytes == 0) throw std::runtime_error("El Torito EFI 引导镜像为空：" + options.efi);
            // 与 ESP 同一条规则：写死 FAT32
            RequireFat32Image(options.efi, "El Torito EFI 引导镜像");
            // FAT32 卷的体积下限（约 66500 个 512 字节扇区）超过 El Torito 的 16 位扇区计数上限，
            // 该字段按规范表达不了这份镜像：按上限 65535 写入，并在下面提示
            if ((bytes + 511) / 512 > 0xFFFF) {
                std::cerr << "MakeIso: El Torito 的扇区计数是 16 位（上限 65535 个 512 字节扇区），"
                          << "FAT32 镜像下限已超过它；引导目录里按上限写，" << options.efi
                          << "（" << (bytes + 511) / 512 << " 个扇区）实际按 FAT 卷自身尺寸读取\n";
            }
        }
        if (!options.esp.empty()) {
            if (FileSize(options.esp) == 0) throw std::runtime_error("ESP 镜像为空：" + options.esp);
            // ESP 写死为 FAT32：UEFI 固件读它，内核必备的 FAT32 实现也读它
            RequireFat32Image(options.esp, "ESP 镜像");
        }
        if (!options.iso_name.empty() && options.system_volume.empty()) throw std::runtime_error("--iso-name 只在提供 --system-volume 时有意义");
        return build;
    }

    // 按选项完成布局、分区表、引导目录、清单与镜像写入
    int Run(const Options& options) {
        Build build = Prepare(options);

        IsoVolume volume;
        volume.volume_id = options.volume_id;
        volume.timestamp = ResolveTimestamp(options);

        Image image(options.out);
        Iso9660 iso(image, volume);

        if (!options.system_volume.empty()) {
            const std::string name = options.iso_name.empty() ? BaseName(options.system_volume) : options.iso_name;
            build.volume_iso_path = iso.AddFile(name, options.system_volume, FileSize(options.system_volume));
        }
        for (const auto& [isoPath, hostPath] : options.files) iso.AddFile(isoPath, hostPath, FileSize(hostPath));

        build.layout = iso.PlanMetadata();

        Reservations reserved;
        reserved.Reserve("元数据区", 0, build.layout.metadata_end);
        uint64_t cursor = build.layout.metadata_end;
        // 按 2048 对齐分配一段连续空间并登记占用
        const auto place = [&](const std::string& name, uint64_t bytes) {
            cursor = AlignUp(cursor, kBlock);
            const Placed placed{name, cursor, bytes};
            reserved.Reserve(name, placed.offset, AlignUp(bytes, kBlock));
            cursor = placed.offset + AlignUp(bytes, kBlock);
            return placed;
        };

        if (!options.stub.empty()) build.stub = place("BaleenStub", FileSize(options.stub));
        if (!options.core.empty()) build.core = place("BaleenCore", FileSize(options.core));
        if (!options.efi.empty()) build.efi = place("El Torito EFI 镜像", FileSize(options.efi));
        for (const auto& [hostPath, offset] : options.raw) {
            const uint64_t bytes = FileSize(hostPath);
            if (offset.has_value()) {
                if (*offset % kBlock != 0) throw std::runtime_error("--raw 偏移未按 2048 对齐：" + hostPath);
                if (*offset < build.layout.metadata_end) throw std::runtime_error("--raw 偏移落在元数据区内：" + hostPath);
                const Placed placed{hostPath, *offset, bytes};
                reserved.Reserve(hostPath, placed.offset, AlignUp(bytes, kBlock));
                cursor = std::max(cursor, placed.offset + AlignUp(bytes, kBlock));
                build.raw.push_back(placed);
            } else build.raw.push_back(place(hostPath, bytes));
        }
        if (!options.esp.empty()) {
            const uint64_t bytes = FileSize(options.esp);
            const uint64_t offset = cursor <= kEspPreferredOffset ? AlignUp(cursor, kEspPreferredOffset) : AlignUp(cursor, kBlock);
            const Placed placed{"ESP 的 FAT32 镜像", offset, bytes};
            reserved.Reserve(placed.name, placed.offset, AlignUp(bytes, kBlock));
            cursor = placed.offset + AlignUp(bytes, kBlock);
            build.esp = placed;
        }

        std::map<std::string, IsoExtent> extents;
        for (const IsoFile& file : iso.Files()) {
            cursor = AlignUp(cursor, kBlock);
            const Placed placed{"ISO 文件 " + file.iso_path, cursor, file.size};
            if (file.size > 0) reserved.Reserve(placed.name, placed.offset, AlignUp(file.size, kBlock));
            cursor = placed.offset + AlignUp(file.size, kBlock);
            extents[file.iso_path] = IsoExtent{placed.offset, file.size};
            build.files.push_back(placed);
        }
        iso.SetFileExtents(extents);
        const uint64_t total = iso.TotalBytes();

        // 引导镜像：混合镜像里位于 LBA 1，纯光盘镜像里位于 LBA 0（描述符都落在绝对偏移 0x300）
        if (image.CopyFile(static_cast<uint64_t>(build.boot_lba) * kBlock, options.boot_image) != FileSize(options.boot_image))
            throw std::runtime_error("引导镜像长度在写入时发生变化");

        if (build.stub.has_value()) {
            BootDescriptor descriptor;
            descriptor.stub_offset = build.stub->offset;
            descriptor.stub_bytes = build.stub->bytes;
            build.descriptor = EncodeBootDescriptor(descriptor, kBlock);
            image.Write(BootDescriptor::kImageOffset, *build.descriptor);
        } else std::cerr << "MakeIso: 未提供 --stub，不写 BootDescriptor，BIOS 路径无法装载 Stub\n";

        // CoreDescriptor 与 BootDescriptor 同扇区：Stub 从同一扇区读到两者，缺它 Stub 不再往下装载
        if (build.core.has_value()) {
            CoreDescriptor coreDescriptor;
            coreDescriptor.core_offset = build.core->offset;
            coreDescriptor.core_bytes = build.core->bytes;
            build.core_descriptor = EncodeCoreDescriptor(coreDescriptor, kBlock);
            image.Write(CoreDescriptor::kImageOffset, *build.core_descriptor);
        } else std::cerr << "MakeIso: 未提供 --core，不写 CoreDescriptor，Stub 无法装载核心阶段\n";

        std::vector<MbrPartition> partitions;
        if (build.esp.has_value()) partitions.push_back(MbrPartition{0xEF, build.esp->offset, build.esp->bytes});
        std::optional<Placed> volumePlaced;
        if (!build.volume_iso_path.empty()) {
            const IsoExtent& extent = extents.at(build.volume_iso_path);
            if (extent.bytes > 0) {
                volumePlaced = Placed{build.volume_iso_path, extent.offset, extent.bytes};
                partitions.push_back(MbrPartition{0x83, extent.offset, extent.bytes});
            }
        }
        build.volume = volumePlaced;
        if (!options.mbr.empty()) {
            const std::vector<uint8_t> mbr = ReadFile(options.mbr);
            const auto patched = makeiso::PatchMbrPartitions(std::span<const uint8_t, 512>(mbr.data(), mbr.size()), std::span<const MbrPartition>(partitions.data(), partitions.size()));
            image.Write(0, patched);
        } else if (!partitions.empty()) std::cerr << "MakeIso: 未提供 --mbr，跳过分区项（纯光盘镜像不需要）\n";
        if (!options.mbr.empty() && !build.esp.has_value()) std::cerr << "MakeIso: 未提供 --esp，混合镜像里没有 ESP 分区，U 盘上 UEFI 侧起不来\n";
        if (build.esp.has_value() && !build.efi.has_value()) std::cerr << "MakeIso: 未提供 --efi，El Torito 没有 EFI 项，光盘 UEFI 侧起不来\n";

        BootCatalogSpec catalog;
        catalog.bios = BootCatalogEntry{build.boot_lba, FileSize(options.boot_image), options.load_segment};
        if (build.efi.has_value()) {
            BootCatalogEntry entry{static_cast<uint32_t>(build.efi->offset / kBlock), build.efi->bytes, 0};
            if ((build.efi->bytes + 511) / 512 > 0xFFFF) entry.sector_count = 0xFFFF;  // 字段写不下，按规范上限写
            catalog.efi = entry;
        }
        catalog.signature = options.volume_id;
        image.Write(static_cast<uint64_t>(build.layout.boot_catalog_lba) * kBlock, makeiso::EncodeBootCatalog(catalog));

        if (build.stub.has_value()) image.CopyFile(build.stub->offset, options.stub);
        if (build.core.has_value()) image.CopyFile(build.core->offset, options.core);
        for (const Placed& placed : build.raw) image.CopyFile(placed.offset, placed.name);
        if (build.efi.has_value()) image.CopyFile(build.efi->offset, options.efi);
        if (build.esp.has_value()) image.CopyFile(build.esp->offset, options.esp);
        iso.Write();
        image.ExtendTo(total);

        // —— 清单与摘要 ——
        std::string manifest = "{\n";
        manifest += "  \"tool\": \"MakeIso\",\n";
        manifest += "  \"volume_id\": \"" + JsonEscape(options.volume_id) + "\",\n";
        manifest += "  \"total_bytes\": " + std::to_string(total) + ",\n";
        manifest += "  \"total_sectors_2048\": " + std::to_string(total / kBlock) + ",\n";
        manifest += "  \"system_area_bytes\": " + std::to_string(kSystemAreaBytes) + ",\n";
        manifest += "  \"boot_image\": {\"lba\": " + std::to_string(build.boot_lba) +
                    ", \"offset\": " + std::to_string(static_cast<uint64_t>(build.boot_lba) * kBlock) +
                    ", \"bytes\": " + std::to_string(FileSize(options.boot_image)) +
                    ", \"sector_count_512\": " + std::to_string(build.boot_sectors_512) +
                    ", \"load_segment\": " + std::to_string(options.load_segment) + "},\n";
        manifest += "  \"boot_catalog\": {\"lba\": " + std::to_string(build.layout.boot_catalog_lba) +
                    ", \"offset\": " + std::to_string(static_cast<uint64_t>(build.layout.boot_catalog_lba) * kBlock) +
                    ", \"bytes\": " + std::to_string(kBlock) + "},\n";
        if (build.descriptor.has_value()) {
            manifest += "  \"descriptor\": {\"offset\": " + std::to_string(BootDescriptor::kImageOffset) +
                        ", \"bytes\": " + std::to_string(BootDescriptor::kHeaderBytes) +
                        ", \"stub_offset\": " + std::to_string(build.stub->offset) +
                        ", \"stub_bytes\": " + std::to_string(build.stub->bytes) + "},\n";
        } else manifest += "  \"descriptor\": null,\n";
        if (build.core_descriptor.has_value()) {
            manifest += "  \"core_descriptor\": {\"offset\": " + std::to_string(CoreDescriptor::kImageOffset) +
                        ", \"bytes\": " + std::to_string(CoreDescriptor::kHeaderBytes) +
                        ", \"core_offset\": " + std::to_string(build.core->offset) +
                        ", \"core_bytes\": " + std::to_string(build.core->bytes) + "},\n";
        } else manifest += "  \"core_descriptor\": null,\n";
        manifest += "  \"stub\": " + JsonPlaced(build.stub.has_value() ? &*build.stub : nullptr) + ",\n";
        manifest += "  \"core\": " + JsonPlaced(build.core.has_value() ? &*build.core : nullptr) + ",\n";
        if (build.efi.has_value()) {
            const uint64_t exact = (build.efi->bytes + 511) / 512;
            const uint64_t written = exact > 0xFFFF ? 0xFFFF : exact;
            manifest += "  \"efi_image\": {\"offset\": " + std::to_string(build.efi->offset) +
                        ", \"bytes\": " + std::to_string(build.efi->bytes) +
                        ", \"sector_count_512\": " + std::to_string(written) +
                        ", \"sector_count_exact\": " + std::to_string(exact) +
                        ", \"sector_count_clamped\": " + std::string(exact > 0xFFFF ? "true" : "false") + "},\n";
        } else manifest += "  \"efi_image\": null,\n";
        manifest += "  \"esp\": " + JsonPlaced(build.esp.has_value() ? &*build.esp : nullptr) + ",\n";
        if (build.volume.has_value()) {
            manifest += "  \"system_volume\": {\"iso_path\": \"" + JsonEscape(build.volume_iso_path) +
                        "\", \"offset\": " + std::to_string(build.volume->offset) +
                        ", \"bytes\": " + std::to_string(build.volume->bytes) + "},\n";
        } else manifest += "  \"system_volume\": null,\n";
        manifest += "  \"files\": [";
        for (std::size_t i = 0; i < build.files.size(); ++i) {
            manifest += (i == 0 ? "\n    " : ",\n    ");
            manifest += "{\"iso_path\": \"" + JsonEscape(build.files[i].name.substr(std::string("ISO 文件 ").size())) +
                        "\", \"offset\": " + std::to_string(build.files[i].offset) +
                        ", \"bytes\": " + std::to_string(build.files[i].bytes) + "}";
        }
        manifest += build.files.empty() ? "],\n" : "\n  ],\n";
        manifest += "  \"raw\": [";
        for (std::size_t i = 0; i < build.raw.size(); ++i) {
            manifest += (i == 0 ? "\n    " : ",\n    ");
            manifest += "{\"host_path\": \"" + JsonEscape(build.raw[i].name) +
                        "\", \"offset\": " + std::to_string(build.raw[i].offset) +
                        ", \"bytes\": " + std::to_string(build.raw[i].bytes) + "}";
        }
        manifest += build.raw.empty() ? "]\n" : "\n  ]\n";
        manifest += "}\n";

        if (!options.manifest.empty()) {
            std::ofstream out(options.manifest, std::ios::binary | std::ios::trunc);
            if (!out || !(out << manifest)) throw std::runtime_error("无法写清单：" + options.manifest);
        }

        std::cout << "MakeIso：已生成 " << options.out << "（" << total << " 字节，" << total / kBlock << " 个 2048 扇区）\n";
        std::cout << "  引导镜像    LBA " << build.boot_lba << "，偏移 " << static_cast<uint64_t>(build.boot_lba) * kBlock << "，LoadSize " << build.boot_sectors_512 << "（512 字节单位）\n";
        if (build.descriptor.has_value()) std::cout << "  BootDescriptor 偏移 " << BootDescriptor::kImageOffset << "，Stub 偏移 " << build.stub->offset << "，Stub 字节 " << build.stub->bytes << "\n";
        if (build.core.has_value()) std::cout << "  BaleenCore  偏移 " << build.core->offset << "，字节 " << build.core->bytes << "\n";
        if (build.core_descriptor.has_value()) std::cout << "  CoreDescriptor 偏移 " << CoreDescriptor::kImageOffset << "，Core 偏移 " << build.core->offset << "，Core 字节 " << build.core->bytes << "\n";
        if (build.efi.has_value()) std::cout << "  El Torito EFI 镜像 偏移 " << build.efi->offset << "，字节 " << build.efi->bytes << "\n";
        if (build.esp.has_value()) std::cout << "  ESP（FAT32）偏移 " << build.esp->offset << "，字节 " << build.esp->bytes << "（0xEF 分区）\n";
        if (build.volume.has_value()) std::cout << "  系统卷      ISO 路径 " << build.volume_iso_path << "，偏移 " << build.volume->offset << "，字节 " << build.volume->bytes << "（Ext4 分区）\n";
        return 0;
    }
}

// 入口：解析参数、检查必选选项，然后组装镜像
int main(int argc, char** argv) {
    try {
        const Options options = ParseArgs(argc, argv);
        if (options.help || argc == 1) {
            Usage();
            return options.help ? 0 : 2;
        }
        if (options.out.empty()) throw std::runtime_error("缺少 --out");
        if (options.boot_image.empty()) throw std::runtime_error("缺少 --boot-image");
        return Run(options);
    } catch (const std::exception& error) {
        std::cerr << "MakeIso: " << error.what() << "\n";
        return 1;
    }
}
