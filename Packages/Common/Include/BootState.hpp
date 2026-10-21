/* BootState.hpp
    引导状态记录：内核集选版的持久状态，读写引导状态区域里的八槽环形记录

    记录字节表、槽位布局、写入顺序与掉电语义见 Docs/Specs/Baleen/引导状态记录.md；
    读取取代次最高且可验证的那一个槽位，空区域与损坏分开处理；写入只落在最旧的槽位并读回校验

    区域切成 8 个等长槽位，**每个槽位固定 512 字节**；槽位长度与设备逻辑块解耦，
    一次提交按设备逻辑块落盘：逻辑块不大于槽位时写一个槽位，4Kn（4096 字节块）上
    读—改—写目标槽位所在的整块（本布局下就是整个区域），其余槽位字节原样写回
    提交因此仍只改变一个槽位的内容，被打断时最多毁掉那一个槽位，其余槽位不受影响
    状态区域起点须按 4096 字节对齐（布局描述默认 0x21000 满足），长度至少 4096：
    这样 512 / 1024 / 2048 / 4096 逻辑块下区域都能整段读写，4Kn 上恰好一个逻辑块
    读取取代次最高且可验证的槽位；写入落在"最旧"的那个槽位（代次最低，未写过视为最低），
    当前最新的那个槽位在写的过程中永不被覆盖——这条不变量是掉电安全的全部依据
    槽位环形使用：连续多次断电各毁一个槽位也要毁到第 8 个才会丢掉全部记录（见规格第一节）

    **格式尚未发布，只有一个版本：`kVersion` 为 1，读写都按同一张字段表。** 字段调整直接改本文件与
    Docs/Specs/Baleen/引导状态记录.md 的字节表，不留兼容分支——首次发布前不背兼容包袱；
    读方遇到版本号不是 1 的记录按损坏处理，不猜字段表
    运行期写入方（Updater / Init）必须读—改—写，保留撤销字段与已试次数：
    清掉撤销字段等于把吊销退回到"依赖签名记录文件在场"
*/

#pragma once
#include <stdint.h>

#include <Revocation.hpp>
#include <Storage/BlockDevice.hpp>

namespace Baleen {
    namespace Core {
        namespace BootState {
            // 区域固定 4096 字节
            constexpr uint32_t kAreaBytes = 4096;
            // 单个槽位长度：512 字节，与设备逻辑块大小解耦（4Kn 上按整块读—改—写提交）
            constexpr uint32_t kCopyBytes = 0x200;
            // 槽位数：环形推进，一次提交写一个
            constexpr uint32_t kSlots = kAreaBytes / kCopyBytes;
            // 记录有效长度
            constexpr uint32_t kRecordBytes = 0xF0;
            // 记录版本：格式尚未发布，只有版本 1
            constexpr uint16_t kVersion = 1;
            // 尝试次数上限
            constexpr uint8_t kMaxTries = 15;
            // 默认尝试次数（候选发布时的写入方取用）
            constexpr uint8_t kDefaultTries = 3;
            // 代次上限：所有有效槽位的最高代次到这里就写不进新记录，状态区域必须修复
            static constexpr uint64_t kGenerationMax = 0xFFFFFFFFFFFFFFFFull;

            // 记录必须落在单份槽位之内：这是"一次提交只改变一个槽位"的前提
            static_assert(kRecordBytes <= kCopyBytes, "记录长度超过一个槽位，单次提交只改变一个槽位的前提不成立");

            // 记录状态：与规格第四节一致
            enum class State : uint8_t {
                Confirmed = 1,   // 已知良好集已确认，无待试候选
                Pending = 2,     // 候选已完整落盘，尚未试启动
                Trying = 3,      // 已记录一次尝试，尚未成功确认
                Failed = 4,      // 候选失败
            };

            // 记录内容：读取或写入的一份完整记录
            struct Record {
                uint64_t generation = 0;                // 代次，写入方按所有槽位最大值加一
                State state = State::Confirmed;         // 状态
                uint8_t triesRemaining = 0;             // 本次记录之外还允许的尝试次数
                uint8_t triesDone = 0;                  // 本候选已经试过几次；只增，不参与判定
                uint64_t attemptId = 0;                 // 尝试身份；Trying 必须非零，其他状态必须为零
                uint8_t confirmedBuildId[32] = {};      // 已知良好集标识
                uint8_t confirmedManifestDigest[32] = {};   // 已知良好集清单摘要
                uint8_t candidateBuildId[32] = {};      // 候选集标识
                uint8_t candidateManifestDigest[32] = {};   // 候选集清单摘要
                uint64_t seenSecurityVersion = 0;       // 本设备已见最高安全版本，只增不减
                Revocation::Facts revocation{};         // 已应用的吊销事实，只增不减；运行期写入方必须保留
            };

