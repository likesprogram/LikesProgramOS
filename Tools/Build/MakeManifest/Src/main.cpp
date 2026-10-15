/* main.cpp
    MakeManifest：生成内核集清单（LPMS），签名区留零，由 SignManifest 填写

    从内核镜像读取 BuildId（或显式给定），按 --artifact 逐条登记产物：类型、集内相对路径与来源文件；
    逐文件计算字节数与 SHA-256，按 Docs/Specs/内核集清单与签名.md 第二节的字节表组装
    清单生成后不得再改：签名工具只填签名区，其它字节变化都要重新生成并重签
*/

#include <HostIo.h>
#include <KernelImage.h>
#include <KernelImage.hpp>
#include <Manifest.hpp>
#include <Sha256.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace ML = KernelSet::ManifestLayout;

    // 产物条目：类型、集内相对路径与来源文件
    struct Artifact {
        uint8_t type = 0;      // 1 = .os，2 = .osm
        std::string path;      // 相对集目录的路径
        std::string file;      // 磁盘上的来源文件
    };

    // 打印用法
    void Usage() {
        std::cout <<
            "MakeManifest — 生成内核集清单（LPMS）\n"
            "\n"
            "用法：MakeManifest --out <清单路径> (--build-id <64 位十六进制> | --build-id-from <内核镜像>)\n"
            "                   --security-version <n> --artifact <type>:<相对路径>:<文件> [--artifact ...]\n"
            "\n"
            "  --out              输出的清单文件（本项目取 Manifest.mft）；签名区写零，须再由 SignManifest 签名\n"
            "  --build-id-from    从内核镜像数据的 BuildId 字段取集标识，与清单必须一致\n"
            "  --build-id         显式给出 32 字节 BuildId（小写十六进制，64 字符）\n"
            "  --security-version 单调安全版本号，发行侧给定，只增不减\n"
            "  --artifact         产物：type 1 = .os、2 = .osm；路径相对集目录；可多次\n"
            "\n"
            "示例：MakeManifest --out Out/Manifest.bin --build-id-from Out/LikesProgram.os \\\n"
            "                   --security-version 1 --artifact 1:LikesProgram.os:Out/LikesProgram.os\n";
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

    // 十六进制字符转值
    int HexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // 解析 64 字符的 BuildId 文本
    std::array<uint8_t, ML::kBuildIdBytes> ParseBuildId(const std::string& text) {
        if (text.size() != ML::kBuildIdBytes * 2) throw std::runtime_error("--build-id 需要 64 个十六进制字符");
        std::array<uint8_t, ML::kBuildIdBytes> buildId{};
        for (std::size_t i = 0; i < buildId.size(); ++i) {
            const int high = HexValue(text[2 * i]);
            const int low = HexValue(text[2 * i + 1]);
            if (high < 0 || low < 0) throw std::runtime_error("--build-id 含非十六进制字符");
            buildId[i] = static_cast<uint8_t>((high << 4) | low);
        }
        return buildId;
    }

    // BuildId 不得全零：全零表示没有集身份，清单规格与引导侧都拒绝
    void CheckBuildIdNotZero(const std::array<uint8_t, ML::kBuildIdBytes>& buildId, const std::string& source) {
        uint8_t any = 0;
        for (uint8_t byte : buildId) any |= byte;
        if (any == 0) throw std::runtime_error("BuildId 全零，禁止使用：" + source);
    }

    // 从内核镜像数据的 BuildId 字段读取集标识；全零拒绝
    std::array<uint8_t, ML::kBuildIdBytes> ReadBuildId(const std::string& file) {
        const std::vector<uint8_t> image = hostbuild::ReadFile(file);
        if (!hostbuild::IsKernelImage(image)) throw std::runtime_error("不是内核镜像：" + file);
        if (image.size() < Kernel::ImageDataLayout::kFieldBuildId + ML::kBuildIdBytes) throw std::runtime_error("内核镜像太短：" + file);
        std::array<uint8_t, ML::kBuildIdBytes> buildId{};
        std::memcpy(buildId.data(), image.data() + Kernel::ImageDataLayout::kFieldBuildId, buildId.size());
        uint8_t any = 0;
        for (uint8_t byte : buildId) any |= byte;
        if (any == 0) throw std::runtime_error("内核镜像的 BuildId 全零，先跑 PackImage：" + file);
        return buildId;
    }

    // 校验集内相对路径：非空、不以 / 开头、分量非空、不含 . 与 ..
    void CheckRelativePath(const std::string& path) {
        if (path.empty()) throw std::runtime_error("产物路径为空");
        if (path.size() > ML::kEntryPathMaxBytes) throw std::runtime_error("产物路径超过 255 字节：" + path);
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
        if (type != ML::kTypeOs && type != ML::kTypeOsm) throw std::runtime_error("产物类型只支持 1（.os）与 2（.osm）");
        Artifact artifact;
        artifact.type = static_cast<uint8_t>(type);
        artifact.path = value.substr(first + 1, last - first - 1);
        artifact.file = value.substr(last + 1);
        CheckRelativePath(artifact.path);
        if (artifact.file.empty()) throw std::runtime_error("产物来源文件为空：" + value);
        return artifact;
    }

    // 打印摘要的小写十六进制
    std::string HexText(const uint8_t* data, std::size_t bytes) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(bytes * 2);
        for (std::size_t i = 0; i < bytes; ++i) {
            text.push_back(kDigits[data[i] >> 4]);
            text.push_back(kDigits[data[i] & 0x0f]);
        }
        return text;
    }

    // 组装清单：头部、条目区与留零的签名区
    std::vector<uint8_t> BuildManifest(const std::array<uint8_t, ML::kBuildIdBytes>& buildId, uint64_t securityVersion, const std::vector<Artifact>& artifacts) {
        std::size_t bytes = ML::kHeaderBytes + ML::kSignatureAreaBytes;
        for (const Artifact& artifact : artifacts) bytes += ML::kEntryFixedBytes + artifact.path.size();
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
        for (const Artifact& artifact : artifacts) {
            const std::vector<uint8_t> content = hostbuild::ReadFile(artifact.file);
            // 零字节产物在引导侧与签名侧都被拒绝，生成侧不放出这种清单
            if (content.empty()) throw std::runtime_error("产物文件是空的：" + artifact.file);
            hostbuild::Sha256 hash;
            hash.Begin();
            hash.Update(content);
            const std::array<uint8_t, hostbuild::kSha256Bytes> digest = hash.Finish();
            manifest[offset + ML::kEntryType] = artifact.type;
            Put16(manifest, offset + ML::kEntryPathBytes, static_cast<uint16_t>(artifact.path.size()));
            manifest[offset + ML::kEntryReserved] = 0;
            Put64(manifest, offset + ML::kEntryDataBytes, content.size());
            std::memcpy(manifest.data() + offset + ML::kEntryDigest, digest.data(), digest.size());
            std::memcpy(manifest.data() + offset + ML::kEntryPath, artifact.path.data(), artifact.path.size());
            std::cout << "MakeManifest：条目 type=" << static_cast<unsigned>(artifact.type) << " path=" << artifact.path
                      << " " << content.size() << " 字节 sha256=" << HexText(digest.data(), digest.size()) << "\n";
            offset += ML::kEntryFixedBytes + artifact.path.size();
        }
        return manifest;
    }
}

