/* main.cpp
    SignManifest：用 Ed25519 私钥填写内核集清单的签名区，并可按信任锚表回验已签名的清单

    签名覆盖：签名值 64 字节视作零，对整份清单
    计算 SHA-256 与 Ed25519 签名；算法与密钥编号在覆盖范围内，签名后清单字节不得再改
    私钥是 32 字节种子文件；开发期密钥由 MakeKeys 生成在 .cache/Keys，发布期密钥离线保管
    --anchors 读 MakeKeys --anchor-text 产出的文本锚表：签名时拒绝用锚表外的密钥签，
    --verify 时要求清单签名区的 KeyId 命中锚表、并用锚表里的公钥验签
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
#include <string_view>
#include <vector>

namespace {
    namespace ML = KernelSet::ManifestLayout;

    // 打印用法
    void Usage() {
        std::cout <<
            "SignManifest — 填写内核集清单的 Ed25519 签名，或按锚表回验清单\n"
            "\n"
            "签名：SignManifest --in <清单路径> --key <种子文件> [--key-id <n>] [--out <输出路径>] [--anchors <锚表>]\n"
            "验签：SignManifest --verify --in <清单路径> [--key <种子文件>] [--key-id <n>] [--anchors <锚表>]\n"
            "\n"
            "  --in      清单文件（签名模式下签名区应留零；验签模式下是已签名的清单）\n"
            "  --key     32 字节 Ed25519 种子；私钥不进仓库、不进镜像\n"
            "  --key-id  密钥编号，取值 1..255，签名时默认 1；验签时给出则要求与签名区一致\n"
            "  --out     输出路径；给出时不动输入文件\n"
            "  --anchors 文本锚表（MakeKeys --anchor-text）：每行 \"<KeyId> <公钥 64 位十六进制>\"；\n"
            "            签名时拒绝锚表外的密钥，验签时按清单签名区的 KeyId 取锚表公钥\n"
            "  --verify  只验签：核对清单结构、签名区字段、KeyId 命中锚表与 Ed25519 签名\n";
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

    // 信任锚条目：编号与 Ed25519 公钥，与 MakeKeys --anchor-text 的文本锚表同构
    struct AnchorEntry {
        uint8_t keyId;                                                    // 密钥编号
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey;  // Ed25519 公钥
    };

    // 一个十六进制字符的取值；不是十六进制返回 -1
    int HexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // 去掉行两端的空白：空格、制表符与回车
    std::string Trim(std::string_view text) {
        std::size_t begin = 0, end = text.size();
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
        return std::string(text.substr(begin, end - begin));
    }

    // 读文本锚表：每行 "<KeyId> <公钥 64 位十六进制>"，'#' 开头与空行忽略；格式不符或编号重复抛异常
    std::vector<AnchorEntry> ReadAnchorText(const std::string& path) {
        static constexpr std::size_t kPublicHexChars = hostbuild::kEd25519PublicKeyBytes * 2;
        std::ifstream input(path);
        if (!input) throw std::runtime_error("无法读取锚表 " + path);
        std::vector<AnchorEntry> entries;
        std::string line;
        std::size_t number = 0;
        while (std::getline(input, line)) {
            ++number;
            const std::string where = path + ":" + std::to_string(number);
            const std::string text = Trim(line);
            if (text.empty() || text[0] == '#') continue;
            const std::size_t at = text.find_first_of(" \t");
            if (at == std::string::npos) throw std::runtime_error(where + " 缺少公钥字段");
            const uint64_t keyId = hostbuild::ParseNumber(text.substr(0, at), where + " 的 KeyId");
            if (keyId < 1 || keyId > 255) throw std::runtime_error(where + " 的 KeyId 必须在 1..255 内");
            const std::string keyText = Trim(text.substr(at + 1));
            if (keyText.size() != kPublicHexChars) throw std::runtime_error(where + " 的公钥必须是 " + std::to_string(kPublicHexChars) + " 位十六进制");
            AnchorEntry entry{ static_cast<uint8_t>(keyId), {} };
            for (std::size_t i = 0; i < hostbuild::kEd25519PublicKeyBytes; ++i) {
                const int high = HexValue(keyText[i * 2]), low = HexValue(keyText[i * 2 + 1]);
                if (high < 0 || low < 0) throw std::runtime_error(where + " 的公钥含非十六进制字符");
                entry.publicKey[i] = static_cast<uint8_t>((high << 4) | low);
            }
            for (const auto& existing : entries) {
                if (existing.keyId == entry.keyId) throw std::runtime_error(where + " 的 KeyId 在锚表里重复");
            }
            entries.push_back(entry);
        }
        return entries;
    }

    // 按 KeyId 查锚；找不到返回空指针
    const AnchorEntry* FindAnchor(const std::vector<AnchorEntry>& anchors, uint8_t keyId) {
        for (const auto& entry : anchors) {
            if (entry.keyId == keyId) return &entry;
        }
        return nullptr;
    }

    // 清单签名区的固定字段核对：算法与保留字节；返回签名区的 KeyId
    uint8_t CheckSignatureFields(const std::vector<uint8_t>& manifest, std::size_t signatureBegin) {
        if (manifest[signatureBegin + ML::kSignatureAlgorithm] != ML::kAlgorithmEd25519) throw std::runtime_error("清单签名算法不是 Ed25519");
        if (manifest[signatureBegin + ML::kSignatureReserved] != 0 || manifest[signatureBegin + ML::kSignatureReserved + 1] != 0) throw std::runtime_error("清单签名区保留字节非零");
        return manifest[signatureBegin + ML::kSignatureKeyId];
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
            if (type != ML::kTypeOs && type != ML::kTypeOsm && type != ML::kTypeBootList) throw std::runtime_error("清单条目类型非法");
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

    // 验签：结构巡检之后核对签名区、信任锚命中与 Ed25519 签名
    // keyPath 给出时用种子派生公钥（给了锚表还要与锚表逐字节比对），anchors 给出时以锚表公钥为准
    void VerifyManifest(const std::vector<uint8_t>& manifest, const std::string& keyPath, const std::string& anchorsPath, bool hasKeyId, uint64_t wantedKeyId) {
        const std::size_t signatureBegin = CheckManifest(manifest);
        const uint8_t keyId = CheckSignatureFields(manifest, signatureBegin);
        std::vector<AnchorEntry> anchors;
        if (!anchorsPath.empty()) anchors = ReadAnchorText(anchorsPath);
        const AnchorEntry* anchored = anchors.empty() ? nullptr : FindAnchor(anchors, keyId);
        if (!anchors.empty() && anchored == nullptr) throw std::runtime_error("清单签名区的 KeyId " + std::to_string(keyId) + " 不在锚表里");
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
        if (!keyPath.empty()) {
            const std::vector<uint8_t> seed = hostbuild::ReadFile(keyPath);
            if (seed.size() != hostbuild::kEd25519SeedBytes) throw std::runtime_error("种子文件长度不是 32 字节：" + keyPath);
            std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
            hostbuild::Ed25519DeriveKey(seed, publicKey, privateKey);
        }
        if (anchored != nullptr) {
            if (!keyPath.empty() && !(anchored->publicKey == publicKey)) throw std::runtime_error("密钥文件与锚表里 KeyId " + std::to_string(keyId) + " 的公钥不一致");
            publicKey = anchored->publicKey;
        } else if (keyPath.empty()) {
            throw std::runtime_error("--verify 需要 --key 或 --anchors 给出信任来源");
        }
        if (hasKeyId && keyId != static_cast<uint8_t>(wantedKeyId)) throw std::runtime_error("清单签名区的 KeyId 与 --key-id 不一致");
        // 验签：签名值 64 字节按零代入，与签名侧覆盖同一段字节
        std::vector<uint8_t> signedManifest = manifest;
        std::memset(signedManifest.data() + signatureBegin + ML::kSignatureValue, 0, ML::kSignatureValueBytes);
        std::array<uint8_t, hostbuild::kEd25519SignatureBytes> signature{};
        std::memcpy(signature.data(), manifest.data() + signatureBegin + ML::kSignatureValue, signature.size());
        if (!hostbuild::Ed25519Verify(publicKey, signedManifest, signature)) throw std::runtime_error("清单签名验签失败");
        std::cout << "SignManifest：签名有效 " << (anchors.empty() ? "（公钥来自密钥文件）" : "（公钥来自锚表）")
                  << " KeyId " << static_cast<unsigned>(keyId) << "\n";
    }
}

// 入口：验签模式或签名模式（校验清单、计算摘要与签名、写回）
int main(int argc, char** argv) {
    try {
        std::string inPath;
        std::string keyPath;
        std::string outPath;
        std::string anchorsPath;
        bool verify = false;
        bool hasKeyId = false;
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
            else if (arg == "--anchors") anchorsPath = next("--anchors");
            else if (arg == "--verify") verify = true;
            else if (arg == "--key-id") {
                keyId = hostbuild::ParseNumber(next("--key-id"), "密钥编号（--key-id）");
                hasKeyId = true;
            } else throw std::runtime_error("无法识别的参数：" + arg);
        }
        if (keyId < 1 || keyId > 255) throw std::runtime_error("--key-id 必须在 1..255 内");

        // 验签模式：只读清单与信任来源，不写任何文件
        if (verify) {
            if (inPath.empty()) {
                Usage();
                return 2;
            }
            if (!outPath.empty()) throw std::runtime_error("--verify 不接受 --out：验签不写文件");
            if (keyPath.empty() && anchorsPath.empty()) throw std::runtime_error("--verify 至少需要 --key 或 --anchors 之一");
            const std::vector<uint8_t> manifest = hostbuild::ReadFile(inPath);
            VerifyManifest(manifest, keyPath, anchorsPath, hasKeyId, keyId);
            return 0;
        }

        if (inPath.empty() || keyPath.empty()) {
            Usage();
            return 2;
        }

        std::vector<uint8_t> manifest = hostbuild::ReadFile(inPath);
        const std::size_t signatureBegin = CheckManifest(manifest);

        // 私钥：32 字节种子 → 64 字节展开
        const std::vector<uint8_t> seed = hostbuild::ReadFile(keyPath);
        if (seed.size() != hostbuild::kEd25519SeedBytes) throw std::runtime_error("种子文件长度不是 32 字节：" + keyPath);
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
        hostbuild::Ed25519DeriveKey(seed, publicKey, privateKey);

        // 锚表给定时先核对签名密钥确实在表里：表外的密钥不能签这份清单
        if (!anchorsPath.empty()) {
            const std::vector<AnchorEntry> anchors = ReadAnchorText(anchorsPath);
            const AnchorEntry* anchored = FindAnchor(anchors, static_cast<uint8_t>(keyId));
            if (anchored == nullptr) throw std::runtime_error("锚表里没有 KeyId " + std::to_string(keyId) + "：清单不能由不受信任的密钥签名");
            if (!(anchored->publicKey == publicKey)) throw std::runtime_error("密钥文件的公钥与锚表里 KeyId " + std::to_string(keyId) + " 的公钥不一致");
        }

        // 签名区：先写算法与密钥编号，签名值留零参与覆盖计算
        std::vector<uint8_t> signedManifest = manifest;
        signedManifest[signatureBegin + ML::kSignatureAlgorithm] = ML::kAlgorithmEd25519;
        signedManifest[signatureBegin + ML::kSignatureKeyId] = static_cast<uint8_t>(keyId);
        signedManifest[signatureBegin + ML::kSignatureReserved] = 0;
        signedManifest[signatureBegin + ML::kSignatureReserved + 1] = 0;
        std::memset(signedManifest.data() + signatureBegin + ML::kSignatureValue, 0, ML::kSignatureValueBytes);

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
