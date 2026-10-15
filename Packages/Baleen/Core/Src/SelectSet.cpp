/* SelectSet.cpp
    内核集选版的实现：集根推导、状态决策、清单验证接线与状态写回

    决策顺序按 Docs/Specs/内核集更新与回滚.md 与引导状态记录规格：
    损坏记录不猜测目标集；空区域与无状态区按缺省集直启；写不成立时不进行状态转换；
    候选验证失败或尝试次数用尽写 Failed 并回退已知良好集；良好集验证失败返回致命原因
*/

#include <SelectSet.hpp>

#include <BootState.hpp>
#include <Diagnostics.hpp>
#include <Ed25519.hpp>
#include <Libc.hpp>
#include <ManifestVerify.hpp>
#include <PrintTarget.hpp>
#include <Revocation.hpp>
#include <TrustAnchor.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            // —— 本阶段失败与警告的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailStateCorrupt{"E-CORE-69", "The boot state records are written but unusable"};
            constexpr Diagnostic kWarnStateWrite{"E-CORE-70", "The boot state record wasn't written; continuing without a state transition"};
            constexpr Diagnostic kWarnCandidate{"E-CORE-72", "The candidate kernel set didn't verify; using the confirmed set"};
            constexpr Diagnostic kWarnCandidateTries{"E-CORE-73", "The candidate kernel set has no boot attempts left; using the confirmed set"};
            constexpr Diagnostic kWarnRevocation{"E-CORE-75", "The revocation record is unusable; continuing with the persisted revocation facts"};

            // 吊销记录在系统卷里的固定落点：缺文件是正常形态，不是错误
            constexpr const char* kRevocationPath = "/LikesProgram/System/Revocation.rvk";

            // 集根标记与集内内核产物名：与系统卷包的落点约定一致
            constexpr const char* kSetMarker = "KernelSets/";
            constexpr uint32_t kSetMarkerBytes = 11;
            constexpr const char* kSetKernelName = "LikesProgram.os";

            // text 是否以 pattern 开头
            bool Matches(const char* text, const char* pattern) {
                for (uint32_t i = 0; pattern[i] != '\0'; ++i) {
                    if (text[i] == '\0' || text[i] != pattern[i]) return false;
                }
                return true;
            }

            // 在缺省集路径里找 "KernelSets/"，集根取到它结尾；找不到（旧扁平形态）返回 false
            bool DeriveSetRoot(const char* kernelPath, char* root) {
                for (uint32_t i = 0; kernelPath[i] != '\0'; ++i) {
                    if (!Matches(kernelPath + i, kSetMarker)) continue;
                    const uint32_t length = i + kSetMarkerBytes;
                    if (length + 64 + 16 + 2 > kSetPathBytes) return false;
                    memcpy(root, kernelPath, length);
                    root[length] = '\0';
                    return true;
                }
                return false;
            }

            // 由集根与 BuildId 拼出集目录：集名是 64 位小写十六进制
            void BuildSetDirectory(const char* root, const uint8_t buildId[32], char* out) {
                static const char kDigits[] = "0123456789abcdef";
                uint32_t length = strlen(root);
                memcpy(out, root, length);
                for (uint32_t i = 0; i < 32; ++i) {
                    out[length++] = kDigits[buildId[i] >> 4];
                    out[length++] = kDigits[buildId[i] & 0x0f];
                }
                out[length] = '\0';
            }

            // 集内内核镜像路径：集目录 + "/" + 产物名
            void BuildKernelPath(const char* setDirectory, char* out) {
                uint32_t length = strlen(setDirectory);
                memcpy(out, setDirectory, length);
                out[length++] = '/';
                for (uint32_t i = 0; kSetKernelName[i] != '\0'; ++i) out[length++] = kSetKernelName[i];
                out[length] = '\0';
            }

            // 父目录：去掉最后一个路径分量
            void ParentDirectory(const char* path, char* out) {
                uint32_t length = strlen(path);
                while (length > 1 && path[length - 1] != '/') --length;
                if (length > 1) --length;
                memcpy(out, path, length);
                out[length] = '\0';
            }

            // 十六进制字符转值
            int HexValue(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            }

            // 把路径最后一段按 64 位十六进制解析成 BuildId；失败返回 false
            bool ParseSetName(const char* directory, uint8_t buildId[32]) {
                uint32_t begin = 0;
                for (uint32_t i = 0; directory[i] != '\0'; ++i) {
                    if (directory[i] == '/') begin = i + 1;
                }
                for (uint32_t i = 0; i < 64; ++i) {
                    const int value = HexValue(directory[begin + i]);
                    if (value < 0) return false;
                    if (i % 2 == 0) buildId[i / 2] = static_cast<uint8_t>(value << 4);
                    else buildId[i / 2] = static_cast<uint8_t>(buildId[i / 2] | value);
                }
                return directory[begin + 64] == '\0';
            }

            // 吊销事实是否前进：并集只能更大，任一分量增大就说明需要回写
            bool FactsAdvanced(const Revocation::Facts& merged, const Revocation::Facts& current) {
                if (merged.epoch != current.epoch) return true;
                if (merged.minimumSecurityVersion != current.minimumSecurityVersion) return true;
                for (uint32_t i = 0; i < Revocation::kKeyMaskBytes; ++i) {
                    if ((merged.revokedKeyMask[i] & static_cast<uint8_t>(~current.revokedKeyMask[i])) != 0) return true;
                }
                return false;
            }

            // 缺省集直启：父目录是集目录时按目录名解析 BuildId 并验证清单，否则不验证
            // seenSecurityVersion 是设备已见最高版本：没有记录时取 0（规格第六节第 4 条只做内嵌下限检查），
            // 有记录的回退路径必须把记录里的值传下来，缺省集同样参与防回滚比较
            // revocation 是有效吊销事实：旧扁平形态不做清单验签，吊销在该形态下无从判定
            Diagnostic SelectDefault(Storage::FileSystem& fs, uint64_t seenSecurityVersion, const char* layoutKernelPath, const Revocation::Facts& revocation, SelectedSet& selected) {
                uint32_t length = strlen(layoutKernelPath);
                if (length + 1 > kSetPathBytes) return kFailStateCorrupt;
                memcpy(selected.kernelPath, layoutKernelPath, length + 1);
                ParentDirectory(layoutKernelPath, selected.setDirectory);
                uint8_t buildId[32] = {};
                if (!ParseSetName(selected.setDirectory, buildId)) {
                    // 旧扁平形态：没有集身份，不做清单验证（选版启用前的兼容路径）
                    selected.verified = false;
                    selected.attemptId = 0;
                    return Diagnostic{};
                }
                ManifestFacts facts;
                const Diagnostic reason = VerifyKernelSet(fs, selected.setDirectory, buildId, seenSecurityVersion, revocation, facts);
                if (reason.code != nullptr) return reason;
                memcpy(selected.buildId, facts.buildId, sizeof(selected.buildId));
                selected.verified = true;
                selected.attemptId = 0;
                return Diagnostic{};
            }

            // 回退已知良好集：写 Failed（可选）后选良好集，没有良好集时启动缺省集
            Diagnostic SelectFallback(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* setRoot, const char* layoutKernelPath, const Revocation::Facts& revocation, const BootState::Record& record, SelectedSet& selected, bool writeFailed);

            // 已知良好集：验证清单后填充结果；已见版本抬升或吊销事实前进时写回同状态记录
            Diagnostic SelectConfirmed(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* setRoot, const Revocation::Facts& revocation, const BootState::Record& record, SelectedSet& selected) {
                BuildSetDirectory(setRoot, record.confirmedBuildId, selected.setDirectory);
                BuildKernelPath(selected.setDirectory, selected.kernelPath);
                ManifestFacts facts;
                const Diagnostic reason = VerifyKernelSet(fs, selected.setDirectory, record.confirmedBuildId, record.seenSecurityVersion, revocation, facts);
                if (reason.code != nullptr) return reason;
                memcpy(selected.buildId, facts.buildId, sizeof(selected.buildId));
                selected.verified = true;
                selected.attemptId = 0;
                // 已见版本抬升与吊销事实前进都只改记录内容，不改状态；两者都不成立时不写
                BootState::Record next = record;
                bool write = false;
                if (facts.securityVersion > record.seenSecurityVersion) {
                    next.seenSecurityVersion = facts.securityVersion;
                    write = true;
                }
                if (FactsAdvanced(revocation, record.revocation)) {
                    next.revocation = revocation;
                    write = true;
                }
                if (write) {
                    uint64_t generation = 0;
                    if (!BootState::Write(medium, stateOffset, stateBytes, next, generation))
                        WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                }
                return Diagnostic{};
            }

            // 候选集：验证清单后写 Trying 并选候选；次数用尽或验证失败写 Failed 并回退
            // 写不成立时不进行状态转换，按降级路径选已知良好集
            Diagnostic SelectCandidate(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* setRoot, const char* layoutKernelPath, const Revocation::Facts& revocation, const BootState::Record& record, SelectedSet& selected) {
                // 次数用尽：Trying 的重试耗尽，或 Pending 带回的可用次数为零；写 Failed 后回退
                if (record.triesRemaining == 0) {
                    WriteDiagnostic(PrintTargets::Tag::Warn, kWarnCandidateTries);
                    return SelectFallback(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected, true);
                }

                char candidateDirectory[kSetPathBytes] = {};
                BuildSetDirectory(setRoot, record.candidateBuildId, candidateDirectory);
                ManifestFacts facts;
                const Diagnostic reason = VerifyKernelSet(fs, candidateDirectory, record.candidateBuildId, record.seenSecurityVersion, revocation, facts);
                if (reason.code != nullptr) {
                    // 候选验证失败：记录候选失败，回退良好集
                    WriteDiagnostic(PrintTargets::Tag::Warn, kWarnCandidate, reason.text);
                    return SelectFallback(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected, true);
                }

                // 候选通过：写 Trying（剩余次数减一），尝试身份取写入代次
                BootState::Record next = record;
                next.state = BootState::State::Trying;
                next.triesRemaining = static_cast<uint8_t>(record.triesRemaining - 1);
                next.attemptId = 0;   // 由写入代次填出
                next.revocation = revocation;   // 状态转换时一并固化吊销事实
                if (facts.securityVersion > record.seenSecurityVersion) next.seenSecurityVersion = facts.securityVersion;
                uint64_t generation = 0;
                if (!BootState::Write(medium, stateOffset, stateBytes, next, generation)) {
                    WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                    return SelectFallback(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected, false);
                }
                uint32_t length = strlen(candidateDirectory);
                memcpy(selected.setDirectory, candidateDirectory, length + 1);
                BuildKernelPath(candidateDirectory, selected.kernelPath);
                memcpy(selected.buildId, facts.buildId, sizeof(selected.buildId));
                selected.verified = true;
                selected.attemptId = generation;
                return Diagnostic{};
            }

            // 回退已知良好集：写 Failed（可选）后验证并选择良好集；没有良好集时启动缺省集
            // 写 Failed 成功时后续按新记录走，避免已见版本抬升写回候选状态
            Diagnostic SelectFallback(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* setRoot, const char* layoutKernelPath, const Revocation::Facts& revocation, const BootState::Record& record, SelectedSet& selected, bool writeFailed) {
                // 候选失败先落记录：没有已知良好集时也要留下 Failed，等新的 Pending 才会再试
                BootState::Record current = record;
                if (writeFailed) {
                    BootState::Record next = record;
                    next.state = BootState::State::Failed;
                    next.attemptId = 0;
                    next.triesRemaining = 0;
                    next.revocation = revocation;
                    uint64_t generation = 0;
                    if (BootState::Write(medium, stateOffset, stateBytes, next, generation)) current = next;
                    else WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                }
                uint8_t any = 0;
                for (uint32_t i = 0; i < 32; ++i) any |= record.confirmedBuildId[i];
                // 没有已知良好集时启动缺省集：把设备已见最高版本一起传下去，缺省集也受防回滚比较约束
                if (any == 0) return SelectDefault(fs, current.seenSecurityVersion, layoutKernelPath, revocation, selected);
                return SelectConfirmed(fs, medium, stateOffset, stateBytes, setRoot, revocation, current, selected);
            }
        }

        Diagnostic SelectKernelSet(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* layoutKernelPath, SelectedSet& selected) {
            selected = SelectedSet{};

            // 吊销记录：系统卷里的签名记录。缺文件是正常形态；不可用只报 WARN 后继续，
            // 理由是记录只能增加限制，忽略它不给攻击者收益，而停机让“写坏一个文件”就能拒绝服务
            Revocation::Facts fromRecord;
            const char* revocationReason = nullptr;
            if (Revocation::Load(fs, kRevocationPath, &Crypto::FindTrustAnchor, &Crypto::Ed25519Verify, fromRecord, revocationReason) == Revocation::LoadResult::Unusable)
                WriteDiagnostic(PrintTargets::Tag::Warn, kWarnRevocation, revocationReason);

            // 集根：从缺省集路径推导；没有集根（旧扁平形态）时只直启缺省集
            char setRoot[kSetPathBytes] = {};
            if (!DeriveSetRoot(layoutKernelPath, setRoot)) return SelectDefault(fs, 0, layoutKernelPath, fromRecord, selected);

            // 状态记录：损坏不猜测目标集；空区域或没有状态区按缺省集直启
            BootState::Record record;
            const BootState::ReadResult read = BootState::Read(medium, stateOffset, stateBytes, record);
            if (read == BootState::ReadResult::Corrupt) return kFailStateCorrupt;
            if (read != BootState::ReadResult::Ok) return SelectDefault(fs, 0, layoutKernelPath, fromRecord, selected);

            // 持久事实并入签名记录：记录被删除、损坏或回滚成旧纪元都不解除已经落地的吊销
            Revocation::Facts revocation = fromRecord;
            Revocation::Merge(revocation, record.revocation);

            switch (record.state) {
                case BootState::State::Confirmed:
                case BootState::State::Failed:
                    return SelectConfirmed(fs, medium, stateOffset, stateBytes, setRoot, revocation, record, selected);
                case BootState::State::Pending:
                case BootState::State::Trying:
                    return SelectCandidate(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected);
                default:
                    return kFailStateCorrupt;
            }
        }
    }
}
