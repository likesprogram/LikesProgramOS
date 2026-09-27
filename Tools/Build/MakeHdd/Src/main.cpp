// main.cpp
// MakeHdd：把磁盘一级引导（MBR）、实模式服务层、核心阶段、ESP 的 FAT 镜像与 Ext4 系统卷组装成可写入硬盘 / U 盘的磁盘镜像
//
// 布局（字节偏移）：
//   0x000        MBR：BaleenIPL.bin 的引导代码 + 由本工具写实的分区表
//   0x300        BootDescriptor（32 字节；一级引导从 512 字节 LBA 1 的偏移 0x100 读它）
//   0x400 起     原始载荷：Stub、Core 与 --raw 条目，各自 2048 对齐
//   1 MiB（原始载荷越过 1 MiB 时顺延）  0 号分区：ESP 的 FAT 镜像
//   ESP 之后     1 号分区：Ext4 系统卷镜像
//   总长向上对齐到 --pad-to（默认 1 MiB）

#include "Baleen.h"
#include "Fat.h"
#include "HostIo.h"
#include "Image.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    using makeiso::AlignUp;
    using makeiso::BootDescriptor;
    using makeiso::FileSize;
    using makeiso::RequireFat32Image;
    using makeiso::Image;
    using makeiso::JsonEscape;
    using makeiso::JsonPlaced;
    using makeiso::MbrPartition;
    using makeiso::ParseNumber;
    using makeiso::Placed;
    using makeiso::ReadFile;
    using makeiso::Reservations;

    constexpr uint64_t kMbrBytes = 512;     // 分区项从 446 起，510 起是 0xAA55
    constexpr uint64_t kEntryBytes = 16 * 4;
    constexpr uint64_t kAlign = 2048;       // 载荷与分区起点一律 2048 对齐
    constexpr uint64_t kRawStart = 0x400;   // BootDescriptor 之后
    // 与 Ipl/Src/Mbr.asm 分区项 0 的占位 LBA 2048（512 字节单位）一致：ESP 优先落在 1 MiB
    constexpr uint64_t kEspPreferredOffset = 1u << 20;
    constexpr uint64_t kDefaultPadTo = 1u << 20;

    // 参数
    struct Options {
        std::string out;                                                   // 输出镜像路径
        std::string mbr;                                                   // MBR 文件路径
        std::string stub;                                                  // Stub 文件路径
        std::string core;                                                  // BootCore 文件路径
        std::string esp;                                                   // Esp 文件路径
        std::string system_volume;                                         // 系统卷文件路径
        std::string manifest;                                              // 构建清单输出路径
        uint64_t pad_to = kDefaultPadTo;                                   // 镜像总长的对齐粒度
        bool help = false;                                                 // 是否只要帮助
        std::vector<std::pair<std::string, std::optional<uint64_t>>> raw;  // 宿主路径 -> 偏移，空表示自动分配
    };

    // 分区
    struct Partitions {
        std::optional<Placed> esp;             // 0 号槽：类型 0xEF
        std::optional<Placed> system_volume;   // 1 号槽：类型 0x83
    };

    // 打印用法
    void Usage() {
        std::cout <<
            "MakeHdd — Baleen 磁盘引导介质（MBR + ESP + Ext4）组装器\n"
            "\n"
            "用法：MakeHdd --out <镜像> --mbr <BaleenIPL.bin> [选项]\n"
            "\n"
            "载荷：\n"
            "  --mbr PATH                磁盘一级引导（BaleenIPL.bin，512 字节、带 0xAA55）\n"
            "  --stub PATH               实模式服务层：原始扇区放置并写 BootDescriptor\n"
            "  --core PATH               核心阶段：原始扇区放置（位置进清单）\n"
            "  --esp PATH                ESP 的 FAT32 镜像（含 \\EFI\\BOOT\\BOOTX64.EFI），0 号分区\n"
            "  --system-volume PATH      Ext4 系统卷镜像，1 号分区\n"
            "  --raw 宿主路径[@偏移]      原始载荷，可重复；偏移须 2048 对齐，省略则自动分配\n"
            "\n"
            "镜像：\n"
            "  --pad-to N                镜像总长向上对齐的粒度，默认 0x100000（1 MiB）\n"
            "  --manifest PATH           输出构建清单（JSON）\n"
            "  -h, --help                显示本说明\n";
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
            else if (arg == "--mbr") options.mbr = needValue(i, arg);
            else if (arg == "--stub") options.stub = needValue(i, arg);
            else if (arg == "--core") options.core = needValue(i, arg);
            else if (arg == "--esp") options.esp = needValue(i, arg);
            else if (arg == "--system-volume") options.system_volume = needValue(i, arg);
            else if (arg == "--raw") {
                const std::string value = needValue(i, arg);
                const auto at = value.rfind('@');
                if (at != std::string::npos && at + 1 < value.size()) options.raw.emplace_back(value.substr(0, at), ParseNumber(value.substr(at + 1), "原始载荷偏移"));
                else options.raw.emplace_back(value, std::nullopt);
            } else if (arg == "--manifest") options.manifest = needValue(i, arg);
            else if (arg == "--pad-to") options.pad_to = ParseNumber(needValue(i, arg), "对齐粒度");
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        return options;
    }

    // 参数验证
    void Validate(const Options& options) {
        if (FileSize(options.mbr) != kMbrBytes) throw std::runtime_error("MBR 必须恰好 512 字节：" + options.mbr);
        const std::vector<uint8_t> mbr = ReadFile(options.mbr);
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) throw std::runtime_error("MBR 缺少 0xAA55 引导签名：" + options.mbr);
        if (!options.stub.empty()) {
            const uint64_t bytes = FileSize(options.stub);
            if (bytes == 0 || bytes > BootDescriptor::kMaxStubBytes) throw std::runtime_error("Stub 长度必须非 0 且不超过 0x10000-0x7E00：" + options.stub);
        }
        if (!options.core.empty() && FileSize(options.core) == 0) throw std::runtime_error("核心阶段载荷为空：" + options.core);
        if (!options.esp.empty()) {
            if (FileSize(options.esp) == 0) throw std::runtime_error("ESP 镜像为空：" + options.esp);
            // ESP 写死为 FAT32：UEFI 固件读它，内核必备的 FAT32 实现也读它
            RequireFat32Image(options.esp, "ESP 镜像");
        }
        if (!options.system_volume.empty() && FileSize(options.system_volume) == 0) throw std::runtime_error("系统卷镜像为空：" + options.system_volume);
        if (options.pad_to == 0 || options.pad_to % kAlign != 0) throw std::runtime_error("--pad-to 必须是 2048 的正整数倍");
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
        if (options.mbr.empty()) throw std::runtime_error("缺少 --mbr");
        Validate(options);

        Image image(options.out);
        Reservations reserved;
        reserved.Reserve("MBR 与 BootDescriptor 区", 0, kRawStart);
        uint64_t cursor = kRawStart;
        // 按 2048 对齐分配一段连续空间并登记占用
        const auto place = [&](const std::string& name, uint64_t bytes) {
            cursor = AlignUp(cursor, kAlign);
            const Placed placed{name, cursor, bytes};
            reserved.Reserve(name, placed.offset, AlignUp(bytes, kAlign));
            cursor = placed.offset + AlignUp(bytes, kAlign);
            return placed;
        };

        std::optional<Placed> stub;
        std::optional<Placed> core;
        std::vector<Placed> raw;
        if (!options.stub.empty()) stub = place("BaleenStub", FileSize(options.stub));
        if (!options.core.empty()) core = place("BaleenCore", FileSize(options.core));
        for (const auto& [hostPath, offset] : options.raw) {
            const uint64_t bytes = FileSize(hostPath);
            if (offset.has_value()) {
                if (*offset % kAlign != 0) throw std::runtime_error("--raw 偏移未按 2048 对齐：" + hostPath);
                if (*offset < kRawStart) throw std::runtime_error("--raw 偏移落在 MBR / 描述符区内：" + hostPath);
                const Placed placed{hostPath, *offset, bytes};
                reserved.Reserve(hostPath, placed.offset, AlignUp(bytes, kAlign));
                cursor = std::max(cursor, placed.offset + AlignUp(bytes, kAlign));
                raw.push_back(placed);
            } else raw.push_back(place(hostPath, bytes));
        }

        Partitions partitions;
        if (!options.esp.empty()) {
            const uint64_t bytes = FileSize(options.esp);
            const uint64_t offset = cursor <= kEspPreferredOffset ? AlignUp(cursor, kEspPreferredOffset) : AlignUp(cursor, kAlign);
            const Placed placed{"ESP 的 FAT32 镜像", offset, bytes};
            reserved.Reserve(placed.name, placed.offset, AlignUp(bytes, kAlign));
            cursor = placed.offset + AlignUp(bytes, kAlign);
            partitions.esp = placed;
        } else std::cerr << "MakeHdd: 未提供 --esp，0 号分区留空，UEFI 侧无法从该镜像启动\n";
        if (!options.system_volume.empty()) {
            const uint64_t bytes = FileSize(options.system_volume);
            const Placed placed{"Ext4 系统卷", AlignUp(cursor, kAlign), bytes};
            reserved.Reserve(placed.name, placed.offset, AlignUp(bytes, kAlign));
            cursor = placed.offset + AlignUp(bytes, kAlign);
            partitions.system_volume = placed;
        } else std::cerr << "MakeHdd: 未提供 --system-volume，1 号分区留空，引导链没有系统卷可读\n";
        const uint64_t total = AlignUp(cursor, options.pad_to);

        // MBR：保留引导代码，分区表由本工具写实（四个槽位先清空，再写有内容的槽位）
        std::array<uint8_t, kMbrBytes> mbr{};
        {
            const std::vector<uint8_t> source = ReadFile(options.mbr);
            std::copy(source.begin(), source.end(), mbr.begin());
        }
        std::fill(mbr.begin() + 446, mbr.begin() + 446 + kEntryBytes, 0);
        std::vector<MbrPartition> written;
        if (partitions.esp.has_value()) {
            makeiso::WriteMbrEntry(std::span<uint8_t, kMbrBytes>(mbr.data(), mbr.size()), 0, MbrPartition{0xEF, partitions.esp->offset, partitions.esp->bytes});
            written.push_back(MbrPartition{0xEF, partitions.esp->offset, partitions.esp->bytes});
        }
        if (partitions.system_volume.has_value()) {
            makeiso::WriteMbrEntry(std::span<uint8_t, kMbrBytes>(mbr.data(), mbr.size()), 1, MbrPartition{0x83, partitions.system_volume->offset, partitions.system_volume->bytes});
            written.push_back(MbrPartition{0x83, partitions.system_volume->offset, partitions.system_volume->bytes});
        }
        image.Write(0, mbr);

        std::optional<std::array<uint8_t, 32>> descriptor;
        if (stub.has_value()) {
            BootDescriptor boot;
            boot.stub_offset = stub->offset;
            boot.stub_bytes = stub->bytes;
            descriptor = makeiso::EncodeBootDescriptor(boot, kMbrBytes);
            image.Write(BootDescriptor::kImageOffset, *descriptor);
        } else std::cerr << "MakeHdd: 未提供 --stub，不写 BootDescriptor，BIOS 路径无法装载 Stub\n";

        if (stub.has_value()) image.CopyFile(stub->offset, options.stub);
        if (core.has_value()) image.CopyFile(core->offset, options.core);
        for (const Placed& placed : raw) image.CopyFile(placed.offset, placed.name);
        if (partitions.esp.has_value()) image.CopyFile(partitions.esp->offset, options.esp);
        if (partitions.system_volume.has_value()) image.CopyFile(partitions.system_volume->offset, options.system_volume);
        image.ExtendTo(total);

        // 清单与摘要
        std::string manifest = "{\n";
        manifest += "  \"tool\": \"MakeHdd\",\n";
        manifest += "  \"total_bytes\": " + std::to_string(total) + ",\n";
        manifest += "  \"total_sectors_512\": " + std::to_string(total / kMbrBytes) + ",\n";
        manifest += "  \"mbr\": {\"offset\": 0, \"bytes\": " + std::to_string(kMbrBytes) + "},\n";
        if (descriptor.has_value()) {
            manifest += "  \"descriptor\": {\"offset\": " + std::to_string(BootDescriptor::kImageOffset) +
                        ", \"bytes\": " + std::to_string(BootDescriptor::kHeaderBytes) +
                        ", \"stub_offset\": " + std::to_string(stub->offset) +
                        ", \"stub_bytes\": " + std::to_string(stub->bytes) + "},\n";
        } else manifest += "  \"descriptor\": null,\n";
        manifest += "  \"stub\": " + JsonPlaced(stub.has_value() ? &*stub : nullptr) + ",\n";
        manifest += "  \"core\": " + JsonPlaced(core.has_value() ? &*core : nullptr) + ",\n";
        // 可选的载荷统一转成清单里的 JSON 片段
        const auto jsonPlaced = [](const std::optional<Placed>& placed) {
            return JsonPlaced(placed.has_value() ? &*placed : nullptr);
        };
        manifest += "  \"esp\": " + jsonPlaced(partitions.esp) + ",\n";
        manifest += "  \"system_volume\": " + jsonPlaced(partitions.system_volume) + ",\n";
        manifest += "  \"raw\": [";
        for (std::size_t i = 0; i < raw.size(); ++i) {
            manifest += (i == 0 ? "\n    " : ",\n    ");
            manifest += "{\"host_path\": \"" + JsonEscape(raw[i].name) +
                        "\", \"offset\": " + std::to_string(raw[i].offset) +
                        ", \"bytes\": " + std::to_string(raw[i].bytes) + "}";
        }
        manifest += raw.empty() ? "],\n" : "\n  ],\n";
        manifest += "  \"partitions\": [";
        for (std::size_t i = 0; i < written.size(); ++i) {
            const MbrPartition& part = written[i];
            manifest += (i == 0 ? "\n    " : ",\n    ");
            manifest += "{\"index\": " + std::to_string(i) + ", \"type\": " + std::to_string(part.type) +
                        ", \"start_lba_512\": " + std::to_string(part.offset / kMbrBytes) +
                        ", \"sectors_512\": " + std::to_string((part.bytes + kMbrBytes - 1) / kMbrBytes) + "}";
        }
        manifest += written.empty() ? "]\n" : "\n  ]\n";
        manifest += "}\n";

        if (!options.manifest.empty()) {
            std::ofstream out(options.manifest, std::ios::binary | std::ios::trunc);
            if (!out || !(out << manifest)) throw std::runtime_error("无法写清单：" + options.manifest);
        }

        std::cout << "MakeHdd：已生成 " << options.out << "（" << total << " 字节，" << total / kMbrBytes << " 个 512 字节扇区）\n";
        if (descriptor.has_value()) std::cout << "  BootDescriptor 偏移 " << BootDescriptor::kImageOffset << "，Stub 偏移 " << stub->offset << "，Stub 字节 " << stub->bytes << "\n";
        if (core.has_value()) std::cout << "  BaleenCore  偏移 " << core->offset << "，字节 " << core->bytes << "\n";
        if (partitions.esp.has_value()) std::cout << "  0 号分区    ESP（0xEF）偏移 " << partitions.esp->offset << "，字节 " << partitions.esp->bytes << "\n";
        if (partitions.system_volume.has_value()) std::cout << "  1 号分区    Ext4（0x83）偏移 " << partitions.system_volume->offset << "，字节 " << partitions.system_volume->bytes << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MakeHdd: " << error.what() << "\n";
        return 1;
    }
}
