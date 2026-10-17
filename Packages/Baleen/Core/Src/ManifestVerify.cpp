/* ManifestVerify.cpp
    内核集清单与产物验证的实现：读清单、核对结构与签名、防回滚比较与逐产物摘要

    清单最大 0x10000 字节，产物的摘要按 64 KiB 块流式重算；两者各用一块静态缓冲
    签名覆盖是整份清单（签名值 64 字节视作零），ManifestDigest 与签名使用同一段字节
    集内一致的内核镜像数据检查只读 .os 文件的首块，摘要在逐产物的流式比对里覆盖
*/

#include <ManifestVerify.hpp>

#include <BootMemory.hpp>
#include <Ed25519.hpp>
#include <KernelImage.hpp>
#include <Libc.hpp>
#include <SecurityFloor.hpp>
#include <Sha256.hpp>
#include <TrustAnchor.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            namespace ML = KernelSet::ManifestLayout;

            // 清单缓冲：按清单长度上限分配
            uint8_t s_manifest[ML::kManifestMaxBytes];
            // 产物摘要的读取缓冲：块大小取低地址弹跳窗口，与装载路径一致
            uint8_t s_chunk[kBounceBytes];
            // 集内路径缓冲：集目录（约 120 字节）加相对路径（上限 255）充裕
            constexpr uint32_t kPathBytes = 512;
            char s_path[kPathBytes];

            // 引导器内嵌的最低允许 SecurityVersion 由构建期生成头给出：
            // 默认 0 表示不吊销任何发行世代；make SECURITY_FLOOR=<n> 抬高它即吊销更低世代
            // 撤销的生效点是一次可信的引导器更新，见 Docs/Specs/内核集清单与签名.md 第五节

            // —— 本阶段失败原因的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailManifestRead{BALEEN_STAGE_CODE("61"), "Read the kernel set manifest"};
            constexpr Diagnostic kFailManifestFormat{BALEEN_STAGE_CODE("62"), "The kernel set manifest is invalid"};
            constexpr Diagnostic kFailManifestKey{BALEEN_STAGE_CODE("63"), "The manifest signature key isn't trusted"};
            constexpr Diagnostic kFailManifestSignature{BALEEN_STAGE_CODE("64"), "The manifest signature doesn't verify"};
            constexpr Diagnostic kFailManifestBuildId{BALEEN_STAGE_CODE("65"), "The manifest BuildId doesn't match the selected set"};
            constexpr Diagnostic kFailRollback{BALEEN_STAGE_CODE("66"), "The manifest SecurityVersion is below what the device or the bootloader allows"};
            constexpr Diagnostic kFailArtifactRead{BALEEN_STAGE_CODE("67"), "Read a kernel set artifact"};
            constexpr Diagnostic kFailArtifactDigest{BALEEN_STAGE_CODE("68"), "A kernel set artifact digest doesn't match"};
            constexpr Diagnostic kFailKeyRevoked{BALEEN_STAGE_CODE("74"), "The manifest signature key is revoked"};

            // 按小端读取 16 位
            uint16_t Get16(const uint8_t* field) {
                return static_cast<uint16_t>(field[0] | (field[1] << 8));
            }

            // 按小端读取 32 位
            uint32_t Get32(const uint8_t* field) {
                uint32_t value = 0;
                for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
                return value;
            }

            // 按小端读取 64 位
            uint64_t Get64(const uint8_t* field) {
                return static_cast<uint64_t>(Get32(field)) | (static_cast<uint64_t>(Get32(field + 4)) << 32);
            }

            // 拼接 path = directory + "/" + relative；相接处只保留一个 '/'
            bool JoinPath(const char* directory, const char* relative, uint32_t relativeBytes) {
                uint32_t length = 0;
                for (; directory[length] != '\0'; ++length) {
                    if (length + 1 >= kPathBytes) return false;
                    s_path[length] = directory[length];
                }
                if (length > 0 && s_path[length - 1] != '/') s_path[length++] = '/';
                for (uint32_t i = 0; i < relativeBytes; ++i) {
                    if (length + 1 >= kPathBytes) return false;
                    s_path[length++] = relative[i];
                }
                s_path[length] = '\0';
                return true;
            }

            // 从 offset 起读 want 字节到块缓冲；长度不足或读失败返回带编号的原因
            Diagnostic ReadChunk(Storage::FileSystem& fs, const Storage::FileId& id, uint32_t offset, uint32_t want) {
                uint32_t read = 0;
                if (const Storage::Error error = fs.Read(id, offset, s_chunk, want, read); error != Storage::Error::Ok) return kFailArtifactRead;
                if (read != want) return kFailArtifactRead;
                return Diagnostic{};
            }

            // 条目路径是否合法：非空、不以 / 开头、分量非空、不含 . 与 ..
            bool PathValid(const uint8_t* path, uint32_t bytes) {
                if (bytes == 0) return false;
                if (path[0] == '/') return false;
                uint32_t begin = 0;
                for (uint32_t i = 0; i <= bytes; ++i) {
                    if (i < bytes && path[i] != '/') continue;
                    const uint32_t component = i - begin;
                    if (component == 0) return false;
                    if (component == 1 && path[begin] == '.') return false;
                    if (component == 2 && path[begin] == '.' && path[begin + 1] == '.') return false;
                    begin = i + 1;
                }
                return true;
            }

            // 清单尾部签名区的结构检查与取用：算法、密钥编号与签名值
            bool CheckSignatureArea(const uint8_t* manifest, uint32_t bytes, uint8_t& keyId, const uint8_t*& signature) {
                const uint32_t begin = bytes - ML::kSignatureAreaBytes;
                if (manifest[begin + ML::kSignatureAlgorithm] != ML::kAlgorithmEd25519) return false;
                keyId = manifest[begin + ML::kSignatureKeyId];
                if (keyId == 0) return false;
                if (manifest[begin + ML::kSignatureReserved] != 0 || manifest[begin + ML::kSignatureReserved + 1] != 0) return false;
                signature = manifest + begin + ML::kSignatureValue;
                return true;
            }
        }

        Diagnostic VerifyKernelSet(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, ManifestFacts& facts) {
            // 1. 定位清单：集目录 + Manifest.mft（后缀见 Docs/Specs/后缀设计.md）
            if (!JoinPath(setDirectory, "Manifest.mft", 12)) return kFailManifestRead;
            Storage::FileId id;
            if (fs.Open(s_path, id) != Storage::Error::Ok) return kFailManifestRead;
            Storage::FileInfo info;
            if (fs.Stat(s_path, info) != Storage::Error::Ok) return kFailManifestRead;
            if (info.directory || info.bytes < ML::kHeaderBytes + ML::kSignatureAreaBytes || info.bytes > ML::kManifestMaxBytes) return kFailManifestFormat;
            const uint32_t bytes = static_cast<uint32_t>(info.bytes);
            uint32_t read = 0;
            if (fs.Read(id, 0, s_manifest, bytes, read) != Storage::Error::Ok || read != bytes) return kFailManifestRead;

            // 2. 结构与保留字段：头部固定值、长度一致、保留字段为零、签名区字段合法
            if (Get32(s_manifest + ML::kFieldMagic) != ML::kMagic) return kFailManifestFormat;
            if (Get16(s_manifest + ML::kFieldVersion) != ML::kVersion) return kFailManifestFormat;
            if (Get16(s_manifest + ML::kFieldHeaderBytes) != ML::kHeaderBytes) return kFailManifestFormat;
            if (Get32(s_manifest + ML::kFieldBytes) != bytes) return kFailManifestFormat;
            if (Get32(s_manifest + ML::kFieldFlags) != 0) return kFailManifestFormat;
            if (Get16(s_manifest + ML::kFieldReserved16) != 0 || Get32(s_manifest + ML::kFieldReserved32) != 0) return kFailManifestFormat;
            const uint16_t artifactCount = Get16(s_manifest + ML::kFieldArtifactCount);
            if (artifactCount == 0 || artifactCount > ML::kEntryMaxCount) return kFailManifestFormat;
            uint8_t keyId = 0;
            const uint8_t* signature = nullptr;
            if (!CheckSignatureArea(s_manifest, bytes, keyId, signature)) return kFailManifestSignature;

            const uint32_t signatureBegin = bytes - ML::kSignatureAreaBytes;
            uint32_t offset = ML::kFieldArtifacts;
            uint32_t osCount = 0;
            for (uint16_t i = 0; i < artifactCount; ++i) {
                if (offset + ML::kEntryFixedBytes > signatureBegin) return kFailManifestFormat;
                const uint8_t type = s_manifest[offset + ML::kEntryType];
                if (type != ML::kTypeOs && type != ML::kTypeOsm) return kFailManifestFormat;
                if (type == ML::kTypeOs) ++osCount;
                const uint16_t pathBytes = Get16(s_manifest + offset + ML::kEntryPathBytes);
                if (pathBytes == 0 || pathBytes > ML::kEntryPathMaxBytes) return kFailManifestFormat;
                if (s_manifest[offset + ML::kEntryReserved] != 0) return kFailManifestFormat;
                if (!PathValid(s_manifest + offset + ML::kEntryPath, pathBytes)) return kFailManifestFormat;
                if (Get64(s_manifest + offset + ML::kEntryDataBytes) == 0) return kFailManifestFormat;
                offset += ML::kEntryFixedBytes + pathBytes;
            }
            if (offset != signatureBegin) return kFailManifestFormat;
            if (osCount != 1) return kFailManifestFormat;

            // 3. 集身份一致：清单 BuildId 必须等于所选集标识
            uint8_t buildIdAny = 0;
            for (uint32_t i = 0; i < ML::kBuildIdBytes; ++i) buildIdAny |= s_manifest[ML::kFieldBuildId + i];
            if (buildIdAny == 0) return kFailManifestFormat;
            if (memcmp(s_manifest + ML::kFieldBuildId, expectedBuildId, ML::kBuildIdBytes) != 0) return kFailManifestBuildId;

            // 防回滚：SecurityVersion 不低于内嵌下限、设备已见最高与吊销记录给出的下限
            const uint64_t securityVersion = Get64(s_manifest + ML::kFieldSecurityVersion);
            if (securityVersion < Crypto::kMinimumSecurityVersion) return kFailRollback;
            if (securityVersion < seenSecurityVersion) return kFailRollback;
            if (securityVersion < revocation.minimumSecurityVersion) return kFailRollback;

            // 4. 摘要与验签：签名值先取出，字段按零代入后覆盖整份清单
            uint8_t signatureValue[Crypto::kEd25519SignatureBytes];
            memcpy(signatureValue, signature, sizeof(signatureValue));
            // 吊销先于查表：被吊销是显式策略结论，比“不在表里”更能说明现场
            if (Revocation::IsKeyRevoked(revocation.revokedKeyMask, keyId)) return kFailKeyRevoked;
            const uint8_t* publicKey = Crypto::FindTrustAnchor(keyId);
            if (publicKey == nullptr) return kFailManifestKey;
            memset(s_manifest + signatureBegin + ML::kSignatureValue, 0, ML::kSignatureValueBytes);
            Digest::Sha256 digest;
            digest.Begin();
            digest.Update(s_manifest, bytes);
            digest.Finish(facts.manifestDigest);
            if (!Crypto::Ed25519Verify(publicKey, signatureValue, s_manifest, bytes)) return kFailManifestSignature;

            // 5. 逐产物：路径合法已在结构检查里核对；这里核对存在、长度与摘要
            offset = ML::kFieldArtifacts;
            for (uint16_t i = 0; i < artifactCount; ++i) {
                const uint8_t type = s_manifest[offset + ML::kEntryType];
                const uint16_t pathBytes = Get16(s_manifest + offset + ML::kEntryPathBytes);
                const uint64_t dataBytes = Get64(s_manifest + offset + ML::kEntryDataBytes);
                const uint8_t* expectedDigest = s_manifest + offset + ML::kEntryDigest;
                if (!JoinPath(setDirectory, reinterpret_cast<const char*>(s_manifest + offset + ML::kEntryPath), pathBytes)) return kFailManifestFormat;
                if (fs.Open(s_path, id) != Storage::Error::Ok) return kFailArtifactRead;
                if (fs.Stat(s_path, info) != Storage::Error::Ok) return kFailArtifactRead;
                if (info.directory || info.bytes != dataBytes) return kFailArtifactRead;
                const uint32_t fileBytes = static_cast<uint32_t>(dataBytes);
                Digest::Sha256 artifact;
                artifact.Begin();
                for (uint32_t chunk = 0; chunk < fileBytes; chunk += kBounceBytes) {
                    const uint32_t want = (fileBytes - chunk) < kBounceBytes ? (fileBytes - chunk) : kBounceBytes;
                    if (const Diagnostic reason = ReadChunk(fs, id, chunk, want); reason.code != nullptr) return reason;
                    artifact.Update(s_chunk, want);
                }
                uint8_t actual[Digest::kSha256Bytes];
                artifact.Finish(actual);
                if (memcmp(actual, expectedDigest, sizeof(actual)) != 0) return kFailArtifactDigest;
                // 集内一致：内核镜像的 BuildId 必须等于清单 BuildId（读首块核对数据字段）
                if (type == ML::kTypeOs) {
                    const uint32_t headBytes = fileBytes < Kernel::ImageDataLayout::kBytes ? fileBytes : Kernel::ImageDataLayout::kBytes;
                    if (const Diagnostic reason = ReadChunk(fs, id, 0, headBytes); reason.code != nullptr) return reason;
                    Kernel::ImageFacts kernelFacts;
                    if (const char* reason = Kernel::CheckImageData(s_chunk, fileBytes, kernelFacts)) return Diagnostic{kFailManifestFormat.code, reason};
                    if (memcmp(s_chunk + Kernel::ImageDataLayout::kFieldBuildId, s_manifest + ML::kFieldBuildId, ML::kBuildIdBytes) != 0) return kFailManifestBuildId;
                }
                offset += ML::kEntryFixedBytes + pathBytes;
            }

            memcpy(facts.buildId, s_manifest + ML::kFieldBuildId, ML::kBuildIdBytes);
            facts.securityVersion = securityVersion;
            facts.artifactCount = artifactCount;
            facts.manifestBytes = bytes;
            return Diagnostic{};
        }
    }
}
