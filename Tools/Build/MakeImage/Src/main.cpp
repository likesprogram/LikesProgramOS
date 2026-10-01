/* main.cpp
    MakeImage：通用镜像制作器，把一级引导、实模式服务层、核心阶段、EFI 引导镜像与系统卷组装成一份可引导镜像

    混合镜像同时带光盘与磁盘两条引导路径：刻录到光盘、写入 U 盘或移动硬盘、由虚拟机直接挂载
    ISO 文件都能启动；同一份字节覆盖这三种投递方式，不需要分别组装
*/

#include <Baleen.h>
#include <Fat.h>
#include <HostIo.h>
#include <Image.h>
#include <Iso9660.h>
#include <BaleenImage.h>

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
    using hostbuild::AlignUp;
    using hostbuild::BaseName;
    using hostbuild::BootCatalogEntry;
    using hostbuild::BootCatalogSpec;
    using hostbuild::DescriptorArea;
    using hostbuild::CheckPayload;
    using hostbuild::BuildDescriptorArea;
    using hostbuild::FileSize;
    using hostbuild::RequireFat16Image;
    using hostbuild::Image;
    using hostbuild::Iso9660;
    using hostbuild::IsoExtent;
    using hostbuild::IsoFile;
    using hostbuild::IsoLayout;
    using hostbuild::IsoVolume;
    using hostbuild::JsonEscape;
    using hostbuild::JsonPlaced;
    using hostbuild::MbrPartition;
    using hostbuild::ParseNumber;
    using hostbuild::Placed;
    using hostbuild::ReadFile;
    using hostbuild::Reservations;
    using hostbuild::ImageKind;
    using hostbuild::PayloadInfo;
    using hostbuild::PayloadSummary;

    constexpr uint64_t kBlock = 2048;                    // ISO9660 逻辑块大小
    constexpr uint64_t kSystemAreaBytes = 16 * kBlock;   // 系统区 16 个逻辑块
    // 原始载荷与镜像总长按 4096 对齐：混合镜像写进 4Kn 存储后仍能整扇区读取
    constexpr uint64_t kPayloadAlign = 4096;
    // ESP 的两份位置：0xEF 分区项的起始 LBA 固定按 512 字节单位写 2048，与 Ipl/Src/Mbr.asm 的占位值一致。
    // 512e 固件按 512 解释该项、读到 1 MiB，4Kn 固件按 4096 解释同一项、读到 8 MiB；
    // 两处各放一份同样的 FAT 卷，同一份镜像在两种设备上都能被固件找到 ESP
    constexpr uint64_t kEspPartitionLba = 2048;                  // 0xEF 项的起始 LBA，固定 512 单位
    constexpr uint64_t kEspOffset512 = kEspPartitionLba * 512;   // 512e 固件读到的位置：1 MiB
    constexpr uint64_t kEspOffset4Kn = kEspPartitionLba * 4096;  // 4Kn 固件读到的位置：8 MiB

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
        std::string esp;                                                   // 混合镜像的 ESP 分区内容（必须 FAT16）
        std::string system_volume;                                         // 系统卷镜像（Ext4）
        std::string iso_name;                                              // 系统卷在 ISO 内的文件名
        std::vector<std::pair<std::string, std::string>> files;            // ISO 路径 -> 宿主路径
        std::vector<std::pair<std::string, std::optional<uint64_t>>> raw;  // 宿主路径 -> 偏移
        std::string manifest;                                              // 构建清单输出路径
        std::optional<uint64_t> timestamp;                                 // 卷时间戳；空 = 写全零
        uint32_t sector_bytes = 512;                                       // 混合 MBR 的 LBA 单位：512 或 4096
        std::string descs = "Packages/Baleen/Common/Out/Descs";            // 描述符段产物目录：头部与各段拆开，本工具只回填起止位置
        bool stub_unchecked = false;                                       // 是否免除 Stub 头与摘要校验
        bool core_unchecked = false;                                       // 是否免除 Core 头与摘要校验
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
            "MakeImage — 通用镜像制作器：把引导链与系统卷组装成一份可启动镜像\n"
            "（ISO9660 + El Torito + 混合 MBR；可刻录光盘、写入 U 盘或移动硬盘、虚拟机挂载）\n"
            "\n"
            "用法：MakeImage --out <镜像> --boot-image <BaleenIPLCd.bin> [选项]\n"
            "\n"
            "引导与载荷：\n"
            "  --boot-image PATH        El Torito 默认（BIOS）引导镜像，如 BaleenIPLCd.bin\n"
            "  --load-segment N         引导镜像装入段，默认 0x7C0（与一级引导的寻址一致）\n"
            "  --mbr PATH               混合镜像用 MBR（BaleenIPL.bin，512 字节），写到镜像偏移 0\n"
            "  --stub PATH              实模式服务层，原始扇区放置，位置写进描述符区\n"
            "  --core PATH              核心阶段，原始扇区放置，位置写进描述符区\n"
            "  --descs DIR              描述符段产物目录（默认 Packages/Baleen/Common/Out/Descs）\n"
            "  --efi PATH               El Torito 的 EFI 引导镜像（FAT16，供光盘 UEFI 引导）\n"
            "  --esp PATH               混合镜像的 ESP 分区内容（必须 FAT16）\n"
            "  --system-volume PATH     系统卷镜像（Ext4）：作为连续 ISO 文件存放，并写 Ext4 分区项\n"
            "  --iso-name NAME          系统卷在 ISO 内的文件名，默认取源文件名\n"
            "  --file ISO路径=宿主路径   附加文件，可重复（中间目录自动建立）\n"
            "  --raw 宿主路径[@偏移]     原始载荷，可重复；偏移须 4096 对齐，省略则自动分配\n"
            "  --stub-unchecked         免除 Stub 头与摘要校验：只给测试夹具与特殊用途，正式构建不要用\n"
            "  --core-unchecked         免除 Core 头与摘要校验：只给测试夹具与特殊用途，正式构建不要用\n"
            "\n"
            "ISO 元数据：\n"
            "  --volume-id ID           卷标识，默认 LIKESPROGRAM\n"
            "  --sector-bytes N         混合 MBR 的 LBA 单位：512（默认）或 4096；ISO 结构仍为 2048 字节块\n"
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
            else if (arg == "--descs") options.descs = needValue(i, arg);
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
            else if (arg == "--sector-bytes") {
                const uint64_t value = ParseNumber(needValue(i, arg), "逻辑扇区大小");
                if (value != 512 && value != 4096) throw std::runtime_error("--sector-bytes 只能是 512 或 4096");
                options.sector_bytes = static_cast<uint32_t>(value);
            } else if (arg == "--timestamp") options.timestamp = ParseNumber(needValue(i, arg), "时间戳");
            else if (arg == "--stub-unchecked") options.stub_unchecked = true;
            else if (arg == "--core-unchecked") options.core_unchecked = true;
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
        std::optional<Placed> esp;                          // ESP 的 FAT16 镜像（512e 位置：1 MiB）
        std::optional<Placed> esp_4kn;                      // ESP 的同一份镜像（4Kn 位置：8 MiB）
        std::optional<Placed> volume;                       // 系统卷镜像
        // 描述符区由 --descs 的段产物拼成，本工具只回填其中的位置字段
        std::vector<Placed> raw;                            // 原始载荷（--raw）
        std::vector<Placed> files;                          // 附加文件（--file）
        std::string volume_iso_path;                        // 系统卷在 ISO 内的路径
        PayloadInfo stub_info;                              // Stub 载荷的门禁结果
        PayloadInfo core_info;                              // Core 载荷的门禁结果
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
            constexpr uint64_t maxBytes = DescriptorArea::kMaxStubBytes / kBlock * kBlock;
            if (bytes == 0 || bytes > maxBytes) throw std::runtime_error("Stub 长度必须非 0 且不超过 CD 整块读取上限 0x8000：" + options.stub);
            // 带头产物校验头字段与摘要，占位内容放行并在 stderr 说明
            build.stub_info = CheckPayload(options.stub, ImageKind::Stub, options.stub_unchecked, "--stub-unchecked");
        }
        if (!options.core.empty()) {
            if (FileSize(options.core) == 0) throw std::runtime_error("核心阶段载荷为空：" + options.core);
            build.core_info = CheckPayload(options.core, ImageKind::Core, options.core_unchecked, "--core-unchecked");
        }
        if (!options.efi.empty()) {
            const uint64_t bytes = FileSize(options.efi);
            if (bytes == 0) throw std::runtime_error("El Torito EFI 引导镜像为空：" + options.efi);
            // 与 ESP 同一条规则：必须是 FAT16
            RequireFat16Image(options.efi, "El Torito EFI 引导镜像");
            // 16 位扇区计数容得下 4 MiB 的 FAT16；镜像更大时按规范上限写，并在下面提示
            if ((bytes + 511) / 512 > 0xFFFF) {
                std::cerr << "MakeImage: El Torito 的扇区计数是 16 位（上限 65535 个 512 字节扇区），"
                          << "引导目录里按上限写，" << options.efi
                          << "（" << (bytes + 511) / 512 << " 个扇区）实际按 FAT 卷自身尺寸读取\n";
            }
        }
        if (!options.esp.empty()) {
            if (FileSize(options.esp) == 0) throw std::runtime_error("ESP 镜像为空：" + options.esp);
            // ESP 必须是 FAT16：双 ESP 布局要求它同时落在 1 MiB 与 8 MiB、体积不超过 7 MiB
            RequireFat16Image(options.esp, "ESP 镜像");
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
        // 描述符区（0x22000 起）不得被 ISO 元数据覆盖
        if (build.layout.metadata_end > DescriptorArea::kWriteOffset) throw std::runtime_error("ISO 元数据区越过描述符区位置 0x22000，布局需要后移");
        // 载荷起点在元数据之后，且不早于描述符区之后：512 与 4096 两种单位下都安全
        uint64_t cursor = std::max<uint64_t>(build.layout.metadata_end, 0x23000);
        // 按 4096 对齐分配一段连续空间并登记占用：载荷在 512 / 2048 / 4096 逻辑扇区下都整扇区可读
        const auto place = [&](const std::string& name, uint64_t bytes) {
            cursor = AlignUp(cursor, kPayloadAlign);
            const Placed placed{name, cursor, bytes};
            reserved.Reserve(name, placed.offset, AlignUp(bytes, kPayloadAlign));
            cursor = placed.offset + AlignUp(bytes, kPayloadAlign);
            return placed;
        };

        if (!options.stub.empty()) build.stub = place("BaleenStub", FileSize(options.stub));
        if (!options.core.empty()) build.core = place("BaleenCore", FileSize(options.core));
        for (const auto& [hostPath, offset] : options.raw) {
            const uint64_t bytes = FileSize(hostPath);
            if (offset.has_value()) {
                if (*offset % kPayloadAlign != 0) throw std::runtime_error("--raw 偏移未按 4096 对齐：" + hostPath);
                if (*offset < build.layout.metadata_end) throw std::runtime_error("--raw 偏移落在元数据区内：" + hostPath);
                const Placed placed{hostPath, *offset, bytes};
                reserved.Reserve(hostPath, placed.offset, AlignUp(bytes, kPayloadAlign));
                cursor = std::max(cursor, placed.offset + AlignUp(bytes, kPayloadAlign));
                build.raw.push_back(placed);
            } else build.raw.push_back(place(hostPath, bytes));
        }
        // ESP：同一份 FAT 卷放两处，让 512e 与 4Kn 固件从同一个分区项各自读到一份（见 kEspOffset512 的说明）
        if (!options.esp.empty()) {
            const uint64_t bytes = FileSize(options.esp);
            const uint64_t span = AlignUp(bytes, kPayloadAlign);
            if (kEspOffset512 + span > kEspOffset4Kn) throw std::runtime_error("ESP 镜像过大：1 MiB 处那份会盖住 8 MiB 处的位置（上限 7 MiB）");
            build.esp = Placed{"ESP 的 FAT16 镜像（512e 位置）", kEspOffset512, bytes};
            build.esp_4kn = Placed{"ESP 的 FAT16 镜像（4Kn 位置）", kEspOffset4Kn, bytes};
            reserved.Reserve(build.esp->name, kEspOffset512, span);
            reserved.Reserve(build.esp_4kn->name, kEspOffset4Kn, span);
            // 0xEF 分区项在 4Kn 视角下的范围是 [8 MiB, 8 MiB + 扇区数 × 4096)，后续内容让开它，避免与分区范围重叠
            cursor = kEspOffset4Kn + (bytes + 511) / 512 * 4096;
        }

        // El Torito EFI 引导镜像：位置与分区表无关（引导目录按 LBA 引用），排在 ESP 让出的范围之后
        if (!options.efi.empty()) build.efi = place("El Torito EFI 引导镜像", FileSize(options.efi));

        std::map<std::string, IsoExtent> extents;
        for (const IsoFile& file : iso.Files()) {
            // ISO 文件按 4096 对齐分配：既是 2048 逻辑块的整数倍，也让分区表在 512 / 4096 两种 LBA 单位下都能描述起点
            cursor = AlignUp(cursor, kPayloadAlign);
            const Placed placed{"ISO 文件 " + file.iso_path, cursor, file.size};
            if (file.size > 0) reserved.Reserve(placed.name, placed.offset, AlignUp(file.size, kPayloadAlign));
            cursor = placed.offset + AlignUp(file.size, kPayloadAlign);
            extents[file.iso_path] = IsoExtent{placed.offset, file.size};
            build.files.push_back(placed);
        }
        iso.SetFileExtents(extents);

        // 引导镜像：混合镜像里位于 LBA 1，纯光盘镜像里位于 LBA 0（描述符都落在绝对偏移 0x22000）
        if (image.CopyFile(static_cast<uint64_t>(build.boot_lba) * kBlock, options.boot_image) != FileSize(options.boot_image))
            throw std::runtime_error("引导镜像长度在写入时发生变化");

        // 描述符区：读段产物、回填起止位置后写到约定位置；本工具不生成描述符结构
        // 位置字段按 512 字节基准写，读取方按运行期单位换算：同一份字节在光盘、512e 与 4Kn 下指向同一处
        if (build.stub.has_value() || build.core.has_value()) {
            const std::vector<uint8_t> area = BuildDescriptorArea(options.descs, build.stub ? &*build.stub : nullptr, build.core ? &*build.core : nullptr);
            image.Write(DescriptorArea::kWriteOffset, area);
        } else std::cerr << "MakeImage: 未提供 --stub 与 --core，不写描述符区，BIOS 路径无法装载\n";

        std::optional<Placed> volumePlaced;
        if (!build.volume_iso_path.empty()) {
            const IsoExtent& extent = extents.at(build.volume_iso_path);
            if (extent.bytes > 0) volumePlaced = Placed{build.volume_iso_path, extent.offset, extent.bytes};
        }
        build.volume = volumePlaced;
        if (!options.mbr.empty()) {
            std::vector<uint8_t> mbr = ReadFile(options.mbr);
            std::fill(mbr.begin() + 446, mbr.begin() + 446 + 64, 0);   // 四个槽先清空，再写有内容的
            // 0xEF 项固定按 512 字节单位写：512e 与 4Kn 固件从同一个值分别读到 1 MiB 与 8 MiB 处的两份 ESP
            if (build.esp.has_value()) hostbuild::WriteMbrEntry(std::span<uint8_t, 512>(mbr.data(), mbr.size()), 0, MbrPartition{0xEF, kEspOffset512, build.esp->bytes}, 512);
            // Ext4 系统卷按目标设备单位写：内核对系统卷用布局描述定位，不依赖这一项
            if (volumePlaced.has_value()) hostbuild::WriteMbrEntry(std::span<uint8_t, 512>(mbr.data(), mbr.size()), 1, MbrPartition{0x83, volumePlaced->offset, volumePlaced->bytes}, options.sector_bytes);
            image.Write(0, mbr);
        } else if (build.esp.has_value() || volumePlaced.has_value()) std::cerr << "MakeImage: 未提供 --mbr，跳过分区项（纯光盘镜像不需要）\n";
        if (!options.mbr.empty() && !build.esp.has_value()) std::cerr << "MakeImage: 未提供 --esp，混合镜像里没有 ESP 分区，U 盘上 UEFI 侧起不来\n";
        if (build.esp.has_value() && !build.efi.has_value()) std::cerr << "MakeImage: 未提供 --efi，El Torito 没有 EFI 项，光盘 UEFI 侧起不来\n";

        BootCatalogSpec catalog;
        catalog.bios = BootCatalogEntry{build.boot_lba, FileSize(options.boot_image), options.load_segment};
        if (build.efi.has_value()) {
            BootCatalogEntry entry{static_cast<uint32_t>(build.efi->offset / kBlock), build.efi->bytes, 0};
            if ((build.efi->bytes + 511) / 512 > 0xFFFF) entry.sector_count = 0xFFFF;  // 字段写不下，按规范上限写
            catalog.efi = entry;
        }
        catalog.signature = options.volume_id;
        image.Write(static_cast<uint64_t>(build.layout.boot_catalog_lba) * kBlock, hostbuild::EncodeBootCatalog(catalog));

        if (build.stub.has_value()) image.CopyFile(build.stub->offset, options.stub);
        if (build.core.has_value()) image.CopyFile(build.core->offset, options.core);
        for (const Placed& placed : build.raw) image.CopyFile(placed.offset, placed.name);
        if (build.efi.has_value()) image.CopyFile(build.efi->offset, options.efi);
        // 同一份 ESP 写两处：512e 与 4Kn 固件各自从自己读到的那份挂载
        if (build.esp.has_value()) image.CopyFile(build.esp->offset, options.esp);
        if (build.esp_4kn.has_value()) image.CopyFile(build.esp_4kn->offset, options.esp);
        iso.Write();
        // 总长按 4096 对齐：覆盖 ISO 结构、描述符区与全部引导载荷，尾部不整扇区的载荷也要补到整扇区
        // 载荷末尾即读盘范围的上界，Stub 按整扇区读取，缺这段填充会读盘越界
        const uint64_t total = AlignUp(std::max(iso.TotalBytes(), image.Size()), kPayloadAlign);
        image.ExtendTo(total);

        // —— 清单与摘要 ——
        std::string manifest = "{\n";
        manifest += "  \"tool\": \"MakeImage\",\n";
        manifest += "  \"volume_id\": \"" + JsonEscape(options.volume_id) + "\",\n";
        manifest += "  \"total_bytes\": " + std::to_string(total) + ",\n";
        manifest += "  \"sector_bytes\": " + std::to_string(options.sector_bytes) + ",\n";
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
        // 描述符区由 --descs 的段产物拼成，这里只记录本区写在哪；位置字段以 512 字节为基准
        if (build.stub.has_value() || build.core.has_value()) {
            manifest += "  \"descriptor_area\": {\"offset\": " + std::to_string(DescriptorArea::kWriteOffset) +
                        ", \"base_bytes\": " + std::to_string(1u << DescriptorArea::kBaseShift) +
                        ", \"descs\": \"" + JsonEscape(options.descs) + "\"},\n";
        } else manifest += "  \"descriptor_area\": null,\n";
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
        manifest += "  \"esp_4kn\": " + JsonPlaced(build.esp_4kn.has_value() ? &*build.esp_4kn : nullptr) + ",\n";
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

        std::cout << "MakeImage：已生成 " << options.out << "（" << total << " 字节，" << total / kBlock << " 个 2048 扇区）\n";
        std::cout << "  引导镜像    LBA " << build.boot_lba << "，偏移 " << static_cast<uint64_t>(build.boot_lba) * kBlock << "，LoadSize " << build.boot_sectors_512 << "（512 字节单位）\n";
        if (build.stub.has_value() || build.core.has_value()) std::cout << "  描述符区    偏移 " << DescriptorArea::kWriteOffset << "，段产物目录 " << options.descs << "\n";
        if (build.stub.has_value()) std::cout << "  Stub 载荷   偏移 " << build.stub->offset << "，字节 " << build.stub->bytes << "\n";
        if (build.stub.has_value()) std::cout << "  Stub 载荷：" << PayloadSummary(build.stub_info) << "\n";
        if (build.core.has_value()) std::cout << "  Core 载荷   偏移 " << build.core->offset << "，字节 " << build.core->bytes << "\n";
        if (build.core.has_value()) std::cout << "  Core 载荷：" << PayloadSummary(build.core_info) << "\n";
        if (build.efi.has_value()) std::cout << "  El Torito EFI 镜像 偏移 " << build.efi->offset << "，字节 " << build.efi->bytes << "\n";
        if (build.esp.has_value()) std::cout << "  ESP（FAT16）偏移 " << build.esp->offset << " 与 " << build.esp_4kn->offset << "，字节 " << build.esp->bytes << "（0xEF 分区，两份）\n";
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
        std::cerr << "MakeImage: " << error.what() << "\n";
        return 1;
    }
}
