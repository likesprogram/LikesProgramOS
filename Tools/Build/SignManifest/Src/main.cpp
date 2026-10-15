/* main.cpp
    SignManifest：用 Ed25519 私钥填写内核集清单的签名区

    签名覆盖按 Docs/Specs/内核集清单与签名.md 2.1：签名值 64 字节视作零，对整份清单
    计算 SHA-256 与 Ed25519 签名；算法与密钥编号在覆盖范围内，签名后清单字节不得再改
    私钥是 32 字节种子文件；开发期密钥由 MakeKeys 生成在 .cache/Keys，发布期密钥离线保管
*/

#include <Ed25519.h>
#include <HostIo.h>
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

    // 打印用法
    void Usage() {
        std::cout <<
            "SignManifest — 填写内核集清单的 Ed25519 签名\n"
            "\n"
            "用法：SignManifest --in <清单路径> --key <种子文件> [--key-id <n>] [--out <输出路径>]\n"
            "\n"
            "  --in      已生成的清单文件（签名区留零）；未给 --out 时就地写回\n"
            "  --key     32 字节 Ed25519 种子；私钥不进仓库、不进镜像\n"
            "  --key-id  密钥编号，取值 1..255，默认 1\n"
            "  --out     输出路径；给出时不动输入文件\n";
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

    // 摘要的小写十六进制
    std::string HexText(const uint8_t* bytes, std::size_t count) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            text.push_back(kDigits[bytes[i] >> 4]);
            text.push_back(kDigits[bytes[i] & 0x0f]);
        }
        return text;
    }

    // 条目路径的语义校验：与 MakeManifest 的生成侧、引导侧的读取侧同一条规则
    void CheckEntryPath(const std::vector<uint8_t>& manifest, std::size_t offset, uint16_t bytes) {
        const uint8_t* path = manifest.data() + offset + ML::kEntryPath;
        if (path[0] == '/') throw std::runtime_error("清单条目路径不得以 / 开头");
        std::size_t begin = 0;
        for (std::size_t i = 0; i <= bytes; ++i) {
            if (i < bytes && path[i] != '/') continue;
            const std::size_t component = i - begin;
            if (component == 0) throw std::runtime_error("清单条目路径含空分量");
            if (component == 1 && path[begin] == '.') throw std::runtime_error("清单条目路径含 . 分量");
            if (component == 2 && path[begin] == '.' && path[begin + 1] == '.') throw std::runtime_error("清单条目路径含 .. 分量");
            begin = i + 1;
        }
    }

    // 校验清单结构与保留字段；返回签名区起点
    std::size_t CheckManifest(const std::vector<uint8_t>& manifest) {
        if (manifest.size() < ML::kHeaderBytes + ML::kSignatureAreaBytes) throw std::runtime_error("清单短于头部与签名区");
        if (Get32(manifest, ML::kFieldMagic) != ML::kMagic) throw std::runtime_error("清单格式标记不符");
        if (Get16(manifest, ML::kFieldVersion) != ML::kVersion) throw std::runtime_error("清单版本不是 1");
        if (Get16(manifest, ML::kFieldHeaderBytes) != ML::kHeaderBytes) throw std::runtime_error("清单头部长度不是 0x40");
        if (Get32(manifest, ML::kFieldBytes) != manifest.size()) throw std::runtime_error("清单长度字段与文件长度不符");
        if (Get32(manifest, ML::kFieldFlags) != 0) throw std::runtime_error("清单标志字段非零");
        if (Get16(manifest, ML::kFieldReserved16) != 0 || Get32(manifest, ML::kFieldReserved32) != 0) throw std::runtime_error("清单头部保留字段非零");
        uint8_t buildIdAny = 0;
        for (std::size_t i = 0; i < ML::kBuildIdBytes; ++i) buildIdAny |= manifest[ML::kFieldBuildId + i];
        if (buildIdAny == 0) throw std::runtime_error("清单的 BuildId 全零");
        const uint16_t count = Get16(manifest, ML::kFieldArtifactCount);
        if (count < 1 || count > ML::kEntryMaxCount) throw std::runtime_error("清单条目数超出 1..32");
        const std::size_t signatureBegin = manifest.size() - ML::kSignatureAreaBytes;
        std::size_t offset = ML::kFieldArtifacts;
        unsigned osCount = 0;
        for (uint16_t i = 0; i < count; ++i) {
            if (offset + ML::kEntryFixedBytes > signatureBegin) throw std::runtime_error("清单条目区越界");
            const uint8_t type = manifest[offset + ML::kEntryType];
            if (type != ML::kTypeOs && type != ML::kTypeOsm) throw std::runtime_error("清单条目类型非法");
            if (type == ML::kTypeOs) ++osCount;
            const uint16_t pathBytes = Get16(manifest, offset + ML::kEntryPathBytes);
            if (pathBytes == 0 || pathBytes > ML::kEntryPathMaxBytes) throw std::runtime_error("清单条目路径长度非法");
            if (manifest[offset + ML::kEntryReserved] != 0) throw std::runtime_error("清单条目保留字节非零");
            CheckEntryPath(manifest, offset, pathBytes);
            if (Get64(manifest, offset + ML::kEntryDataBytes) == 0) throw std::runtime_error("清单条目字节数为零");
            offset += ML::kEntryFixedBytes + pathBytes;
        }
        if (offset != signatureBegin) throw std::runtime_error("清单条目区长度与签名区起点不符");
        // 与引导侧一致：内核集恰好一条 .os，多一条就是引导侧必拒的清单，不给它签名
        if (osCount != 1) throw std::runtime_error("清单必须恰好有一条 .os 条目");
        return signatureBegin;
    }
}

