/* ManifestVerify.hpp
    内核集清单与产物的验证：引导选择器在装载前核对集的完整性与签名

    验证顺序：结构与条目格式、签名区结构、
    签名与吊销判定（吊销先于信任锚查表）、集身份一致、防回滚比较与逐产物摘要；
    签名覆盖计算把签名值 64 字节视作零
    验证同时解析 Modules.list 并做结构核对（单行格式、条目上限、去重、路径命清单 Type=2 条目），
    把清单副本、列表副本与列表产物视图经 ManifestFacts 交给选版、预装与 BootInfo 交接；
    列表是内核侧书写的完整模块加载列表，预装实现按闭包期望集从中筛出要搬运的成员
    只读不写，不改动系统卷；清单、列表副本与列表产物视图由本模块的静态存储承担，交权前一直有效
*/

#pragma once
#include <stdint.h>

#include <Diagnostics.hpp>
#include <Manifest.hpp>
#include <Revocation.hpp>
#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        // 列表条目上限：清单条目上限 32 减去 .os 与 Modules.list 各一条
        constexpr uint32_t kBootListMaxEntries = 30;

        // 列表条目对应的一条清单 .osm 产物：产物下标与预装所需的全部身份事实
        // 数据全部取自验签通过的清单与已核对的列表条目，预装实现不再自行解释清单
        struct ListedArtifact {
            uint32_t artifactIndex;                                               // 清单产物下标（0 起，含 .os 与列表条目）
            uint32_t pathBytes;                                                   // 相对路径字节数（不含结尾空字符）
            uint64_t dataBytes;                                                   // 清单登记的产物字节数
            uint8_t path[KernelSet::ManifestLayout::kEntryPathMaxBytes + 1];      // 集内相对路径，含结尾空字符
            uint8_t digest[KernelSet::ManifestLayout::kEntryDigestBytes];         // 清单登记的内容 SHA-256
        };

        // 清单事实：验证通过后带回，供选版、状态记录、预装与 BootInfo 交接使用
        struct ManifestFacts {
            uint8_t buildId[KernelSet::ManifestLayout::kBuildIdBytes];  // 集标识
            uint8_t manifestDigest[32];                                 // 清单身份摘要
            uint64_t securityVersion;                                   // 清单的安全版本
            uint64_t bootListRevision;                                  // 列表的修订号，即本次选中的模块修订号
            uint16_t moduleMajor;                                       // 列表与 .os 一致的模块主版本
            uint32_t artifactCount;                                     // 产物条目数
            uint32_t manifestBytes;                                     // 清单长度
            // 清单副本：验签时已把签名值 64 字节原地归零，这份缓冲就是交给内核的字节
            // （清单摘要的原像）；内核不重验签名、不重算摘要，只按头部与条目区做结构核对
            const uint8_t* manifestCopy;
            // 列表副本：交权前保持有效，BootInfo 的 bootList 字段指向它
            const uint8_t* bootListCopy;
            uint32_t bootListBytes;                                     // 列表副本字节数
            // 列表产物视图：按列表条目顺序，每条指向清单里逐字节匹配的 .osm 条目
            const ListedArtifact* listedArtifacts;
            uint32_t listedArtifactCount;                               // 列表条目数
            // 清单里恰好一条 .os 条目的相对路径：选择器的装载路径必须用它（受签名覆盖的产物），
            // 不得按固定的文件名另行拼接；路径已在结构检查里核对非空、无 . 与 ..、不越出集目录
            uint8_t kernelPath[KernelSet::ManifestLayout::kEntryPathMaxBytes + 1];
            uint16_t kernelPathBytes;                                   // 上面路径的字节数
        };

        // 清单与列表的来源：当前修订用固定名字，旧修订快照按 Manifest.<rev>.mft /
        // Modules.<rev>.list 取回；成员为空表示按当前修订的固定名字
        struct ManifestSource {
            const char* manifestName;   // 集目录内的清单文件名；空表示 Manifest.mft
            const char* bootListName;   // 集目录内的列表文件名；空表示清单 Type=3 条目登记的路径
        };

        // 验证集目录里的清单与全部产物：结构、签名、BuildId 一致、防回滚与逐产物摘要
        // expectedBuildId 是所选集的标识（目录名与状态记录里的值）；seenSecurityVersion 是
        // 状态记录里的已见最高版本，无记录时取 0
        // revocation 是有效吊销事实（签名吊销记录与状态记录里的持久事实取并集后的结果）：
        // 它的最低允许安全版本是防回滚的下限之一，它的位图里有该清单的签名密钥时一律拒绝
        // source 指定清单与列表的文件名：旧修订快照的内容仍是同一集的一份修订，
        // 清单里 Type=3 条目登记的名字不因快照改名而变，因此列表按 source.bootListName 取文件
        // 返回空 Diagnostic 表示通过
        Diagnostic VerifyKernelSet(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, ManifestFacts& facts, const ManifestSource& source);

        // 当前修订的验证入口：来源固定为 Manifest.mft 与清单登记的列表路径
        inline Diagnostic VerifyKernelSet(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, ManifestFacts& facts) {
            return VerifyKernelSet(fs, setDirectory, expectedBuildId, seenSecurityVersion, revocation, facts, ManifestSource{});
        }

        // 读当前修订列表的固定头并解出修订号：仅供修订选择判断目标修订是否等于当前修订；
        // 这里不做签名校验——内容随后由所选清单的摘要覆盖，路由用的读数不构成信任
        // 读不到、长度越界或固定头格式不符时返回 false（调用方按“当前修订不可知”处理）
        bool ReadBootListRevision(Storage::FileSystem& fs, const char* setDirectory, uint64_t& revision);
    }
}
