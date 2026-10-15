/* ManifestVerify.hpp
    内核集清单与产物的验证：引导选择器在装载前核对集的完整性与签名

    验证顺序按 Docs/Specs/内核集清单与签名.md 第三节：结构、签名、集身份一致、
    防回滚比较与逐产物摘要；签名覆盖按 2.1，签名值 64 字节视作零
    只读不写，不改动系统卷；清单与读缓冲由本模块的静态存储承担
*/

#pragma once
#include <stdint.h>

#include <Diagnostics.hpp>
#include <Manifest.hpp>
#include <Revocation.hpp>
#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        // 清单事实：验证通过后带回，供选版与状态记录使用
        struct ManifestFacts {
            uint8_t buildId[KernelSet::ManifestLayout::kBuildIdBytes];  // 集标识
            uint8_t manifestDigest[32];                                 // 清单身份摘要
            uint64_t securityVersion;                                   // 清单的安全版本
            uint32_t artifactCount;                                     // 产物条目数
            uint32_t manifestBytes;                                     // 清单长度
        };

        // 验证集目录里的清单与全部产物：结构、签名、BuildId 一致、防回滚与逐产物摘要
        // expectedBuildId 是所选集的标识（目录名与状态记录里的值）；seenSecurityVersion 是
        // 状态记录里的已见最高版本，无记录时取 0
        // revocation 是有效吊销事实（签名吊销记录与状态记录里的持久事实取并集后的结果）：
        // 它的最低允许安全版本是防回滚的下限之一，它的位图里有该清单的签名密钥时一律拒绝
        // 返回空 Diagnostic 表示通过
        Diagnostic VerifyKernelSet(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, ManifestFacts& facts);
    }
}
