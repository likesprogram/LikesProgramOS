/* main.cpp
    CheckRevocation：签名吊销记录与引导状态区单调纪元的宿主侧检查（C++20）

    检查的是将要交付的那些实现：Revocation.cpp 与 BootState.cpp 取自 Packages/Common，
    ManifestVerify.cpp 与 TrustAnchor.cpp 取自 BaleenCore；锚表、介质与文件系统由本工具自带
    用例：
      1. 锚表与固定种子的派生结果逐字节一致（保护夹具自身）
      2. 记录结构：正例与十类结构不符的负例
      3. 签名：正例、换错公钥、改动一个字节、签名者自吊销
      4. 位图与并集：按 KeyId 定位与只增不减
      5. 系统卷读取：缺文件、长度不符、篡改、合法
      6. 真实清单验证：被吊销的签名密钥拒绝（E-CORE-74）、记录下限抬升拒绝（E-CORE-66）
      7. 单调性：记录文件缺失不解除持久事实、回滚成旧纪元同样不解除
      8. 引导状态记录：版本 2 往返一致、版本 1 仍可读且撤销字段按全零
    手工执行，不进入默认构建；结论行不写“全部通过”以外的措辞
*/

#include <BootState.hpp>
#include <Ed25519.h>
#include <HostIo.h>
#include <KernelImage.hpp>
#include <Manifest.hpp>
#include <ManifestVerify.hpp>
#include <Revocation.hpp>
#include <Sha256.h>
#include <Storage/BlockDevice.hpp>
#include <Storage/FileSystem.hpp>
#include <TrustAnchor.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Baleen {
    namespace Crypto {
        // 本工具自带的信任锚表与条目数：定义在文件末尾，供目标侧 FindTrustAnchor 与用例引用
        extern const TrustAnchor kTrustAnchors[];
        extern const uint32_t kTrustAnchorCount;
    }
}

namespace {
    namespace ML = KernelSet::ManifestLayout;
    namespace RV = Baleen::Core::Revocation;
    using Bytes = std::vector<uint8_t>;

