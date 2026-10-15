/* main.cpp
    MakeRevocation：生成并签名内核集签名吊销记录（LPRV）

    字节表与签名规则见 Docs/Specs/内核集清单与签名.md 第五节：签名值 64 字节视作零，
    对整份记录签名；同一组输入产出同样的字节，签名之后记录的任何字节变化都必须重新生成
    签名者不得出现在本记录的吊销位图里——自吊销的记录在引导侧一律被拒绝，这里提前拦下
    结构检查用 Packages/Common 的目标侧实现，工具与引导侧因此是同一份规则而不是两份
*/

#include <Ed25519.h>
#include <HostIo.h>
#include <Revocation.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace RV = Baleen::Core::Revocation;

    // 打印用法
    void Usage() {
        std::cout <<
            "MakeRevocation — 生成并签名吊销记录（LPRV）\n"
            "\n"
            "用法：MakeRevocation --key <种子文件> [--key-id <n>] --epoch <n>\n"
            "                     [--min-security-version <n>] [--revoke-key-id <n>]... [--out <路径>]\n"
            "\n"
            "  --key                 32 字节 Ed25519 种子；私钥不进仓库、不进镜像\n"
            "  --key-id              签名密钥编号，取值 1..255，默认 1\n"
            "  --epoch               吊销纪元，取值 1..2^64-1；只增不减，回滚旧纪元不解除吊销\n"
            "  --min-security-version 记录给出的最低允许清单安全版本，默认 0\n"
            "  --revoke-key-id       吊销一个签名密钥编号，取值 1..255，可重复；不得与签名者相同\n"
            "  --out                 输出路径，默认 Revocation.rvk\n"
            "\n"
            "落点约定：系统卷内 /LikesProgram/System/Revocation.rvk；文件缺失表示没有吊销记录\n";
    }

    // 小端写入 16 位
    void Put16(std::vector<uint8_t>& bytes, std::size_t offset, uint16_t value) {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    // 小端写入 32 位
    void Put32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // 小端写入 64 位
    void Put64(std::vector<uint8_t>& bytes, std::size_t offset, uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // 位图里置位的 KeyId 升序列成文本，供结论行使用
    std::string MaskText(const uint8_t* mask) {
        std::string text;
        for (unsigned keyId = 1; keyId <= 255; ++keyId) {
            if (!RV::IsKeyRevoked(mask, static_cast<uint8_t>(keyId))) continue;
            if (!text.empty()) text += ",";
            text += std::to_string(keyId);
        }
        return text.empty() ? "none" : text;
    }

    // 十六进制文本
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
}

// 入口：构造记录、结构自检、签名自检、落盘
int main(int argc, char** argv) {
    try {
        std::string keyPath;
        std::string outPath = "Revocation.rvk";
        uint64_t keyId = 1;
        uint64_t epoch = 0;
        uint64_t minSecurityVersion = 0;
        std::vector<uint64_t> revoked;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&](const char* what) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("选项 ") + what + " 缺少参数");
                return argv[++i];
            };
            if (arg == "-h" || arg == "--help") {
                Usage();
                return 0;
            } else if (arg == "--key") keyPath = next("--key");
            else if (arg == "--out") outPath = next("--out");
            else if (arg == "--key-id") keyId = hostbuild::ParseNumber(next("--key-id"), "签名密钥编号（--key-id）");
            else if (arg == "--epoch") epoch = hostbuild::ParseNumber(next("--epoch"), "吊销纪元（--epoch）");
            else if (arg == "--min-security-version") minSecurityVersion = hostbuild::ParseNumber(next("--min-security-version"), "最低允许安全版本（--min-security-version）");
            else if (arg == "--revoke-key-id") revoked.push_back(hostbuild::ParseNumber(next("--revoke-key-id"), "吊销密钥编号（--revoke-key-id）"));
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        if (keyPath.empty()) {
            Usage();
            return 2;
        }
        if (keyId < 1 || keyId > 255) throw std::runtime_error("--key-id 必须在 1..255 内");
        // 纪元为零的记录不表达任何吊销策略，引导侧按结构不符拒绝：工具不产出这种记录
        if (epoch < 1) throw std::runtime_error("--epoch 必须非零");
        for (std::size_t i = 0; i < revoked.size(); ++i) {
            if (revoked[i] < 1 || revoked[i] > 255) throw std::runtime_error("--revoke-key-id 必须在 1..255 内");
            if (revoked[i] == keyId) throw std::runtime_error("签名者不能吊销自己：该记录在引导侧一律被拒绝");
            for (std::size_t j = 0; j < i; ++j) {
                if (revoked[j] == revoked[i]) throw std::runtime_error("--revoke-key-id 重复：" + std::to_string(revoked[i]));
            }
        }

        // 记录：头部、位图与签名区字段；签名值保持零参与签名计算
        std::vector<uint8_t> record(RV::kRecordBytes, 0);
        Put32(record, RV::kFieldMagic, RV::kMagic);
        Put16(record, RV::kFieldVersion, RV::kVersion);
        Put16(record, RV::kFieldHeaderBytes, RV::kHeaderBytes);
        Put32(record, RV::kFieldBytes, RV::kRecordBytes);
        Put32(record, RV::kFieldFlags, 0);
        Put64(record, RV::kFieldEpoch, epoch);
        Put64(record, RV::kFieldMinimumSecurityVersion, minSecurityVersion);
        for (const uint64_t id : revoked) {
            const uint64_t bit = id - 1;
            record[RV::kFieldRevokedKeyMask + bit / 8] = static_cast<uint8_t>(record[RV::kFieldRevokedKeyMask + bit / 8] | (1u << (bit % 8)));
        }
        const std::size_t signatureArea = RV::kHeaderBytes;
        record[signatureArea + RV::kSignatureAlgorithm] = RV::kAlgorithmEd25519;
        record[signatureArea + RV::kSignatureKeyId] = static_cast<uint8_t>(keyId);

        // 结构自检：与引导侧同一份实现，签名者编号必须回读一致
        uint8_t signerKeyId = 0;
        if (!RV::CheckRecord(record.data(), static_cast<uint32_t>(record.size()), signerKeyId) || signerKeyId != static_cast<uint8_t>(keyId))
            throw std::runtime_error("生成的记录未通过共用层的结构检查");

        // 私钥：32 字节种子 → 64 字节展开
        const std::vector<uint8_t> seed = hostbuild::ReadFile(keyPath);
        if (seed.size() != hostbuild::kEd25519SeedBytes) throw std::runtime_error("种子文件长度不是 32 字节：" + keyPath);
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
        hostbuild::Ed25519DeriveKey(seed, publicKey, privateKey);

        std::array<uint8_t, hostbuild::kEd25519SignatureBytes> signature{};
        hostbuild::Ed25519Sign(privateKey, record, signature);
        // 自检：签名必须能被公钥验证，防止写出不可验的记录
        if (!hostbuild::Ed25519Verify(publicKey, record, signature)) throw std::runtime_error("签名自检失败");
        std::memcpy(record.data() + signatureArea + RV::kSignatureValue, signature.data(), signature.size());

        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入 " + outPath);
        out.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
        out.close();
        if (!out) throw std::runtime_error("写入失败 " + outPath);

        std::cout << "MakeRevocation：已生成 " << outPath << "（" << record.size() << " 字节）\n"
                  << "  Epoch " << epoch << "  MinSecurityVersion " << minSecurityVersion
                  << "  RevokedKeyIds " << MaskText(record.data() + RV::kFieldRevokedKeyMask) << "\n"
                  << "  Signer KeyId " << keyId << "  Signature " << HexText(signature.data(), signature.size()) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MakeRevocation: " << error.what() << "\n";
        return 1;
    }
}
