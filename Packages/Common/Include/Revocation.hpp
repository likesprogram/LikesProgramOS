/* Revocation.hpp
    内核集签名吊销记录（LPRV）：字节表、签名规则与吊销事实的应用

    字节表与签名规则由本文件定义；签名覆盖与清单同构，
    签名值 64 字节视作零后对整份记录签名；宿主工具生成与签名，引导选择器按同一份定义解析与验签
    本文件放在 Packages/Common：引导期选择器与将来的运行期读取方共用同一份定义与判定；
    验签实现在调用方一侧（引导器与内核各自链入自己的 Ed25519），本层只按回调调用，不依赖加密头
*/

#pragma once
#include <stdint.h>

#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        namespace Revocation {
            // 格式标记，字节顺序为 L P R V
            constexpr uint32_t kMagic = 0x5652504Cu;
            // 当前版本
            constexpr uint16_t kVersion = 1;
            // 头部固定长度：到吊销位图结束
            constexpr uint16_t kHeaderBytes = 0x40;
            // 签名区固定长度：算法 1 + 密钥编号 1 + 保留 2 + 签名值 64
            constexpr uint32_t kSignatureAreaBytes = 68;
            // 签名值长度（Ed25519）
            constexpr uint32_t kSignatureValueBytes = 64;
            // 公钥长度（Ed25519）
            constexpr uint32_t kPublicKeyBytes = 32;
            // 记录总长度：本格式没有变长部分
            constexpr uint32_t kRecordBytes = kHeaderBytes + kSignatureAreaBytes;
            // 吊销位图长度：每个 KeyId 一位，KeyId 取值 1..255，位序见 IsKeyRevoked
            constexpr uint32_t kKeyMaskBytes = 32;
            // 签名算法取值：1 为 Ed25519；其他值拒绝，没有“不验证”取值
            constexpr uint8_t kAlgorithmEd25519 = 1;

            // 头部字段偏移，相对记录起点
            constexpr uint32_t kFieldMagic = 0x000;              // 格式标记
            constexpr uint32_t kFieldVersion = 0x004;            // 版本
            constexpr uint32_t kFieldHeaderBytes = 0x006;        // 头部固定长度
            constexpr uint32_t kFieldBytes = 0x008;              // 记录总长度
            constexpr uint32_t kFieldFlags = 0x00C;              // 可选标志，当前不定义
            constexpr uint32_t kFieldEpoch = 0x010;              // 吊销纪元，单调递增
            constexpr uint32_t kFieldMinimumSecurityVersion = 0x018;   // 最低允许的安全版本
            constexpr uint32_t kFieldRevokedKeyMask = 0x020;     // 已吊销签名密钥位图

            // 签名区字段偏移，相对签名区起点；签名区紧随头部
            constexpr uint32_t kSignatureAlgorithm = 0x00;       // 签名算法
            constexpr uint32_t kSignatureKeyId = 0x01;           // 签名密钥编号
            constexpr uint32_t kSignatureReserved = 0x02;        // 保留，全零
            constexpr uint32_t kSignatureValue = 0x04;           // 签名值

            // 按密钥编号取公钥：返回空指针表示该编号不可信
            using PublicKeyLookup = const uint8_t* (*)(uint8_t keyId);

            // 验证签名：公钥 32 字节、签名 64 字节、待验字节任意长度；实现由调用方链入
            using SignatureVerify = bool (*)(const uint8_t* publicKey, const uint8_t* signature, const uint8_t* message, uint32_t messageBytes);

            // 吊销事实：一份记录解出的结论，也是引导状态记录里持久化的同一组字段
            struct Facts {
                uint64_t epoch = 0;                              // 吊销纪元，只增不减
                uint64_t minimumSecurityVersion = 0;             // 记录给出的最低允许安全版本
                uint8_t revokedKeyMask[kKeyMaskBytes] = {};      // 已吊销签名密钥位图
            };

            // 读取结论：只有文件不存在是正常形态；存储层读不了按不可用报 WARN 后继续
            enum class LoadResult : uint8_t {
                Missing,    // 记录文件不存在（NotFound）：正常形态，按“没有记录”处理
                Applied,    // 记录合法并已解出事实，可以用它更新持久事实
                Unusable,   // 文件在但记录不可用，或存储层读不了：长度、结构、签名者规则或签名不成立
            };

            // KeyId 是否在位图里：KeyId k 对应第 k-1 位、字节 k/8 内的第 (k-1)%8 位
            // KeyId 为 0 时返回 false（0 是保留值，不构成吊销）
            bool IsKeyRevoked(const uint8_t revokedKeyMask[kKeyMaskBytes], uint8_t keyId);

            // 并入事实：纪元与最低允许安全版本取较大者、位图按位或
            // 结果只增不减：这是“记录被删除、损坏或回滚成旧纪元也不解除吊销”的落点
            void Merge(Facts& into, const Facts& other);

            // 结构检查并读出签名者编号：标记、版本、头部长度、总长度、保留字段与签名区字段
            // 不验签，也不判断签名者是否可信；通过返回 true 并填出 signerKeyId
            bool CheckRecord(const uint8_t* bytes, uint32_t length, uint8_t& signerKeyId);

            // 验签与应用：要求 CheckRecord 已通过；bytes 可写，签名值先取出再把该字段归零
            // publicKey 是调用方按 signerKeyId 查到的公钥，为空按“签名者不可信”拒绝
            // 签名者出现在本记录的吊销位图里同样拒绝：自吊销的记录不成立
            // 通过返回空指针并填出 facts；否则返回静态原因文本
            const char* VerifyRecord(uint8_t* bytes, uint32_t length, uint8_t signerKeyId, const uint8_t* publicKey, SignatureVerify verify, Facts& facts);

            // 从系统卷读吊销记录并验签：缺文件按 Missing 返回，不改动 facts
            // 不合法的记录按 Unusable 返回并给出原因，调用方报 WARN 后继续，不据此停机
            LoadResult Load(Storage::FileSystem& fs, const char* path, PublicKeyLookup lookup, SignatureVerify verify, Facts& facts, const char*& reason);
        }
    }
}