// 入口：解析参数、组装清单并写文件
int main(int argc, char** argv) {
    try {
        std::string outPath;
        std::string buildIdText;
        std::string buildIdFrom;
        uint64_t securityVersion = 0;
        bool hasSecurityVersion = false;
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
            } else if (arg == "--out") outPath = next("--out");
            else if (arg == "--build-id") buildIdText = next("--build-id");
            else if (arg == "--build-id-from") buildIdFrom = next("--build-id-from");
            else if (arg == "--security-version") {
                securityVersion = hostbuild::ParseNumber(next("--security-version"), "安全版本号");
                hasSecurityVersion = true;
            } else if (arg == "--artifact") artifacts.push_back(ParseArtifact(next("--artifact")));
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        if (outPath.empty()) {
            Usage();
            return 2;
        }
        if (buildIdText.empty() == buildIdFrom.empty()) throw std::runtime_error("--build-id 与 --build-id-from 必须给出且只给一个");
        if (!hasSecurityVersion) throw std::runtime_error("缺少 --security-version");
        if (artifacts.empty()) throw std::runtime_error("至少需要一个 --artifact");
        if (artifacts.size() > ML::kEntryMaxCount) throw std::runtime_error("产物条目数超过上限 32");
        unsigned osCount = 0;
        for (std::size_t i = 0; i < artifacts.size(); ++i) {
            if (artifacts[i].type == ML::kTypeOs) ++osCount;
            for (std::size_t j = i + 1; j < artifacts.size(); ++j)
                if (artifacts[i].path == artifacts[j].path) throw std::runtime_error("产物路径重复：" + artifacts[i].path);
        }
        if (osCount != 1) throw std::runtime_error("必须恰好有一条 .os 产物");

        const std::array<uint8_t, ML::kBuildIdBytes> buildId = !buildIdFrom.empty() ? ReadBuildId(buildIdFrom) : ParseBuildId(buildIdText);
        // 与 --build-id-from 的读取同一条门禁：全零 BuildId 不得进清单
        if (buildIdFrom.empty()) CheckBuildIdNotZero(buildId, "--build-id");
        const std::vector<uint8_t> manifest = BuildManifest(buildId, securityVersion, artifacts);
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入 " + outPath);
        out.write(reinterpret_cast<const char*>(manifest.data()), static_cast<std::streamsize>(manifest.size()));
        out.close();
        if (!out) throw std::runtime_error("写入失败 " + outPath);
        std::cout << "MakeManifest：已生成 " << outPath << "（" << manifest.size() << " 字节，BuildId "
                  << HexText(buildId.data(), buildId.size()) << "，SecurityVersion " << securityVersion
                  << "，条目 " << artifacts.size() << "）\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MakeManifest: " << error.what() << "\n";
        return 1;
    }
}