// 入口：校验清单、计算摘要与签名、写回
int main(int argc, char** argv) {
    try {
        std::string inPath;
        std::string keyPath;
        std::string outPath;
        uint64_t keyId = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&](const char* what) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("选项 ") + what + " 缺少参数");
                return argv[++i];
            };
            if (arg == "-h" || arg == "--help") {
                Usage();
                return 0;
            } else if (arg == "--in") inPath = next("--in");
            else if (arg == "--key") keyPath = next("--key");
            else if (arg == "--out") outPath = next("--out");
            else if (arg == "--key-id") keyId = hostbuild::ParseNumber(next("--key-id"), "密钥编号（--key-id）");
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        if (inPath.empty() || keyPath.empty()) {
            Usage();
            return 2;
        }
        if (keyId < 1 || keyId > 255) throw std::runtime_error("--key-id 必须在 1..255 内");

        std::vector<uint8_t> manifest = hostbuild::ReadFile(inPath);
        const std::size_t signatureBegin = CheckManifest(manifest);

        // 签名区：先写算法与密钥编号，签名值留零参与覆盖计算
        std::vector<uint8_t> signedManifest = manifest;
        signedManifest[signatureBegin + ML::kSignatureAlgorithm] = ML::kAlgorithmEd25519;
        signedManifest[signatureBegin + ML::kSignatureKeyId] = static_cast<uint8_t>(keyId);
        signedManifest[signatureBegin + ML::kSignatureReserved] = 0;
        signedManifest[signatureBegin + ML::kSignatureReserved + 1] = 0;
        std::memset(signedManifest.data() + signatureBegin + ML::kSignatureValue, 0, ML::kSignatureValueBytes);

        // 私钥：32 字节种子 → 64 字节展开
        const std::vector<uint8_t> seed = hostbuild::ReadFile(keyPath);
        if (seed.size() != hostbuild::kEd25519SeedBytes) throw std::runtime_error("种子文件长度不是 32 字节：" + keyPath);
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
        hostbuild::Ed25519DeriveKey(seed, publicKey, privateKey);

        // 摘要与签名：覆盖整份清单，签名值 64 字节按零代入
        hostbuild::Sha256 hash;
        hash.Begin();
        hash.Update(signedManifest);
        const std::array<uint8_t, hostbuild::kSha256Bytes> digest = hash.Finish();

        std::array<uint8_t, hostbuild::kEd25519SignatureBytes> signature{};
        hostbuild::Ed25519Sign(privateKey, signedManifest, signature);
        // 自检：签名必须能被公钥验证，防止写出不可验的清单
        if (!hostbuild::Ed25519Verify(publicKey, signedManifest, signature)) throw std::runtime_error("签名自检失败");
        std::memcpy(signedManifest.data() + signatureBegin + ML::kSignatureValue, signature.data(), signature.size());

        const std::string target = outPath.empty() ? inPath : outPath;
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入 " + target);
        out.write(reinterpret_cast<const char*>(signedManifest.data()), static_cast<std::streamsize>(signedManifest.size()));
        out.close();
        if (!out) throw std::runtime_error("写入失败 " + target);

        std::cout << "SignManifest：已签名 " << target << "（" << signedManifest.size() << " 字节，KeyId " << keyId << "）\n"
                  << "  ManifestDigest " << HexText(digest.data(), digest.size()) << "\n"
                  << "  Signature " << HexText(signature.data(), signature.size()) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SignManifest: " << error.what() << "\n";
        return 1;
    }
}
