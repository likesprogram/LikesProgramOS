/* Revocation.cpp
    内核集签名吊销记录的实现：结构检查、签名覆盖、签名者规则与系统卷读取

    签名覆盖规则见 Docs/Specs/内核集清单与签名.md 第五节：签名值 64 字节视作零，
    对整份记录签名；与清单验签同一套零代入办法，区别只在本格式没有变长部分
    验签经回调调用（Packages/Common 不依赖加密实现），公钥由调用方按信任锚表提供
    无运行库环境只用到 memcpy，就地声明；读缓冲是静态存储，不在栈上开
*/

#include <Revocation.hpp>

#include <stddef.h>

// 目标侧无运行库：只用到 memcpy，按最小 C 运行库的约定声明，符号由各阶段提供
extern "C" void* memcpy(void* dest, const void* source, size_t bytes);

namespace Baleen {
    namespace Core {
        namespace Revocation {
            namespace {
                // 读缓冲：一份记录就是全部内容，留出余量
                constexpr uint32_t kReadBufferBytes = 256;
                uint8_t s_record[kReadBufferBytes];

                // 签名值在记录里的绝对偏移：头部之后紧跟签名区
                constexpr uint32_t kSignatureArea = kHeaderBytes;
                constexpr uint32_t kSignatureValueAt = kSignatureArea + kSignatureValue;

                // —— 拒绝原因：静态文本，供调用方在 WARN 诊断里原样引用 ——
                const char* const kReasonLength = "the revocation record isn't the defined length";
                const char* const kReasonFormat = "the revocation record is invalid";
                const char* const kReasonUnreadable = "the revocation record couldn't be read";
                const char* const kReasonUntrusted = "the revocation record signer isn't trusted";
                const char* const kReasonSelfRevoked = "the revocation record signer revokes itself";
                const char* const kReasonSignature = "the revocation record signature doesn't verify";

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

                // 一段字节是否全零
                bool IsZero(const uint8_t* bytes, uint32_t count) {
                    uint8_t any = 0;
                    for (uint32_t i = 0; i < count; ++i) any |= bytes[i];
                    return any == 0;
                }

                // 记录声明的事实：只做读取，可信与否由验签决定
                void ReadFacts(const uint8_t* bytes, Facts& facts) {
                    facts.epoch = Get64(bytes + kFieldEpoch);
                    facts.minimumSecurityVersion = Get64(bytes + kFieldMinimumSecurityVersion);
                    memcpy(facts.revokedKeyMask, bytes + kFieldRevokedKeyMask, kKeyMaskBytes);
                }
            }

            bool IsKeyRevoked(const uint8_t revokedKeyMask[kKeyMaskBytes], uint8_t keyId) {
                if (keyId == 0) return false;
                const uint32_t bit = static_cast<uint32_t>(keyId) - 1;
                return (revokedKeyMask[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) != 0;
            }

            void Merge(Facts& into, const Facts& other) {
                if (other.epoch > into.epoch) into.epoch = other.epoch;
                if (other.minimumSecurityVersion > into.minimumSecurityVersion) into.minimumSecurityVersion = other.minimumSecurityVersion;
                for (uint32_t i = 0; i < kKeyMaskBytes; ++i) into.revokedKeyMask[i] = static_cast<uint8_t>(into.revokedKeyMask[i] | other.revokedKeyMask[i]);
            }

            bool CheckRecord(const uint8_t* bytes, uint32_t length, uint8_t& signerKeyId) {
                if (length != kRecordBytes) return false;
                if (Get32(bytes + kFieldMagic) != kMagic) return false;
                if (Get16(bytes + kFieldVersion) != kVersion) return false;
                if (Get16(bytes + kFieldHeaderBytes) != kHeaderBytes) return false;
                if (Get32(bytes + kFieldBytes) != kRecordBytes) return false;
                if (Get32(bytes + kFieldFlags) != 0) return false;
                // 纪元为零的记录不表达任何吊销策略：拒绝，避免“空记录”成为状态机上的一格
                if (Get64(bytes + kFieldEpoch) == 0) return false;
                if (bytes[kSignatureArea + kSignatureAlgorithm] != kAlgorithmEd25519) return false;
                if (bytes[kSignatureArea + kSignatureKeyId] == 0) return false;
                if (!IsZero(bytes + kSignatureArea + kSignatureReserved, 2)) return false;
                signerKeyId = bytes[kSignatureArea + kSignatureKeyId];
                return true;
            }

            const char* VerifyRecord(uint8_t* bytes, uint32_t length, uint8_t signerKeyId, const uint8_t* publicKey, SignatureVerify verify, Facts& facts) {
                if (publicKey == nullptr || verify == nullptr) return kReasonUntrusted;

                Facts declared;
                ReadFacts(bytes, declared);

                // 签名者规则：自吊销的记录不成立——泄露的密钥不能靠签一份记录把自己留下
                if (IsKeyRevoked(declared.revokedKeyMask, signerKeyId)) return kReasonSelfRevoked;

                // 签名值先取出，该字段归零后整份记录参与验签（与清单 2.1 同一套办法）
                uint8_t signatureValue[kSignatureValueBytes];
                memcpy(signatureValue, bytes + kSignatureValueAt, kSignatureValueBytes);
                for (uint32_t i = 0; i < kSignatureValueBytes; ++i) bytes[kSignatureValueAt + i] = 0;
                if (!verify(publicKey, signatureValue, bytes, length)) return kReasonSignature;

                facts = declared;
                return nullptr;
            }

            LoadResult Load(Storage::FileSystem& fs, const char* path, PublicKeyLookup lookup, SignatureVerify verify, Facts& facts, const char*& reason) {
                Storage::FileId id;
                // 只有“文件不存在”是正常的缺失形态；其它打开失败是存储层读不了，
                // 不能当成“没有记录”静默略过——那会丢掉一条本可生效的吊销限制
                if (const Storage::Error error = fs.Open(path, id); error != Storage::Error::Ok) {
                    if (error == Storage::Error::NotFound) {
                        reason = kReasonFormat;
                        return LoadResult::Missing;
                    }
                    reason = kReasonUnreadable;
                    return LoadResult::Unusable;
                }
                Storage::FileInfo info;
                if (fs.Stat(path, info) != Storage::Error::Ok) {
                    reason = kReasonUnreadable;
                    return LoadResult::Unusable;
                }
                if (info.directory || info.bytes != kRecordBytes) {
                    reason = kReasonLength;
                    return LoadResult::Unusable;
                }
                uint32_t read = 0;
                if (fs.Read(id, 0, s_record, kRecordBytes, read) != Storage::Error::Ok || read != kRecordBytes) {
                    reason = kReasonUnreadable;
                    return LoadResult::Unusable;
                }

                uint8_t signerKeyId = 0;
                if (!CheckRecord(s_record, kRecordBytes, signerKeyId)) {
                    reason = kReasonFormat;
                    return LoadResult::Unusable;
                }
                const uint8_t* publicKey = lookup == nullptr ? nullptr : lookup(signerKeyId);
                if (publicKey == nullptr) {
                    reason = kReasonUntrusted;
                    return LoadResult::Unusable;
                }
                if (const char* failure = VerifyRecord(s_record, kRecordBytes, signerKeyId, publicKey, verify, facts); failure != nullptr) {
                    reason = failure;
                    return LoadResult::Unusable;
                }
                reason = nullptr;
                return LoadResult::Applied;
            }
        }
    }
}
