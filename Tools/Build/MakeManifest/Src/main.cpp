/* main.cpp
    MakeManifest：生成内核集清单（LPMS）与模块加载列表，或按清单与实际产物重算核对

    生成：读 BuildId 与 Digest 都未分配的原型 .os，按发布线身份规则算出 BuildId（只由 .os
    内容与路径决定，模块补丁不改变它），写进最终 .os 的 BuildId 字段与按新 BuildId 复算的
    Digest；生成 Modules.list（模块主版本 + 修订号 + 该修订全部 .osm 条目），再以最终字节填清单；
    签名区留零，由 SignManifest 填写
    核对：--verify 用同一套派生函数从清单与实际产物重算 BuildId，并逐项比对条目与 .os 内字段
    清单生成后不得再改：签名工具只填签名区，其它字节变化都要重新生成并重签
*/

#include <HostIo.h>
#include <KernelImage.h>
#include <KernelImage.hpp>
#include <Manifest.hpp>
#include <Sha256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
    namespace ML = KernelSet::ManifestLayout;

    // BuildId 派生规则的域分离标记：8 字节 ASCII，含算法版本（02：发布线身份，不含模块摘要）
    constexpr char kBuildIdDomain[8] = { 'L', 'P', 'K', 'S', 'I', 'D', '0', '2' };
    // 模块加载列表在集内的固定相对路径：与 Manifest.mft 同级
    constexpr char kBootListPath[] = "Modules.list";
    // 模块加载列表的容量上限：16 KiB（含固定头与条目）
    constexpr std::size_t kBootListMaxBytes = 16 * 1024;
    // 列表条目上限：清单条目上限 32，其中恰好一条是 .os、一条是 Modules.list
    constexpr std::size_t kBootListMaxEntries = 30;
    // .osm 条目数上限：清单条目上限 32，其中恰好一条是 .os、一条是 Modules.list
    constexpr std::size_t kOsmMaxCount = ML::kEntryMaxCount - 2;

    // 命令行给出的产物：类型、集内相对路径与来源文件
    struct Artifact {
        uint8_t type = 0;   // 1 = .os，2 = .osm，3 = 模块加载列表
        std::string path;   // 相对集目录的路径
        std::string file;   // 磁盘上的来源文件
    };

    // 已核对的产物：条目字段与整份文件的内容摘要
    struct Resolved {
        uint8_t type = 0;                                      // 1 = .os，2 = .osm，3 = Modules.list
        std::string path;                                      // 相对集目录的路径
        uint64_t bytes = 0;                                    // 文件字节数
        std::array<uint8_t, hostbuild::kSha256Bytes> digest{}; // 整份文件的 SHA-256
    };

    // 清单里的产物条目：类型、相对路径、字节数与内容摘要
    struct Entry {
        uint8_t type = 0;                                      // 1 = .os，2 = .osm，3 = Modules.list
        std::string path;                                      // 相对集目录的路径
        uint64_t bytes = 0;                                    // 清单登记的字节数
        std::array<uint8_t, hostbuild::kSha256Bytes> digest{}; // 清单登记的整份文件摘要
    };

    // 解析出的清单：集身份、安全版本与产物条目
    struct ParsedManifest {
        std::array<uint8_t, hostbuild::kBuildIdBytes> buildId{}; // 清单里的构建级 BuildId
        uint64_t securityVersion = 0;                            // 单调安全版本
        std::vector<Entry> entries;                              // 产物条目，顺序与清单一致
    };

    // 打印用法
    void Usage() {
        std::cout <<
            "MakeManifest — 生成内核集清单（LPMS）与 Modules.list，或按清单与产物重算核对\n"
            "\n"
            "生成：MakeManifest --out <清单> --kernel-in <原型 .os> --kernel-out <最终 .os>\n"
            "                   --kernel-path <集内相对路径> --security-version <n>\n"
            "                   --module-revision <n> --module-list <Modules.list 输出路径>\n"
            "                   [--artifact 2:<相对路径>:<文件> ...]\n"
            "                   [--build-id-out <BuildId 文本>]\n"
            "核对：MakeManifest --verify --in <清单> --artifact <1|2|3:<路径>:<文件> ...\n"
            "\n"
            "  --out             输出的清单文件（本项目取 Manifest.mft）；签名区写零，须再由 SignManifest 签名\n"
            "  --kernel-in       未分配身份的 .os 原型：BuildId 与 Digest 必须全零，结构合法且 ModuleMajor 非零\n"
            "  --kernel-out      写出最终 .os 的路径：BuildId 与 Digest 在这里写入，不得与 --kernel-in 相同\n"
            "  --kernel-path     最终 .os 在集内的相对路径，清单第一条条目的路径\n"
            "  --security-version 单调安全版本号：模块修订每次发布只增不减，防回滚比较的依据\n"
            "  --module-revision 模块修订号（u64，非零，随补丁单调递增），写进 Modules.list\n"
            "  --module-list     生成的 Modules.list 输出路径；清单以 type=3 条目登记它\n"
            "  --artifact        生成模式只登记 type=2 的 .osm，可多次；顺序即 Modules.list 条目顺序；\n"
            "                    核对模式给 1/2/3 类型。列表由全部 .osm 自动生成，条目与清单 Type=2 条目一一相等\n"
            "  --build-id-out    把 64 位小写十六进制的 BuildId 写进这份文本文件，供构建链命名集目录\n"
            "  --verify          按清单与实际产物重算 BuildId 并核对条目，不写任何文件\n"
            "\n"
            "BuildId 是 .os 发布线身份：域标记 + .os 相对路径 + 内容摘要 C_os，模块补丁不改变它。\n"
            "模块主版本在 .os 镜像数据里，Modules.list 的 major 必须与它相等；跨主版本不承诺兼容。\n"
            ""
            "Modules.list 是内核侧书写的完整模块加载列表：Baleen 只预装其中命中引导闭包期望集的成员\n"
            "生成与核对共用同一份实现，签名区内容不参与核对\n"
            "\n"
            "示例：MakeManifest --out Out/KernelSetStage/Manifest.mft \\\n"
            "                   --kernel-in Out/Bin/LikesProgram.os --kernel-out Out/KernelSetStage/LikesProgram.os \\\n"
            "                   --kernel-path LikesProgram.os --security-version 1 \\\n"
            "                   --module-revision 1 --module-list Out/KernelSetStage/Modules.list \\\n"
            "                   --build-id-out Out/BuildId\n";
    }

    // 小端读取 16 位
    uint16_t Get16(const std::vector<uint8_t>& data, std::size_t offset) {
        return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
    }

    // 小端读取 32 位
    uint32_t Get32(const std::vector<uint8_t>& data, std::size_t offset) {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[offset + i]) << (8 * i);
        return value;
    }

    // 小端读取 64 位
    uint64_t Get64(const std::vector<uint8_t>& data, std::size_t offset) {
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[offset + i]) << (8 * i);
        return value;
    }

    // 小端写入 16 位
    void Put16(std::vector<uint8_t>& out, std::size_t offset, uint16_t value) {
        out[offset] = static_cast<uint8_t>(value);
        out[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    // 小端写入 32 位
    void Put32(std::vector<uint8_t>& out, std::size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // 小端写入 64 位
    void Put64(std::vector<uint8_t>& out, std::size_t offset, uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // 一段字节的小写十六进制文本
    std::string HexText(const uint8_t* data, std::size_t bytes) {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(bytes * 2);
        for (std::size_t i = 0; i < bytes; ++i) {
            text.push_back(kDigits[data[i] >> 4]);
            text.push_back(kDigits[data[i] & 0x0F]);
        }
        return text;
    }

    // 把一段字节整份写到文件，失败即报错
    void WriteAll(const std::string& path, const void* data, std::size_t bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入 " + path);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        out.close();
        if (!out) throw std::runtime_error("写入失败 " + path);
    }

    // 整份写出一组字节
    void WriteFile(const std::string& path, std::span<const uint8_t> data) {
        WriteAll(path, data.data(), data.size());
    }

    // 整份写出一份文本
    void WriteTextFile(const std::string& path, const std::string& text) {
        WriteAll(path, text.data(), text.size());
    }

    // 校验集内相对路径：非空、不以 / 开头、分量非空、不含 . 与 ..、不含 NUL（定义见下方）
    void CheckRelativePath(const std::string& path);

    // 生成 Modules.list：固定三行 + 模块条目（module <相对路径>），ASCII、LF。
    // 模块主版本来自 .os 镜像数据，修订号来自构建参数；条目顺序即内核侧装载偏好顺序，
    // 条目集合必须等于该修订全部 .osm 产物；空列表是合法形态（没有 .osm 的集）
    std::string BuildBootList(uint16_t moduleMajor, uint64_t moduleRevision, const std::vector<std::string>& listedModules) {
        std::string text = "LPML 1\nmajor " + std::to_string(moduleMajor) + "\nrevision " + std::to_string(moduleRevision) + "\n";
        for (const std::string& path : listedModules) text += "module " + path + "\n";
        if (text.size() > kBootListMaxBytes) throw std::runtime_error("Modules.list 超过 16 KiB 上限");
        return text;
    }

    // 解析 Modules.list：固定三行加 0..30 条 module 条目，返回主版本、修订号与模块路径。
    // 列表内容整体受清单摘要与签名覆盖，这里只做结构与集合核对。
    void ParseBootList(const std::vector<uint8_t>& content, uint16_t& moduleMajor, uint64_t& moduleRevision,
        std::vector<std::string>& listedModules) {
        if (content.size() > kBootListMaxBytes) throw std::runtime_error("Modules.list 超过 16 KiB 上限");
        const std::string text(content.begin(), content.end());
        if (text.rfind("LPML 1\n", 0) != 0) throw std::runtime_error("Modules.list 的格式标记不受支持");
        std::size_t pos = 7;
        auto nextLine = [&text, &pos]() {
            const std::size_t end = text.find('\n', pos);
            if (end == std::string::npos) throw std::runtime_error("Modules.list 缺少结尾换行");
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            return line;
        };
        const std::string majorLine = nextLine();
        const std::string revisionLine = nextLine();
        if (majorLine.rfind("major ", 0) != 0 || revisionLine.rfind("revision ", 0) != 0) {
            throw std::runtime_error("Modules.list 的字段顺序不受支持");
        }
        const uint64_t major = hostbuild::ParseNumber(majorLine.substr(6), "Modules.list 主版本");
        moduleRevision = hostbuild::ParseNumber(revisionLine.substr(9), "Modules.list 修订号");
        if (major == 0 || major > 0xFFFF) throw std::runtime_error("Modules.list 的模块主版本不在 1..65535 内");
        if (moduleRevision == 0) throw std::runtime_error("Modules.list 的模块修订号为零");
        moduleMajor = static_cast<uint16_t>(major);
        listedModules.clear();
        while (pos < text.size()) {
            const std::string line = nextLine();
            if (line.rfind("module ", 0) != 0) throw std::runtime_error("Modules.list 含未定义的额外行：" + line);
            const std::string path = line.substr(7);
            CheckRelativePath(path);
            if (path.size() <= 8 || path.rfind("Modules/", 0) != 0) {
                throw std::runtime_error("Modules.list 的 module 条目必须在 Modules/ 下：" + path);
            }
            for (const std::string& earlier : listedModules) {
                if (earlier == path) throw std::runtime_error("Modules.list 的 module 条目重复：" + path);
            }
            listedModules.push_back(path);
            if (listedModules.size() > kBootListMaxEntries) throw std::runtime_error("Modules.list 的 module 条目超过 30 条上限");
        }
    }

    // 把路径规范化成“去掉重复斜杠、单点分量与可抵消的双点分量”的形态，用于比较两条路径
    std::string NormalizePath(const std::string& path) {
        std::vector<std::string> parts;
        std::size_t begin = 0;
        while (begin <= path.size()) {
            const std::size_t end = path.find('/', begin);
            const std::string part = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!part.empty() && part != ".") {
                if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
                else parts.push_back(part);
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        std::string normalized = (!path.empty() && path.front() == '/') ? "/" : "";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0) normalized += '/';
            normalized += parts[i];
        }
        return normalized;
    }

    // 生成器的输入与输出不得是同一路径：避免“生成器改输入”的歧义
    void CheckDistinctKernelPaths(const std::string& kernelIn, const std::string& kernelOut) {
        if (NormalizePath(kernelIn) == NormalizePath(kernelOut)) throw std::runtime_error("--kernel-in 与 --kernel-out 必须是不同路径：" + kernelIn);
    }

    // 校验集内相对路径：非空、不以 / 开头、分量非空、不含 . 与 ..、不含 NUL
    void CheckRelativePath(const std::string& path) {
        if (path.empty()) throw std::runtime_error("产物路径为空");
        if (path.size() > ML::kEntryPathMaxBytes) throw std::runtime_error("产物路径超过 255 字节：" + path);
        if (path.find('\0') != std::string::npos) throw std::runtime_error("产物路径含 NUL 字节");
        if (path.front() == '/') throw std::runtime_error("产物路径不得以 / 开头：" + path);
        std::size_t begin = 0;
        while (begin <= path.size()) {
            const std::size_t end = path.find('/', begin);
            const std::string component = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (component.empty()) throw std::runtime_error("产物路径含空分量：" + path);
            if (component == "." || component == "..") throw std::runtime_error("产物路径含 . 或 ..：" + path);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }

    // 解析 --artifact 的 <type>:<相对路径>:<文件>
    Artifact ParseArtifact(const std::string& value) {
        const std::size_t first = value.find(':');
        const std::size_t last = value.rfind(':');
        if (first == std::string::npos || first == last) throw std::runtime_error("--artifact 需要 <type>:<相对路径>:<文件>：" + value);
        const uint64_t type = hostbuild::ParseNumber(value.substr(0, first), "产物类型");
        if (type != ML::kTypeOs && type != ML::kTypeOsm && type != ML::kTypeBootList) {
            throw std::runtime_error("产物类型只支持 1（.os）、2（.osm）与 3（Modules.list）");
        }
        Artifact artifact;
        artifact.type = static_cast<uint8_t>(type);
        artifact.path = value.substr(first + 1, last - first - 1);
        artifact.file = value.substr(last + 1);
        CheckRelativePath(artifact.path);
        if (artifact.file.empty()) throw std::runtime_error("产物来源文件为空：" + value);
        return artifact;
    }

    // 派生发布线 BuildId：域标记 + .os 相对路径 + 内容摘要 C_os。
    // 模块补丁不参与派生——同一 .os 发布线下的模块修订共用同一个 BuildId 与集目录；
    // 模块主版本已经包含在 .os 镜像数据与 C_os 里。
    std::array<uint8_t, hostbuild::kBuildIdBytes> DeriveBuildId(const std::string& osPath,
        std::span<const uint8_t> osContentId) {
        static_assert(hostbuild::kBuildIdBytes == hostbuild::kSha256Bytes, "BuildId 是 SHA-256 的 32 字节输出");
        hostbuild::Sha256 sha;
        sha.Begin();
        sha.Update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(kBuildIdDomain), sizeof(kBuildIdDomain)));
        const auto length = static_cast<uint16_t>(osPath.size());
        const uint8_t prefix[2] = { static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8) };
        sha.Update(std::span<const uint8_t>(prefix, sizeof(prefix)));
        sha.Update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(osPath.data()), osPath.size()));
        sha.Update(osContentId);
        return sha.Finish();
    }

    // 组装清单：头部、条目区与留零的签名区
    std::vector<uint8_t> BuildManifest(const std::array<uint8_t, hostbuild::kBuildIdBytes>& buildId,
        uint64_t securityVersion, const std::vector<Resolved>& artifacts) {
        std::size_t bytes = ML::kHeaderBytes + ML::kSignatureAreaBytes;
        for (const Resolved& artifact : artifacts) bytes += ML::kEntryFixedBytes + artifact.path.size();
        if (bytes > ML::kManifestMaxBytes) throw std::runtime_error("清单超过 0x10000 字节");
        std::vector<uint8_t> manifest(bytes, 0);
        Put32(manifest, ML::kFieldMagic, ML::kMagic);
        Put16(manifest, ML::kFieldVersion, ML::kVersion);
        Put16(manifest, ML::kFieldHeaderBytes, ML::kHeaderBytes);
        Put32(manifest, ML::kFieldBytes, static_cast<uint32_t>(bytes));
        Put32(manifest, ML::kFieldFlags, 0);
        std::memcpy(manifest.data() + ML::kFieldBuildId, buildId.data(), buildId.size());
        Put64(manifest, ML::kFieldSecurityVersion, securityVersion);
        Put16(manifest, ML::kFieldArtifactCount, static_cast<uint16_t>(artifacts.size()));
        Put16(manifest, ML::kFieldReserved16, 0);
        Put32(manifest, ML::kFieldReserved32, 0);

        std::size_t offset = ML::kFieldArtifacts;
        for (const Resolved& artifact : artifacts) {
            manifest[offset + ML::kEntryType] = artifact.type;
            Put16(manifest, offset + ML::kEntryPathBytes, static_cast<uint16_t>(artifact.path.size()));
            manifest[offset + ML::kEntryReserved] = 0;
            Put64(manifest, offset + ML::kEntryDataBytes, artifact.bytes);
            std::memcpy(manifest.data() + offset + ML::kEntryDigest, artifact.digest.data(), artifact.digest.size());
            std::memcpy(manifest.data() + offset + ML::kEntryPath, artifact.path.data(), artifact.path.size());
            offset += ML::kEntryFixedBytes + artifact.path.size();
        }
        return manifest;
    }

    // 解析并校验清单结构：头部、条目区与保留字段；签名区内容不参与核对，由 SignManifest 负责
    ParsedManifest ParseManifest(const std::vector<uint8_t>& manifest) {
        if (manifest.size() < ML::kHeaderBytes + ML::kSignatureAreaBytes) throw std::runtime_error("清单短于头部加签名区");
        if (manifest.size() > ML::kManifestMaxBytes) throw std::runtime_error("清单超过 0x10000 字节");
        if (Get32(manifest, ML::kFieldMagic) != ML::kMagic) throw std::runtime_error("清单的格式标记不匹配");
        if (Get16(manifest, ML::kFieldVersion) != ML::kVersion || Get16(manifest, ML::kFieldHeaderBytes) != ML::kHeaderBytes) {
            throw std::runtime_error("清单的版本或头部长度不受支持");
        }
        if (Get32(manifest, ML::kFieldBytes) != manifest.size()) throw std::runtime_error("清单的 Bytes 与实际文件长度不一致");
        if (Get32(manifest, ML::kFieldFlags) != 0) throw std::runtime_error("清单的 Flags 不受支持");
        if (Get16(manifest, ML::kFieldReserved16) != 0 || Get32(manifest, ML::kFieldReserved32) != 0) {
            throw std::runtime_error("清单头部的保留字段不是全零");
        }
        ParsedManifest parsed;
        std::memcpy(parsed.buildId.data(), manifest.data() + ML::kFieldBuildId, parsed.buildId.size());
        if (std::all_of(parsed.buildId.begin(), parsed.buildId.end(), [](uint8_t byte) { return byte == 0; })) {
            throw std::runtime_error("清单的 BuildId 全零");
        }
        parsed.securityVersion = Get64(manifest, ML::kFieldSecurityVersion);
        const std::size_t count = Get16(manifest, ML::kFieldArtifactCount);
        if (count < 1 || count > ML::kEntryMaxCount) throw std::runtime_error("清单的产物条目数不在 1..32 内");

        const std::size_t end = manifest.size() - ML::kSignatureAreaBytes;
        std::size_t offset = ML::kFieldArtifacts;
        unsigned osCount = 0;
        unsigned bootListCount = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (offset > end || end - offset < ML::kEntryFixedBytes) throw std::runtime_error("清单的条目区放不下声明的条目数");
            const std::size_t pathBytes = Get16(manifest, offset + ML::kEntryPathBytes);
            if (pathBytes < 1 || pathBytes > ML::kEntryPathMaxBytes) throw std::runtime_error("清单条目的路径长度不在 1..255 内");
            if (end - offset < ML::kEntryFixedBytes + pathBytes) throw std::runtime_error("清单条目的路径越出条目区");
            Entry entry;
            entry.type = manifest[offset + ML::kEntryType];
            if (entry.type != ML::kTypeOs && entry.type != ML::kTypeOsm && entry.type != ML::kTypeBootList) throw std::runtime_error("清单条目的产物类型不受支持");
            if (manifest[offset + ML::kEntryReserved] != 0) throw std::runtime_error("清单条目的保留字节不是全零");
            entry.bytes = Get64(manifest, offset + ML::kEntryDataBytes);
            if (entry.bytes == 0) throw std::runtime_error("清单条目的 DataBytes 为零");
            std::memcpy(entry.digest.data(), manifest.data() + offset + ML::kEntryDigest, entry.digest.size());
            entry.path.assign(reinterpret_cast<const char*>(manifest.data() + offset + ML::kEntryPath), pathBytes);
            CheckRelativePath(entry.path);
            for (const Entry& earlier : parsed.entries) {
                if (earlier.path == entry.path) throw std::runtime_error("清单条目的路径重复：" + entry.path);
            }
            if (entry.type == ML::kTypeOs) ++osCount;
            if (entry.type == ML::kTypeBootList) {
                ++bootListCount;
                if (entry.path != kBootListPath) throw std::runtime_error(std::string("清单的模块加载列表路径必须是 ") + kBootListPath);
            }
            parsed.entries.push_back(std::move(entry));
            offset += ML::kEntryFixedBytes + pathBytes;
        }
        if (offset != end) throw std::runtime_error("清单的条目区长度与总长不符");
        if (osCount != 1) throw std::runtime_error("清单必须恰好有一条 .os 产物");
        if (bootListCount != 1) throw std::runtime_error("清单必须恰好有一条 Modules.list 产物（type=3）");
        return parsed;
    }

    // 打印一条产物条目：类型、路径、字节数与摘要
    void PrintEntry(const Resolved& artifact) {
        std::cout << "MakeManifest：条目 type=" << static_cast<unsigned>(artifact.type) << " path=" << artifact.path
                  << " " << artifact.bytes << " 字节 sha256=" << HexText(artifact.digest.data(), artifact.digest.size()) << "\n";
    }

    // 生成：派生发布线 BuildId、写最终 .os、生成 Modules.list、生成清单与 BuildId 文本
    void Generate(const std::string& outPath, const std::string& kernelIn, const std::string& kernelOut,
        const std::string& kernelPath, const std::string& buildIdOut, uint64_t securityVersion,
        uint64_t moduleRevision, const std::string& moduleListPath, const std::vector<Artifact>& osms) {
        CheckRelativePath(kernelPath);
        CheckDistinctKernelPaths(kernelIn, kernelOut);
        if (moduleRevision == 0) throw std::runtime_error("--module-revision 必须非零");
        if (moduleListPath.empty()) throw std::runtime_error("--module-list 不能为空");
        for (const Artifact& osm : osms) {
            if (osm.path == kernelPath) throw std::runtime_error("清单条目的路径重复：" + kernelPath);
            if (osm.path == kBootListPath) throw std::runtime_error(std::string("产物路径不得占用 ") + kBootListPath + "：" + osm.path);
            // 模块文件固定在集目录的 Modules/ 下，文件名带版本，平铺存放（不用版本目录）
            if (osm.path.rfind("Modules/", 0) != 0 || osm.path.size() <= 8) {
                throw std::runtime_error("模块产物的相对路径必须在 Modules/ 下：" + osm.path);
            }
        }
        // 列表条目 = 该修订全部 .osm 产物的相对路径，顺序即命令行顺序（内核侧的装载偏好顺序）；
        // 列表是内核侧书写的完整模块加载列表，不再由 Baleen 的预装集合决定内容。
        // 闭包期望集只由 BootClosure.def 定义，Baleen 与内核各自从列表里筛，不在这里裁剪
        if (osms.size() > kBootListMaxEntries) throw std::runtime_error("Modules.list 条目超过 30 条上限");
        std::vector<std::string> listedModules;
        listedModules.reserve(osms.size());
        for (const Artifact& osm : osms) listedModules.push_back(osm.path);

        // 原型：起点是内核镜像数据，结构合法、身份两段全零、模块主版本非零
        const std::vector<uint8_t> prototype = hostbuild::ReadFile(kernelIn);
        if (!hostbuild::IsKernelImage(prototype)) throw std::runtime_error("--kernel-in 不是内核镜像（起点缺少 LPOSKRN 数据）：" + kernelIn);
        if (hostbuild::VerifyKernelImage(prototype) != hostbuild::KernelIdentityState::Prototype) {
            throw std::runtime_error("--kernel-in 必须是未分配身份的 .os 原型（BuildId 与 Digest 全零），已带身份的 .os 不能再生成：" + kernelIn);
        }
        const uint16_t moduleMajor = hostbuild::KernelModuleMajor(prototype);

        // 模块加载列表：与 Manifest.mft 同级，内容受清单摘要覆盖；主版本必须等于 .os 发布线
        WriteTextFile(moduleListPath, BuildBootList(moduleMajor, moduleRevision, listedModules));
        const std::vector<uint8_t> bootListBytes = hostbuild::ReadFile(moduleListPath);
        hostbuild::Sha256 bootListSha;
        bootListSha.Begin();
        bootListSha.Update(bootListBytes);
        std::vector<Resolved> resolved;
        resolved.push_back({ ML::kTypeOs, kernelPath, 0, {} });
        // 各 .osm 只登记实际字节：容器格式由 PackModule 生成，清单侧不解释内容
        for (const Artifact& osm : osms) {
            const std::vector<uint8_t> content = hostbuild::ReadFile(osm.file);
            if (content.empty()) throw std::runtime_error("产物文件是空的：" + osm.file);
            hostbuild::Sha256 sha;
            sha.Begin();
            sha.Update(content);
            resolved.push_back({ ML::kTypeOsm, osm.path, content.size(), sha.Finish() });
        }
        resolved.push_back({ ML::kTypeBootList, kBootListPath, bootListBytes.size(), bootListSha.Finish() });

        // 派生 BuildId：发布线身份只由 .os 内容与路径决定，模块补丁不改变它
        const std::array<uint8_t, hostbuild::kSha256Bytes> osContentId = hostbuild::ComputeKernelContentId(prototype);
        const std::array<uint8_t, hostbuild::kBuildIdBytes> buildId = DeriveBuildId(kernelPath, osContentId);
        if (std::all_of(buildId.begin(), buildId.end(), [](uint8_t byte) { return byte == 0; })) {
            throw std::runtime_error("派生的 BuildId 全零，清单格式禁止");
        }

        // 写最终 .os：BuildId 与按新 BuildId 复算的 Digest；随后回读以最终字节填 .os 条目
        std::vector<uint8_t> finalImage = prototype;
        hostbuild::AssignKernelIdentity(finalImage, buildId);
        WriteFile(kernelOut, finalImage);
        const std::vector<uint8_t> written = hostbuild::ReadFile(kernelOut);
        if (written != finalImage) throw std::runtime_error("写出的最终 .os 与内存内容不一致：" + kernelOut);
        if (hostbuild::VerifyKernelImage(written, buildId) != hostbuild::KernelIdentityState::Assigned) {
            throw std::runtime_error("最终 .os 的身份复算不成立：" + kernelOut);
        }
        hostbuild::Sha256 kernelSha;
        kernelSha.Begin();
        kernelSha.Update(written);
        resolved[0].bytes = written.size();
        resolved[0].digest = kernelSha.Finish();

        for (const Resolved& artifact : resolved) PrintEntry(artifact);
        const std::vector<uint8_t> manifest = BuildManifest(buildId, securityVersion, resolved);
        WriteFile(outPath, manifest);
        const std::string buildIdText = hostbuild::BuildIdText(buildId) + "\n";
        if (!buildIdOut.empty()) WriteTextFile(buildIdOut, buildIdText);
        std::cout << "MakeManifest：已生成 " << outPath << "（" << manifest.size() << " 字节，BuildId "
                  << hostbuild::BuildIdText(buildId) << "，SecurityVersion " << securityVersion
                  << "，ModuleMajor " << moduleMajor << "，ModuleRevision " << moduleRevision
                  << "，条目 " << resolved.size() << "）\n";
        std::cout << "MakeManifest：已写入最终内核镜像 " << kernelOut << "（" << written.size() << " 字节）\n";
        std::cout << "MakeManifest：已写入模块加载列表 " << moduleListPath << "（" << bootListBytes.size() << " 字节）\n";
        if (!buildIdOut.empty()) std::cout << "MakeManifest：BuildId 文本已写入 " << buildIdOut << "\n";
    }

    // 核对：按同一套派生函数从清单与实际产物重算 BuildId，并逐项比对条目、.os 内字段与 Modules.list
    void Verify(const std::string& inPath, const std::vector<Artifact>& artifacts) {
        const ParsedManifest manifest = ParseManifest(hostbuild::ReadFile(inPath));
        if (artifacts.size() != manifest.entries.size()) {
            throw std::runtime_error("--artifact 条目数（" + std::to_string(artifacts.size()) + "）与清单（"
                + std::to_string(manifest.entries.size()) + "）不一致");
        }
        std::map<std::string, Artifact> provided;
        unsigned osCount = 0;
        unsigned bootListCount = 0;
        for (const Artifact& artifact : artifacts) {
            if (!provided.emplace(artifact.path, artifact).second) throw std::runtime_error("--artifact 路径重复：" + artifact.path);
            if (artifact.type == ML::kTypeOs) ++osCount;
            if (artifact.type == ML::kTypeBootList) {
                ++bootListCount;
                if (artifact.path != kBootListPath) throw std::runtime_error(std::string("--artifact 的 Modules.list 路径必须是 ") + kBootListPath);
            }
        }
        if (osCount != 1) throw std::runtime_error("--artifact 必须恰好给出一条 type=1 的 .os");
        if (bootListCount != 1) throw std::runtime_error("--artifact 必须恰好给出一条 type=3 的 Modules.list");

        std::string osPath;
        std::vector<uint8_t> kernelImage;
        std::vector<uint8_t> bootListBytes;
        for (const Entry& entry : manifest.entries) {
            const auto it = provided.find(entry.path);
            if (it == provided.end()) throw std::runtime_error("清单条目没有对应的 --artifact：" + entry.path);
            if (it->second.type != entry.type) throw std::runtime_error("清单条目与 --artifact 的产物类型不一致：" + entry.path);
            const std::vector<uint8_t> content = hostbuild::ReadFile(it->second.file);
            if (content.empty()) throw std::runtime_error("产物文件是空的：" + it->second.file);
            hostbuild::Sha256 sha;
            sha.Begin();
            sha.Update(content);
            const std::array<uint8_t, hostbuild::kSha256Bytes> digest = sha.Finish();
            if (content.size() != entry.bytes) throw std::runtime_error("产物字节数与清单不一致：" + it->second.file);
            if (digest != entry.digest) throw std::runtime_error("产物摘要与清单不一致：" + it->second.file);
            if (entry.type == ML::kTypeOs) {
                osPath = entry.path;
                kernelImage = content;
            } else if (entry.type == ML::kTypeBootList) {
                bootListBytes = content;
            }
        }
        if (kernelImage.empty()) throw std::runtime_error("清单缺少 .os 产物");

        // Modules.list：固定格式、模块主版本必须等于 .os 发布线；内容整体已受清单摘要覆盖
        uint16_t listMajor = 0;
        uint64_t listRevision = 0;
        std::vector<std::string> listedModules;
        ParseBootList(bootListBytes, listMajor, listRevision, listedModules);
        const uint16_t kernelMajor = hostbuild::KernelModuleMajor(kernelImage);
        if (listMajor != kernelMajor) {
            throw std::runtime_error("Modules.list 的模块主版本（" + std::to_string(listMajor) + "）与 .os（"
                + std::to_string(kernelMajor) + "）不一致");
        }
        // 列表条目集合必须等于清单 Type=2 条目集合：列表是内核侧书写的完整模块加载列表，
        // 漏登会让模块装不上、多登会指向清单没有的产物，两者都必须拒绝。
        // 列表已保证路径唯一，因此数量相等 + 每条都命中 .osm 条目即为一对一相等
        std::size_t osmEntryCount = 0;
        for (const Entry& entry : manifest.entries) if (entry.type == ML::kTypeOsm) ++osmEntryCount;
        if (listedModules.size() != osmEntryCount) {
            throw std::runtime_error("Modules.list 条目数（" + std::to_string(listedModules.size())
                + "）与清单 .osm 条目数（" + std::to_string(osmEntryCount) + "）不一致");
        }
        for (const std::string& path : listedModules) {
            bool found = false;
            for (const Entry& entry : manifest.entries) {
                if (entry.type == ML::kTypeOsm && entry.path == path) { found = true; break; }
            }
            if (!found) throw std::runtime_error("Modules.list 的 module 条目没有对应的清单 .osm 条目：" + path);
        }

        // 重算 BuildId：只由 .os 内容与路径派生，模块补丁不参与
        const std::array<uint8_t, hostbuild::kSha256Bytes> osContentId = hostbuild::ComputeKernelContentId(kernelImage);
        const std::array<uint8_t, hostbuild::kBuildIdBytes> derived = DeriveBuildId(osPath, osContentId);
        if (!std::equal(derived.begin(), derived.end(), manifest.buildId.begin())) {
            throw std::runtime_error("清单 BuildId 与从实际产物重算的 BuildId 不一致：清单 " + hostbuild::BuildIdText(manifest.buildId)
                + "，重算 " + hostbuild::BuildIdText(derived));
        }
        // .os 自身的身份：BuildId 必须等于清单 BuildId，Digest 必须与整幅内容一致
        if (hostbuild::VerifyKernelImage(kernelImage, manifest.buildId) != hostbuild::KernelIdentityState::Assigned) {
            throw std::runtime_error("清单的 .os 产物不是已分配身份的内核镜像：" + osPath);
        }
        std::cout << "MakeManifest：清单核对通过 " << inPath << "（" << manifest.entries.size() << " 条产物，BuildId "
                  << hostbuild::BuildIdText(manifest.buildId) << "，SecurityVersion " << manifest.securityVersion
                  << "，ModuleMajor " << kernelMajor << "，ModuleRevision " << listRevision << "）\n";
    }
}