    // —— 固定种子与由它派生的公钥：公钥与内嵌锚表逐字节核对，见 CaseAnchorDerivation ——
    constexpr uint8_t kSignerSeed[32] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f };
    constexpr uint8_t kOtherSeed[32] = { 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f };
    constexpr uint8_t kSignerPublicKey[32] = { 0x77, 0x76, 0xe8, 0x70, 0xb9, 0x33, 0x54, 0xf2, 0xa0, 0xb2, 0x4c, 0x23, 0xf2, 0xa3, 0x6c, 0xc4, 0xe8, 0x0e, 0x22, 0x32, 0x18, 0xc1, 0xb9, 0x79, 0x26, 0xfd, 0xd0, 0x18, 0x39, 0x6a, 0x2b, 0x9b };
    constexpr uint8_t kOtherPublicKey[32] = { 0xcd, 0x14, 0xb3, 0x7f, 0x95, 0x6e, 0x95, 0x31, 0x94, 0xff, 0x7f, 0xb7, 0x3b, 0x3d, 0x81, 0xdc, 0xc5, 0x61, 0xd6, 0x1a, 0x75, 0x38, 0x09, 0x4b, 0x7c, 0x3e, 0x1a, 0x64, 0x3e, 0xe5, 0xf3, 0xaa };
    // 签名密钥编号与另一把受信密钥的编号
    constexpr uint8_t kSignerKeyId = 1;
    constexpr uint8_t kOtherKeyId = 3;
    // 引导状态区域与集目录的夹具取值
    constexpr uint64_t kStateOffset = 0x21000;
    constexpr uint32_t kStateBytes = 4096;
    constexpr uint64_t kMediumBytes = 0x24000;
    // 吊销记录在系统卷里的固定落点
    constexpr const char* kRevocationPath = "/LikesProgram/System/Revocation.rvk";

    // 用例失败计数
    unsigned g_failures = 0;

    // 断言：失败只记账并打印，继续跑完剩下的用例
    void Require(bool ok, const std::string& what) {
        if (ok) return;
        ++g_failures;
        std::cerr << "CheckRevocation: 失败 " << what << "\n";
    }

    // 小端写入 16 位
    void Put16(Bytes& bytes, std::size_t offset, uint16_t value) {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    // 小端写入 32 位
    void Put32(Bytes& bytes, std::size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // 小端写入 64 位
    void Put64(Bytes& bytes, std::size_t offset, uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // CRC-32：与引导状态记录同一组参数，供手写的版本 1 记录使用
    uint32_t Crc32(const uint8_t* data, std::size_t bytes) {
        uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < bytes; ++i) {
            crc ^= data[i];
            for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    // 固定种子的密钥对
    struct KeyPair {
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
    };

    // 由种子派生密钥对
    KeyPair Derive(const uint8_t seed[32]) {
        KeyPair pair;
        hostbuild::Ed25519DeriveKey(std::span<const uint8_t>(seed, 32), pair.publicKey, pair.privateKey);
        return pair;
    }

    // 签名密钥（KeyId 1）的密钥对
    const KeyPair& SignerPair() {
        static const KeyPair pair = Derive(kSignerSeed);
        return pair;
    }

    // 另一把受信密钥（KeyId 3）的密钥对
    const KeyPair& OtherPair() {
        static const KeyPair pair = Derive(kOtherSeed);
        return pair;
    }

    // 把某个 KeyId 标进吊销位图
    void RevokeKey(RV::Facts& facts, uint8_t keyId) {
        const uint32_t bit = static_cast<uint32_t>(keyId) - 1;
        facts.revokedKeyMask[bit / 8] = static_cast<uint8_t>(facts.revokedKeyMask[bit / 8] | (1u << (bit % 8)));
    }

    // 构造并签名一份吊销记录：与 MakeRevocation 同一套字节表与签名规则
    Bytes BuildRecord(uint64_t epoch, uint64_t minimumSecurityVersion, const std::vector<uint8_t>& revokedKeyIds, uint8_t signerKeyId, const KeyPair& signer, bool sign = true) {
        Bytes record(RV::kRecordBytes, 0);
        Put32(record, RV::kFieldMagic, RV::kMagic);
        Put16(record, RV::kFieldVersion, RV::kVersion);
        Put16(record, RV::kFieldHeaderBytes, RV::kHeaderBytes);
        Put32(record, RV::kFieldBytes, RV::kRecordBytes);
        Put32(record, RV::kFieldFlags, 0);
        Put64(record, RV::kFieldEpoch, epoch);
        Put64(record, RV::kFieldMinimumSecurityVersion, minimumSecurityVersion);
        for (const uint8_t keyId : revokedKeyIds) {
            const uint32_t bit = static_cast<uint32_t>(keyId) - 1;
            record[RV::kFieldRevokedKeyMask + bit / 8] = static_cast<uint8_t>(record[RV::kFieldRevokedKeyMask + bit / 8] | (1u << (bit % 8)));
        }
        record[RV::kHeaderBytes + RV::kSignatureAlgorithm] = RV::kAlgorithmEd25519;
        record[RV::kHeaderBytes + RV::kSignatureKeyId] = signerKeyId;
        if (sign) {
            std::array<uint8_t, hostbuild::kEd25519SignatureBytes> signature{};
            hostbuild::Ed25519Sign(signer.privateKey, record, signature);
            std::memcpy(record.data() + RV::kHeaderBytes + RV::kSignatureValue, signature.data(), signature.size());
        }
        return record;
    }

    // 构造一份通过内核镜像数据检查的 .os 内容：头部 0x80 字节加填充
    Bytes BuildKernelImage(const uint8_t buildId[32]) {
        using L = Kernel::ImageDataLayout;
        Bytes image(0x200, 0);
        std::memcpy(image.data(), Kernel::kImageMagic, sizeof(Kernel::kImageMagic));
        Put16(image, L::kFieldVersion, L::kVersion);
        Put16(image, L::kFieldDataBytes, L::kBytes);
        Put32(image, L::kFieldFlags, L::kFlags);
        Put32(image, L::kFieldImageBytes, 0x200);
        Put32(image, L::kFieldMemoryBytes, 0x200);
        Put32(image, L::kFieldEntryOffset, L::kBytes);
        Put32(image, L::kFieldLoadAddress, L::kLoadAlign);
        Put16(image, L::kFieldDigestAlgorithm, L::kDigestSha256);
        Put16(image, L::kFieldBuildIdBytes, L::kBuildIdBytes);
        Put32(image, L::kFieldBootInfoVersion, 1);
        std::memcpy(image.data() + L::kFieldBuildId, buildId, ML::kBuildIdBytes);
        hostbuild::Sha256 hash;
        hash.Begin();
        hash.Update(image);
        const std::array<uint8_t, hostbuild::kSha256Bytes> digest = hash.Finish();
        std::memcpy(image.data() + L::kFieldDigest, digest.data(), digest.size());
        return image;
    }

    // 构造一份可验证的清单：头部、一条 .os 产物与签名区
    Bytes BuildManifest(const uint8_t buildId[32], uint64_t securityVersion, const std::string& artifactPath, uint64_t artifactBytes, const uint8_t digest[32], uint8_t signerKeyId, const KeyPair& signer) {
        const std::size_t pathBytes = artifactPath.size();
        const std::size_t total = ML::kHeaderBytes + ML::kEntryFixedBytes + pathBytes + ML::kSignatureAreaBytes;
        Bytes manifest(total, 0);
        Put32(manifest, ML::kFieldMagic, ML::kMagic);
        Put16(manifest, ML::kFieldVersion, ML::kVersion);
        Put16(manifest, ML::kFieldHeaderBytes, ML::kHeaderBytes);
        Put32(manifest, ML::kFieldBytes, static_cast<uint32_t>(total));
        Put32(manifest, ML::kFieldFlags, 0);
        std::memcpy(manifest.data() + ML::kFieldBuildId, buildId, ML::kBuildIdBytes);
        Put64(manifest, ML::kFieldSecurityVersion, securityVersion);
        Put16(manifest, ML::kFieldArtifactCount, 1);
        const std::size_t entry = ML::kFieldArtifacts;
        manifest[entry + ML::kEntryType] = ML::kTypeOs;
        Put16(manifest, entry + ML::kEntryPathBytes, static_cast<uint16_t>(pathBytes));
        Put64(manifest, entry + ML::kEntryDataBytes, artifactBytes);
        std::memcpy(manifest.data() + entry + ML::kEntryDigest, digest, ML::kEntryDigestBytes);
        std::memcpy(manifest.data() + entry + ML::kEntryPath, artifactPath.data(), pathBytes);
        const std::size_t signatureArea = total - ML::kSignatureAreaBytes;
        manifest[signatureArea + ML::kSignatureAlgorithm] = ML::kAlgorithmEd25519;
        manifest[signatureArea + ML::kSignatureKeyId] = signerKeyId;
        std::array<uint8_t, hostbuild::kEd25519SignatureBytes> signature{};
        hostbuild::Ed25519Sign(signer.privateKey, manifest, signature);
        std::memcpy(manifest.data() + signatureArea + ML::kSignatureValue, signature.data(), signature.size());
        return manifest;
    }

    // 内容摘要
    std::array<uint8_t, hostbuild::kSha256Bytes> DigestOf(const Bytes& bytes) {
        hostbuild::Sha256 hash;
        hash.Begin();
        hash.Update(bytes);
        return hash.Finish();
    }

    // 64 位小写十六进制：集目录名用
    std::string Hex64(const uint8_t buildId[32]) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        for (unsigned i = 0; i < 32; ++i) {
            text.push_back(kDigits[buildId[i] >> 4]);
            text.push_back(kDigits[buildId[i] & 0x0f]);
        }
        return text;
    }

    // 内存文件系统：按路径索引内容，供吊销记录读取与清单验证使用
    class MemoryFileSystem final : public Storage::FileSystem {
    public:
        // 登记一个文件
        void Add(const std::string& path, const Bytes& content) {
            m_files.push_back(Entry{path, content});
        }
        // 撤掉一个文件：模拟记录被删除
        void Remove(const std::string& path) {
            m_files.erase(std::remove_if(m_files.begin(), m_files.end(), [&](const Entry& entry) { return entry.path == path; }), m_files.end());
        }
        // 覆盖一个文件的内容：模拟篡改
        void Overwrite(const std::string& path, const Bytes& content) {
            for (Entry& entry : m_files) {
                if (entry.path == path) entry.content = content;
            }
        }
        // 事实查询：文件不存在返回 NotFound
        Storage::Error Stat(const char* path, Storage::FileInfo& info) override {
            for (std::size_t i = 0; i < m_files.size(); ++i) {
                if (m_files[i].path != path) continue;
                info.bytes = m_files[i].content.size();
                info.id = i + 1;
                info.directory = false;
                return Storage::Error::Ok;
            }
            return Storage::Error::NotFound;
        }
        // 打开：返回 1 起的下标作为标识
        Storage::Error Open(const char* path, Storage::FileId& id) override {
            for (std::size_t i = 0; i < m_files.size(); ++i) {
                if (m_files[i].path != path) continue;
                id.value = i + 1;
                return Storage::Error::Ok;
            }
            return Storage::Error::NotFound;
        }
        // 按标识读取
        Storage::Error Read(const Storage::FileId& id, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) override {
            read = 0;
            if (id.value == 0 || id.value > m_files.size()) return Storage::Error::NotFound;
            const Bytes& content = m_files[id.value - 1].content;
            if (offset > content.size() || bytes > content.size() - offset) return Storage::Error::Io;
            std::memcpy(buffer, content.data() + offset, bytes);
            read = bytes;
            return Storage::Error::Ok;
        }
    private:
        // 一个文件：路径与内容
        struct Entry {
            std::string path;   // 卷内绝对路径
            Bytes content;      // 内容
        };
        std::vector<Entry> m_files;   // 登记的文件
    };

    // 内存介质：引导状态记录用
    class MemoryMedium final : public Storage::BlockDevice {
    public:
        // 构造：长度与可写性
        explicit MemoryMedium(bool writable) : m_bytes(kMediumBytes, 0), m_writable(writable) {}
        // 逻辑扇区大小
        uint32_t BlockBytes() const override { return 512; }
        // 容量
        uint64_t CapacityBytes() const override { return m_bytes.size(); }
        // 可写性：只读介质返回 false，写入入口据此拒绝
        bool Writable() const override { return m_writable; }
        // 读
        Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override {
            if (offset > m_bytes.size() || bytes > m_bytes.size() - offset) return Storage::Error::Io;
            std::memcpy(buffer, m_bytes.data() + offset, bytes);
            return Storage::Error::Ok;
        }
        // 写
        Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override {
            if (!m_writable) return Storage::Error::ReadOnly;
            if (offset > m_bytes.size() || bytes > m_bytes.size() - offset) return Storage::Error::Io;
            std::memcpy(m_bytes.data() + offset, buffer, bytes);
            return Storage::Error::Ok;
        }
        // 刷盘：直接落内存
        Storage::Error Flush() override { return Storage::Error::Ok; }
        // 供用例直接改写原始字节：手写版本 1 记录
        void RawWrite(uint64_t offset, const uint8_t* source, uint32_t bytes) { std::memcpy(m_bytes.data() + offset, source, bytes); }
    private:
        std::vector<uint8_t> m_bytes;   // 介质内容
        bool m_writable = true;         // 是否可写
    };

    // 装配一份可验证的集：集目录、清单与 .os 产物都进文件系统
    void BuildSet(MemoryFileSystem& fs, const uint8_t buildId[32], uint64_t securityVersion, std::string& setDirectory) {
        const std::string artifactPath = "LikesProgram.os";
        const Bytes image = BuildKernelImage(buildId);
        const std::array<uint8_t, hostbuild::kSha256Bytes> digest = DigestOf(image);
        const Bytes manifest = BuildManifest(buildId, securityVersion, artifactPath, image.size(), digest.data(), kSignerKeyId, SignerPair());
        setDirectory = "/LikesProgram/System/KernelSets/" + Hex64(buildId) + "/";
        fs.Add(setDirectory + "Manifest.mft", manifest);
        fs.Add(setDirectory + artifactPath, image);
    }

    // 用例：锚表与固定种子的派生结果逐字节一致
    void CaseAnchorDerivation() {
        Require(Baleen::Crypto::kTrustAnchorCount == 2, "本工具自带的锚表应有 2 条");
        Require(std::equal(SignerPair().publicKey.begin(), SignerPair().publicKey.end(), kSignerPublicKey), "KeyId 1 的公钥不是固定种子的派生结果");
        Require(std::equal(OtherPair().publicKey.begin(), OtherPair().publicKey.end(), kOtherPublicKey), "KeyId 3 的公钥不是固定种子的派生结果");
        Require(std::equal(Baleen::Crypto::kTrustAnchors[0].publicKey, Baleen::Crypto::kTrustAnchors[0].publicKey + 32, kSignerPublicKey), "锚表第一条与 KeyId 1 的公钥不符");
        Require(std::equal(Baleen::Crypto::kTrustAnchors[1].publicKey, Baleen::Crypto::kTrustAnchors[1].publicKey + 32, kOtherPublicKey), "锚表第二条与 KeyId 3 的公钥不符");
        Require(Baleen::Crypto::FindTrustAnchor(kSignerKeyId) != nullptr, "按 KeyId 1 应命中锚");
        Require(Baleen::Crypto::FindTrustAnchor(2) == nullptr, "表外的 KeyId 2 不应命中");
    }

    // 用例：记录结构检查的正例与负例
    void CaseRecordStructure() {
        const Bytes good = BuildRecord(1, 0, {}, kSignerKeyId, SignerPair());
        uint8_t signer = 0;
        Require(RV::CheckRecord(good.data(), static_cast<uint32_t>(good.size()), signer), "生成的记录应通过结构检查");
        Require(signer == kSignerKeyId, "结构检查应读出签名者编号 1");

        // 长度不符
        Require(!RV::CheckRecord(good.data(), static_cast<uint32_t>(good.size()) - 1, signer), "长度少一字节应被拒绝");
        Require(!RV::CheckRecord(good.data(), static_cast<uint32_t>(good.size()) + 1, signer), "长度多一字节应被拒绝");

        // 逐项破坏结构字段
        struct FieldCase {
            const char* what;      // 破坏项
            std::size_t offset;    // 破坏位置
            uint8_t value;         // 写入值
        };
        const FieldCase cases[] = {
            { "格式标记", RV::kFieldMagic, 0x00 },
            { "版本", RV::kFieldVersion, 2 },
            { "头部长度", RV::kFieldHeaderBytes, 0x00 },
            { "长度字段", RV::kFieldBytes, 0x00 },
            { "标志字段", RV::kFieldFlags, 1 },
            { "纪元为零", RV::kFieldEpoch, 0x00 },
            { "签名算法", RV::kHeaderBytes + RV::kSignatureAlgorithm, 0 },
            { "签名密钥编号", RV::kHeaderBytes + RV::kSignatureKeyId, 0 },
            { "签名区保留字节", RV::kHeaderBytes + RV::kSignatureReserved, 1 },
        };
        for (const FieldCase& item : cases) {
            Bytes broken = good;
            broken[item.offset] = item.value;
            uint8_t keyId = 0;
            Require(!RV::CheckRecord(broken.data(), static_cast<uint32_t>(broken.size()), keyId), std::string("破坏") + item.what + "后应被拒绝");
        }
    }

    // 用例：验签、换错公钥、改动字节与签名者自吊销
    void CaseSignatureVerify() {
        const Bytes good = BuildRecord(2, 7, { kOtherKeyId }, kSignerKeyId, SignerPair());
        Bytes working = good;
        uint8_t signerKeyId = 0;
        Require(RV::CheckRecord(working.data(), static_cast<uint32_t>(working.size()), signerKeyId), "正例记录应通过结构检查");
        RV::Facts facts;
        Require(RV::VerifyRecord(working.data(), static_cast<uint32_t>(working.size()), signerKeyId, kSignerPublicKey, &Baleen::Crypto::Ed25519Verify, facts) == nullptr, "正例记录应验签通过");
        Require(facts.epoch == 2 && facts.minimumSecurityVersion == 7, "验签通过后应解出纪元与下限");
        Require(RV::IsKeyRevoked(facts.revokedKeyMask, kOtherKeyId) && !RV::IsKeyRevoked(facts.revokedKeyMask, kSignerKeyId), "位图应只标 KeyId 3");

        // 换错公钥：KeyId 3 的公钥验不过 KeyId 1 签的记录
        working = good;
        RV::Facts ignored;
        Require(RV::VerifyRecord(working.data(), static_cast<uint32_t>(working.size()), signerKeyId, kOtherPublicKey, &Baleen::Crypto::Ed25519Verify, ignored) != nullptr, "用另一把公钥验签应失败");

        // 改动一个字节：纪元字段被改，签名不再成立
        working = good;
        working[RV::kFieldEpoch] = static_cast<uint8_t>(working[RV::kFieldEpoch] ^ 0x01);
        Require(RV::VerifyRecord(working.data(), static_cast<uint32_t>(working.size()), signerKeyId, kSignerPublicKey, &Baleen::Crypto::Ed25519Verify, ignored) != nullptr, "改动纪元字节后验签应失败");

        // 签名者自吊销：即使签名本身有效，记录也不成立
        const Bytes selfRevoked = BuildRecord(3, 0, { kSignerKeyId }, kSignerKeyId, SignerPair());
        working = selfRevoked;
        Require(RV::CheckRecord(working.data(), static_cast<uint32_t>(working.size()), signerKeyId), "自吊销记录的结构本身是合法的");
        Require(RV::VerifyRecord(working.data(), static_cast<uint32_t>(working.size()), signerKeyId, kSignerPublicKey, &Baleen::Crypto::Ed25519Verify, ignored) != nullptr, "签名者自吊销的记录应被拒绝");
    }

    // 用例：位图定位与并集只增不减
    void CaseKeyMaskAndMerge() {
        RV::Facts facts;
        Require(!RV::IsKeyRevoked(facts.revokedKeyMask, 0), "KeyId 0 是保留值，不构成吊销");
        // 每个编号单独用一张干净位图：标进去的必须恰好是这一位，跨字节边界（8/9、32、64、255）都要对
        const uint8_t boundaries[] = { 1, 8, 9, 32, 64, 255 };
        for (const uint8_t keyId : boundaries) {
            RV::Facts single;
            RevokeKey(single, keyId);
            Require(RV::IsKeyRevoked(single.revokedKeyMask, keyId), std::string("KeyId ") + std::to_string(keyId) + " 应被标进位图");
            unsigned marked = 0;
            for (unsigned other = 1; other <= 255; ++other) {
                if (RV::IsKeyRevoked(single.revokedKeyMask, static_cast<uint8_t>(other))) ++marked;
            }
            Require(marked == 1, std::string("KeyId ") + std::to_string(keyId) + " 应只标一位，实际标了 " + std::to_string(marked) + " 位");
        }

        // 并集：纪元与下限取较大者、位图按位或，不会被较小的输入拉回去
        RV::Facts merged;
        merged.epoch = 5;
        merged.minimumSecurityVersion = 9;
        RevokeKey(merged, 1);
        RV::Facts older;
        older.epoch = 2;
        older.minimumSecurityVersion = 4;
        RevokeKey(older, kOtherKeyId);
        RV::Merge(merged, older);
        Require(merged.epoch == 5, "并集的纪元应取较大者");
        Require(merged.minimumSecurityVersion == 9, "并集的下限应取较大者");
        Require(RV::IsKeyRevoked(merged.revokedKeyMask, 1) && RV::IsKeyRevoked(merged.revokedKeyMask, kOtherKeyId), "并集应保留两侧的吊销位");

        // 反向并入一份更小的事实：结果不变
        RV::Facts smaller;
        smaller.epoch = 1;
        RV::Merge(merged, smaller);
        Require(merged.epoch == 5 && merged.minimumSecurityVersion == 9, "并入更小的事实不应改变结果");
    }

    // 用例：系统卷读取的三态
    void CaseLoadResults() {
        const Bytes good = BuildRecord(4, 0, { kOtherKeyId }, kSignerKeyId, SignerPair());
        MemoryFileSystem fs;
        const char* reason = nullptr;
        RV::Facts facts;

        // 缺文件：Missing，facts 不变
        Require(RV::Load(fs, kRevocationPath, &Baleen::Crypto::FindTrustAnchor, &Baleen::Crypto::Ed25519Verify, facts, reason) == RV::LoadResult::Missing, "没有文件时应返回 Missing");
        Require(facts.epoch == 0, "Missing 不应改动事实");

        // 长度不符：Unusable
        fs.Add(kRevocationPath, Bytes(16, 0));
        Require(RV::Load(fs, kRevocationPath, &Baleen::Crypto::FindTrustAnchor, &Baleen::Crypto::Ed25519Verify, facts, reason) == RV::LoadResult::Unusable, "长度不符的记录应返回 Unusable");
        Require(reason != nullptr, "Unusable 应给出原因文本");

        // 篡改一个字节：Unusable
        Bytes tampered = good;
        tampered[RV::kFieldMinimumSecurityVersion] = 0x7f;
        fs.Overwrite(kRevocationPath, tampered);
        Require(RV::Load(fs, kRevocationPath, &Baleen::Crypto::FindTrustAnchor, &Baleen::Crypto::Ed25519Verify, facts, reason) == RV::LoadResult::Unusable, "篡改后的记录应返回 Unusable");

        // 合法记录：Applied，事实与记录一致
        fs.Overwrite(kRevocationPath, good);
        Require(RV::Load(fs, kRevocationPath, &Baleen::Crypto::FindTrustAnchor, &Baleen::Crypto::Ed25519Verify, facts, reason) == RV::LoadResult::Applied, "合法记录应返回 Applied");
        Require(facts.epoch == 4 && RV::IsKeyRevoked(facts.revokedKeyMask, kOtherKeyId), "Applied 应解出纪元与位图");
    }

    // 用例：真实清单验证的吊销判定与下限判定
    void CaseManifestVerification() {
        uint8_t buildId[32] = {};
        for (unsigned i = 0; i < 32; ++i) buildId[i] = static_cast<uint8_t>(0x40 + i);
        MemoryFileSystem fs;
        std::string setDirectory;
        BuildSet(fs, buildId, 5, setDirectory);
        Baleen::Core::ManifestFacts facts;

        // 没有吊销事实：清单通过
        RV::Facts none;
        Require(Baleen::Core::VerifyKernelSet(fs, setDirectory.c_str(), buildId, 0, none, facts).code == nullptr, "没有吊销事实时清单应通过验证");

        // 吊销签名密钥 1：按 E-CORE-74 拒绝
        RV::Facts revoked;
        revoked.epoch = 1;
        RevokeKey(revoked, kSignerKeyId);
        const Baleen::Diagnostic rejected = Baleen::Core::VerifyKernelSet(fs, setDirectory.c_str(), buildId, 0, revoked, facts);
        Require(rejected.code != nullptr && std::string(rejected.code) == "E-CORE-74", "被吊销的签名密钥应按 E-CORE-74 拒绝，实际是 " + std::string(rejected.code == nullptr ? "通过" : rejected.code));

        // 吊销另一把密钥：本清单的签名者不受影响，仍然通过
        RV::Facts other;
        other.epoch = 1;
        RevokeKey(other, kOtherKeyId);
        Require(Baleen::Core::VerifyKernelSet(fs, setDirectory.c_str(), buildId, 0, other, facts).code == nullptr, "吊销别的密钥不应影响本清单");

        // 记录抬高的下限高于清单的安全版本（5）：按防回滚 E-CORE-66 拒绝
        RV::Facts floor;
        floor.epoch = 1;
        floor.minimumSecurityVersion = 6;
        const Baleen::Diagnostic rolled = Baleen::Core::VerifyKernelSet(fs, setDirectory.c_str(), buildId, 0, floor, facts);
        Require(rolled.code != nullptr && std::string(rolled.code) == "E-CORE-66", "低于吊销记录下限的清单应按 E-CORE-66 拒绝");

        // 下限等于清单版本：不拒绝
        floor.minimumSecurityVersion = 5;
        Require(Baleen::Core::VerifyKernelSet(fs, setDirectory.c_str(), buildId, 0, floor, facts).code == nullptr, "下限等于清单版本时不应拒绝");
    }

    // 用例：单调性——记录文件缺失与被回滚成旧纪元都不解除持久事实
    void CaseMonotonicFacts() {
        MemoryMedium medium(true);
        Baleen::Core::BootState::Record record;
        record.state = Baleen::Core::BootState::State::Confirmed;
        std::memset(record.confirmedBuildId, 0x5a, sizeof(record.confirmedBuildId));
        record.revocation.epoch = 6;
        record.revocation.minimumSecurityVersion = 9;
        RevokeKey(record.revocation, kSignerKeyId);
        uint64_t generation = 0;
        Require(Baleen::Core::BootState::Write(medium, kStateOffset, kStateBytes, record, generation), "带吊销事实的记录应能落盘");

        // 记录文件缺失：持久事实仍在
        MemoryFileSystem empty;
        const char* reason = nullptr;
        RV::Facts fromFile;
        Require(RV::Load(empty, kRevocationPath, &Baleen::Crypto::FindTrustAnchor, &Baleen::Crypto::Ed25519Verify, fromFile, reason) == RV::LoadResult::Missing, "缺文件应返回 Missing");
        RV::Facts effective = fromFile;
        RV::Merge(effective, record.revocation);
        Require(RV::IsKeyRevoked(effective.revokedKeyMask, kSignerKeyId), "文件缺失时持久事实应仍然生效");
        Require(effective.epoch == 6 && effective.minimumSecurityVersion == 9, "文件缺失时纪元与下限应保持");

        // 记录被回滚成旧纪元且不吊销任何密钥：并集后仍不解除
        RV::Facts rolledBack;
        rolledBack.epoch = 2;
        RV::Facts merged = rolledBack;
        RV::Merge(merged, record.revocation);
        Require(RV::IsKeyRevoked(merged.revokedKeyMask, kSignerKeyId), "回滚成旧纪元的记录不应解除吊销");
        Require(merged.epoch == 6 && merged.minimumSecurityVersion == 9, "回滚成旧纪元的记录不应拉低下限");

        // 记忆体中的事实经读回仍然一致
        Baleen::Core::BootState::Record readBack;
        Require(Baleen::Core::BootState::Read(medium, kStateOffset, kStateBytes, readBack) == Baleen::Core::BootState::ReadResult::Ok, "记录应能读回");
        Require(readBack.revocation.epoch == 6 && RV::IsKeyRevoked(readBack.revocation.revokedKeyMask, kSignerKeyId), "读回的吊销事实应与写入一致");
    }

    // 用例：版本 2 往返与版本 1 读取兼容
    void CaseRecordVersions() {
        Require(Baleen::Core::BootState::kVersion == 2 && Baleen::Core::BootState::kRecordBytes == 0xF0, "当前记录版本应是 2、长度 0xF0");

        // 版本 2：（在 CaseMonotonicFacts 已覆盖内容往返）这里核对保留区非零被拒
        MemoryMedium medium(true);
        Baleen::Core::BootState::Record record;
        record.state = Baleen::Core::BootState::State::Confirmed;
        std::memset(record.confirmedBuildId, 0x33, sizeof(record.confirmedBuildId));
        uint64_t generation = 0;
        Require(Baleen::Core::BootState::Write(medium, kStateOffset, kStateBytes, record, generation), "版本 2 记录应能落盘");

        // 手写一份版本 1 记录（0xC0、校验和在 0xBC），读方必须接受且撤销字段按全零
        Bytes legacy(0xC0, 0);
        Put32(legacy, 0x000, 0x54534C42u);
        Put16(legacy, 0x004, 1);
        Put16(legacy, 0x006, 0xC0);
        Put64(legacy, 0x008, 7);
        legacy[0x010] = 1;   // Confirmed
        legacy[0x011] = 0;
        std::memset(legacy.data() + 0x020, 0x44, 32);
        std::memset(legacy.data() + 0x040, 0x45, 32);
        Put32(legacy, 0x0BC, Crc32(legacy.data(), 0x0BC));
        MemoryMedium legacyMedium(true);
        legacyMedium.RawWrite(kStateOffset, legacy.data(), static_cast<uint32_t>(legacy.size()));
        Baleen::Core::BootState::Record legacyRead;
        Require(Baleen::Core::BootState::Read(legacyMedium, kStateOffset, kStateBytes, legacyRead) == Baleen::Core::BootState::ReadResult::Ok, "版本 1 的记录应仍可读");
        Require(legacyRead.generation == 7, "版本 1 记录的代次应读出 7");
        Require(legacyRead.revocation.epoch == 0 && legacyRead.revocation.minimumSecurityVersion == 0, "版本 1 记录的撤销字段应按全零");
        Require(!RV::IsKeyRevoked(legacyRead.revocation.revokedKeyMask, kSignerKeyId), "版本 1 记录不应带任何吊销位");

        // 未知版本：不猜测字段表
        Bytes unknown = legacy;
        Put16(unknown, 0x004, 9);
        MemoryMedium unknownMedium(true);
        unknownMedium.RawWrite(kStateOffset, unknown.data(), static_cast<uint32_t>(unknown.size()));
        Baleen::Core::BootState::Record ignored;
        Require(Baleen::Core::BootState::Read(unknownMedium, kStateOffset, kStateBytes, ignored) == Baleen::Core::BootState::ReadResult::Corrupt, "未知版本的记录应按损坏处理");
    }

    // 跑一个用例
    void Case(const char* name, void (*body)()) {
        const unsigned before = g_failures;
        body();
        std::cout << (g_failures == before ? "  [通过] " : "  [失败] ") << name << "\n";
    }
}

namespace Baleen {
    namespace Crypto {
        // 本工具自带的信任锚表：两条锚的公钥由固定种子派生，用例里核对派生结果与这里逐字节一致；
        // 构建期生成的 Out/TrustAnchors.cpp 定义同名符号，这里绝不编它，否则撞符号
        extern const TrustAnchor kTrustAnchors[] = {
            { 1, {
                0x77, 0x76, 0xe8, 0x70, 0xb9, 0x33, 0x54, 0xf2,
                0xa0, 0xb2, 0x4c, 0x23, 0xf2, 0xa3, 0x6c, 0xc4,
                0xe8, 0x0e, 0x22, 0x32, 0x18, 0xc1, 0xb9, 0x79,
                0x26, 0xfd, 0xd0, 0x18, 0x39, 0x6a, 0x2b, 0x9b,
            } },
            { 3, {
                0xcd, 0x14, 0xb3, 0x7f, 0x95, 0x6e, 0x95, 0x31,
                0x94, 0xff, 0x7f, 0xb7, 0x3b, 0x3d, 0x81, 0xdc,
                0xc5, 0x61, 0xd6, 0x1a, 0x75, 0x38, 0x09, 0x4b,
                0x7c, 0x3e, 0x1a, 0x64, 0x3e, 0xe5, 0xf3, 0xaa,
            } },
        };
        // 锚表条目数
        extern const uint32_t kTrustAnchorCount = 2;
        // 容量由 TrustAnchor.hpp 给出：超出即编译失败
        static_assert(kTrustAnchorCount <= kTrustAnchorMaxCount, "信任锚表超过 TrustAnchor.hpp 的容量");
    }
}

// 入口：跑完全部用例
int main() {
    std::cout << "CheckRevocation：吊销记录与引导状态区单调纪元\n";
    Case("锚表与固定种子派生一致", CaseAnchorDerivation);
    Case("记录结构检查", CaseRecordStructure);
    Case("签名与自吊销", CaseSignatureVerify);
    Case("位图定位与并集", CaseKeyMaskAndMerge);
    Case("系统卷读取三态", CaseLoadResults);
    Case("真实清单验证：吊销与下限", CaseManifestVerification);
    Case("单调性：文件缺失与回滚不解除", CaseMonotonicFacts);
    Case("记录版本：版本 2 往返与版本 1 兼容", CaseRecordVersions);
    if (g_failures != 0) {
        std::cerr << "CheckRevocation: " << g_failures << " 项断言失败\n";
        return 1;
    }
    std::cout << "CheckRevocation: 全部通过\n";
    return 0;
}