            // 读取结果分类
            // 读失败与参数非法不能当成“无记录”：那会让选择器按零事实启动，静默削弱防回滚
            enum class ReadResult : uint8_t {
                Ok,           // 有有效记录
                Empty,        // 区域全零：按“无记录”处理（可写介质的出厂形态）
                Corrupt,      // 有写入痕迹但没有一个槽位可用
                NoRegion,     // 没有状态区域（StateOffset 为 0）：按“无记录”处理
                IoError,      // 读状态区域失败：设备事实未知，不许当成“无记录”
                InvalidArea,  // 区域参数非法：长度不足、起点未按块对齐、块大小非法
            };

            // 单个槽位的校验结论（维护与恢复用）
            enum class CopyStatus : uint8_t {
                NotWritten,   // 标记全零：该槽位未写过
                Valid,        // 结构、组合与校验和都通过
                Invalid,      // 写过但不可用
            };

            // 单个槽位的检查结论：状态与代次（不可用时代次为 0）
            struct CopyFacts {
                CopyStatus status = CopyStatus::NotWritten;   // 校验结论
                uint64_t generation = 0;                      // 该槽位记录的代次
            };

            // 槽位级检查结果：维护工具据此报告"哪些槽位坏了"，选版只关心 Read 的分类
            struct Inspection {
                ReadResult result = ReadResult::NoRegion;     // 与 Read 相同的分类（未检查时为无状态区域）
                CopyFacts slots[kSlots]{};                    // 每个槽位的结论
                Record record{};                              // 有效时选中的那份记录
            };

            // 读状态记录：所有槽位都校验，取代次最高且可验证的一个
            ReadResult Read(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, Record& record);

            // 槽位级检查：与 Read 读同一块区域，另外带回每个槽位各自的结论
            Inspection Inspect(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes);

            // 写状态记录：落在最旧的槽位，代次取所有槽位最大值加一，写后刷盘并读回校验
            // writtenGeneration 带回实际写入的代次（Trying 的尝试身份取它）
            // 记录不可写、写失败、刷盘失败或读回校验失败都返回 false；最高代次到 kGenerationMax 时
            // 同样返回 false——那是“状态区域必须修复”的耗尽形态，调用方要能把它与普通写失败分开
            bool Write(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const Record& record, uint64_t& writtenGeneration);

            // 受控恢复：把状态区域全部槽位清零，回到出厂形态（选版按"无记录"直启缺省集）
            // 只写状态区域，不碰系统卷；介质不可写或写后读回不是全零都返回 false
            bool Clear(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes);

            // 受控恢复：用有效记录重建每一个不可用的槽位，把冗余恢复到满
            // 没有可用记录（空区域、全部不可用、或最高代次出现两次）返回 false，不猜测目标集
            bool Repair(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, uint64_t& writtenGeneration);

            // 人工接受：把已见最高安全版本下调到显式接受的值；这是防回滚的唯一例外
            // acceptedVersion 必须低于当前值（抬高由选择器随清单更新自动完成），否则返回 false
            // 没有记录、介质不可写或写不成立都返回 false；装置侧不存在隐式接受开关
            bool AcceptSeenSecurityVersion(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, uint64_t acceptedVersion, uint64_t& writtenGeneration);

            // 发布候选：写 Pending 记录（候选标识、清单摘要与可用尝试次数）——运行期发布方（Updater）的写入步骤
            // 候选集全部文件完整落盘、清单签名与摘要检查通过之后调用；区域为空时按首次写入处理
            // 已有记录时保留已知良好集、已见最高版本与撤销事实，只换候选与剩余次数
            // 剩余次数为 0 或超过上限、候选标识全零、记录损坏、介质不可写或写不成立都返回 false
            bool PublishCandidate(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const uint8_t candidateBuildId[32], const uint8_t candidateManifestDigest[32], uint8_t triesRemaining, uint64_t& writtenGeneration);

            // 提交确认：当前记录仍是 Trying 且 AttemptId 等于收到的值时才写 Confirmed
            // 这是运行期确认方（Init）的写入步骤：候选字段一次写入转成已知良好集字段并清零候选，
            // 已见最高版本、撤销事实与已试次数原样保留（已试次数是只增的审计账，转正不清零）。
            // 记录不是 Trying、AttemptId 不符、没有记录或写不成立都返回 false
            bool ConfirmAttempt(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, uint64_t attemptId, uint64_t& writtenGeneration);
        }
    }
}