// 入口：解析参数，按 --verify 选择生成或核对
int main(int argc, char** argv) {
    try {
        bool verify = false;
        std::string outPath;
        std::string inPath;
        std::string kernelIn;
        std::string kernelOut;
        std::string kernelPath;
        std::string buildIdOut;
        std::string moduleListPath;
        uint64_t securityVersion = 0;
        uint64_t moduleRevision = 0;
        bool hasSecurityVersion = false;
        bool hasModuleRevision = false;
        std::vector<Artifact> artifacts;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&](const char* what) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("选项 ") + what + " 缺少参数");
                return argv[++i];
            };
            if (arg == "-h" || arg == "--help") {
                Usage();
                return 0;
            } else if (arg == "--verify") verify = true;
            else if (arg == "--out") outPath = next("--out");
            else if (arg == "--in") inPath = next("--in");
            else if (arg == "--kernel-in") kernelIn = next("--kernel-in");
            else if (arg == "--kernel-out") kernelOut = next("--kernel-out");
            else if (arg == "--kernel-path") kernelPath = next("--kernel-path");
            else if (arg == "--build-id-out") buildIdOut = next("--build-id-out");
            else if (arg == "--module-list") moduleListPath = next("--module-list");
            else if (arg == "--module-revision") {
                moduleRevision = hostbuild::ParseNumber(next("--module-revision"), "模块修订号");
                hasModuleRevision = true;
            }
            else if (arg == "--security-version") {
                securityVersion = hostbuild::ParseNumber(next("--security-version"), "安全版本号");
                hasSecurityVersion = true;
            } else if (arg == "--artifact") artifacts.push_back(ParseArtifact(next("--artifact")));
            else throw std::runtime_error("无法识别的参数：" + arg);
        }

        if (verify) {
            if (inPath.empty()) throw std::runtime_error("核对模式需要 --in <清单>");
            if (!outPath.empty() || !kernelIn.empty() || !kernelOut.empty() || !kernelPath.empty()
                || !buildIdOut.empty() || hasSecurityVersion || !moduleListPath.empty() || hasModuleRevision) {
                throw std::runtime_error("核对模式不接受生成参数（--out / --kernel-in / --kernel-out / --kernel-path / --build-id-out / --security-version / --module-list / --module-revision）");
            }
            if (artifacts.empty()) throw std::runtime_error("核对模式至少需要一条 --artifact");
            Verify(inPath, artifacts);
            return 0;
        }

        if (outPath.empty()) {
            Usage();
            return 2;
        }
        if (kernelIn.empty() || kernelOut.empty() || kernelPath.empty()) {
            throw std::runtime_error("生成模式需要 --kernel-in、--kernel-out 与 --kernel-path");
        }
        if (!hasSecurityVersion) throw std::runtime_error("生成模式缺少 --security-version");
        if (!hasModuleRevision) throw std::runtime_error("生成模式缺少 --module-revision");
        if (moduleListPath.empty()) throw std::runtime_error("生成模式缺少 --module-list");
        if (artifacts.size() > kOsmMaxCount) throw std::runtime_error("模块条目数超过上限（.os 与 Modules.list 各占一条）");
        for (const Artifact& artifact : artifacts) {
            if (artifact.type != ML::kTypeOsm) throw std::runtime_error("生成模式的 --artifact 只登记 .osm；.os 由 --kernel-in 与 --kernel-out 给出，Modules.list 由 --module-list 生成");
        }
        Generate(outPath, kernelIn, kernelOut, kernelPath, buildIdOut, securityVersion, moduleRevision, moduleListPath, artifacts);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MakeManifest: " << error.what() << "\n";
        return 1;
    }
}
