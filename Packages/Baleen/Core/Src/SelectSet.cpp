/* SelectSet.cpp
    内核集选版的实现：集根推导、状态决策、修订来源解析、清单验证接线、两阶段回退与状态写回

    决策顺序：
    损坏记录不猜测目标集；空区域与无状态区按缺省集直启；写不成立时不进行状态转换；
    候选验证失败或尝试次数用尽写 Failed 并回退已知良好集；良好集验证失败返回致命原因
    修订选择：记录的 ModuleRevision 是回退目标修订号，当前列表的
    revision 是候选修订号；目标等于当前修订时读当前清单 / 列表，否则读旧修订快照
    （Manifest.<rev>.mft / Modules.<rev>.list），两者都不成立即该目标不可选——不猜、不跨修订拼接
    两阶段：BeginSetSelection 做决策、验证与候选 Trying 写入；SelectAfterLoadFailure 处理
    装载或预装失败后的回退（写 Failed、回退已知良好集或上一修订、防止重试同一组合）
*/

#include <SelectSet.hpp>

#include <BootState.hpp>
#include <Diagnostics.hpp>
#include <Ed25519.hpp>
#include <LayoutDescriptor.hpp>
#include <Libc.hpp>
#include <ManifestVerify.hpp>
#include <PrintTarget.hpp>
#include <Revocation.hpp>
#include <TrustAnchor.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            // —— 本阶段失败与警告的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailStateCorrupt{BALEEN_STAGE_CODE("69"), "The boot state records are written but unusable: repair them with FixBootState --repair, or drop them with --clear"};
            // 布局描述的内核路径推不出 KernelSets/<BuildId>/ 形态：不再有“旧扁平落点直启”的未验证回退
            constexpr Diagnostic kFailNotKernelSetPath{BALEEN_STAGE_CODE("84"), "The kernel path isn't a kernel set path"};
            // 布局路径里的相对产物路径必须等于清单登记的 .os 条目，否则会“验 A 装 B”
            constexpr Diagnostic kFailLayoutKernelMismatch{BALEEN_STAGE_CODE("85"), "The layout kernel path doesn't match the verified kernel artifact"};
            // 读状态区域失败：设备已见最高版本与持久吊销事实未知，不能当成“无记录”按零事实启动
            constexpr Diagnostic kFailStateRead{BALEEN_STAGE_CODE("89"), "The boot state record can't be read; refusing to continue without the device facts"};
            // 状态区域参数非法：长度不足、起点未按块对齐、块大小非法，布局/介质不满足状态区契约
            constexpr Diagnostic kFailStateArea{BALEEN_STAGE_CODE("90"), "The boot state area is invalid: check StateOffset and StateBytes"};
            // 状态代次耗尽：有效记录已到代次上限，写不进新记录；状态区域只能修复或清除
            constexpr Diagnostic kFailStateGeneration{BALEEN_STAGE_CODE("92"), "The boot state generation is exhausted; repair the state area"};
            // 列表非法：这里的触发点是“旧修订快照的文件名与内容不是同一修订”与修订文件名拼装失败，
            // 列表结构与期望集核对在 ManifestVerify / 预装实现里，编号相同
            constexpr Diagnostic kFailBootList{BALEEN_STAGE_CODE("96"), "The module load list is invalid"};
            constexpr Diagnostic kWarnStateWrite{BALEEN_STAGE_CODE("70"), "The boot state record wasn't written; continuing without a state transition"};
            // 回退目标可能是已知良好集，也可能是布局描述指定的缺省集（从来没有集被确认过时），
            // 所以这两条只说"回退"，具体用了哪个集由随后的 Set= 与 E-CORE-59 给出
            constexpr Diagnostic kWarnCandidate{BALEEN_STAGE_CODE("72"), "The candidate kernel set didn't verify; falling back"};
            constexpr Diagnostic kWarnCandidateTries{BALEEN_STAGE_CODE("73"), "The candidate kernel set has no boot attempts left; falling back"};
            constexpr Diagnostic kWarnRevocation{BALEEN_STAGE_CODE("75"), "The revocation record is unusable; continuing with the persisted revocation facts"};
            constexpr Diagnostic kWarnNoConfirmedSet{BALEEN_STAGE_CODE("59"), "No confirmed set is recorded; starting the set the layout names"};
            // 记录损坏时按介质策略退到缺省集：这条 WARN 要让人看出来"降级启动"而不是正常收场
            constexpr Diagnostic kWarnDegradeWithoutRecord{BALEEN_STAGE_CODE("56"), "The boot state records are unusable; the medium allows starting the set the layout names"};
            // 介质没有刷新原语：这次提交只有设备缓存的保证，掉电可能让它回退
            constexpr Diagnostic kWarnNoFlushPrimitive{BALEEN_STAGE_CODE("71"), "The medium has no flush primitive; a power loss may roll back this boot's state transition"};
            // 候选集在预装阶段失败：写 Failed 后回退已知良好集或上一修订，本次不终止启动；
            // 已知良好集或缺省集预装失败不报本编号，直接按对应 FATAL 编号终止
            constexpr Diagnostic kWarnCandidatePreload{BALEEN_STAGE_CODE("98"), "Preloading the candidate kernel set failed; falling back"};

            // 吊销记录在系统卷里的固定落点：缺文件是正常形态，不是错误
            constexpr const char* kRevocationPath = "/LikesProgram/System/Revocation.rvk";

            // 集根标记：布局描述的内核路径必须落在 KernelSets/ 下，路径里不许有其它未验证形态
            constexpr const char* kSetMarker = "KernelSets/";
            constexpr uint32_t kSetMarkerBytes = 11;

            // 旧修订快照的文件名前缀与后缀：Manifest.<rev>.mft / Modules.<rev>.list
            constexpr char kSnapshotManifestPrefix[] = "Manifest.";
            constexpr char kSnapshotBootListPrefix[] = "Modules.";
            constexpr char kSnapshotManifestSuffix[] = ".mft";
            constexpr char kSnapshotBootListSuffix[] = ".list";

            // 目标修订的清单来源：当前修订（固定文件名）或旧修订快照（带修订号的文件名）
            // 文件名缓冲按 "Manifest." + 20 位十进制 + ".mft" + 结尾空字符留足
            struct RevisionTarget {
                uint64_t revision;          // 目标修订号；0 表示按当前列表的 revision 解释
                bool snapshot;              // 是否使用旧修订快照文件
                char manifestName[40];      // 快照清单文件名；当前修订时为空
                char bootListName[40];      // 快照列表文件名；当前修订时为空
            };

            // text 是否以 pattern 开头
            bool Matches(const char* text, const char* pattern) {
                for (uint32_t i = 0; pattern[i] != '\0'; ++i) {
                    if (text[i] == '\0' || text[i] != pattern[i]) return false;
                }
                return true;
            }

            // 在缺省集路径里找 "KernelSets/"，集根取到它结尾；找不到返回 false
            // 调用方把 false 当作布局路径形态非法处理，不存在“旧扁平落点直启”的分支
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

            // 集内产物路径：集目录 + "/" + 清单登记的相对路径；长度不够返回 false
            // 相对路径来自验签通过的清单 .os 条目，调用方不得改成固定文件名
            bool BuildKernelPath(const char* setDirectory, const uint8_t* relative, uint32_t relativeBytes, char* out) {
                uint32_t length = strlen(setDirectory);
                if (length + 1 + relativeBytes + 1 > kSetPathBytes) return false;
                memcpy(out, setDirectory, length);
                out[length++] = '/';
                memcpy(out + length, relative, relativeBytes);
                length += relativeBytes;
                out[length] = '\0';
                return true;
            }

            // 十进制无符号 64 位转文本：修订号拼文件名用
            // 目标侧没有 64 位除法运行库，按 10 的幂做减法展开（每位最多 9 次循环）
            bool Decimal(uint64_t value, char* out, uint32_t cap) {
                static const uint64_t kPow10[20] = {
                    1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull,
                    100000000ull, 1000000000ull, 10000000000ull, 100000000000ull, 1000000000000ull,
                    10000000000000ull, 100000000000000ull, 1000000000000000ull, 10000000000000000ull,
                    100000000000000000ull, 1000000000000000000ull, 10000000000000000000ull,
                };
                uint32_t length = 0;
                bool started = false;
                for (int32_t place = 19; place >= 0; --place) {
                    uint8_t digit = 0;
                    while (value >= kPow10[place]) {
                        value -= kPow10[place];
                        ++digit;
                    }
                    if (!started && digit == 0 && place != 0) continue;
                    if (length + 1 >= cap) return false;
                    out[length++] = static_cast<char>('0' + digit);
                    started = true;
                }
                out[length] = '\0';
                return true;
            }

            // 十六进制字符转值
            int HexValue(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            }

            // 拼一个带修订号的文件名：prefix + 十进制修订号 + suffix
            bool CopyRevisionName(const char* prefix, const char* suffix, uint64_t revision, char* out, uint32_t cap) {
                const uint32_t prefixBytes = strlen(prefix);
                const uint32_t suffixBytes = strlen(suffix);
                char digits[21] = {};
                if (!Decimal(revision, digits, sizeof(digits))) return false;
                const uint32_t digitBytes = strlen(digits);
                if (prefixBytes + digitBytes + suffixBytes + 1 > cap) return false;
                memcpy(out, prefix, prefixBytes);
                memcpy(out + prefixBytes, digits, digitBytes);
                memcpy(out + prefixBytes + digitBytes, suffix, suffixBytes + 1);
                return true;
            }

            // 解析目标修订的清单来源：R == 0 或 R == 当前列表 revision 时
            // 取当前 Manifest.mft / Modules.list，否则取 Manifest.<R>.mft / Modules.<R>.list
            // 当前列表读不到而 R != 0 时按旧修订快照走——两条来源都不可用时由验证失败收口；
            // 文件名拼装失败（十进制上限 20 位，正常不会发生）返回 false，该目标不可选
            bool ResolveRevisionSource(Storage::FileSystem& fs, const char* setDirectory, uint64_t revision, RevisionTarget& target, ManifestSource& source) {
                target = RevisionTarget{};
                target.revision = revision;
                source = ManifestSource{};
                // 旧记录（字段为零）按“无修订”处理：行为与扩展前一致，取当前修订的清单与列表
                if (revision == 0) return true;
                uint64_t currentRevision = 0;
                if (ReadBootListRevision(fs, setDirectory, currentRevision) && currentRevision == revision) return true;
                if (!CopyRevisionName(kSnapshotManifestPrefix, kSnapshotManifestSuffix, revision, target.manifestName, sizeof(target.manifestName))) return false;
                if (!CopyRevisionName(kSnapshotBootListPrefix, kSnapshotBootListSuffix, revision, target.bootListName, sizeof(target.bootListName))) return false;
                target.snapshot = true;
                source.manifestName = target.manifestName;
                source.bootListName = target.bootListName;
                return true;
            }

            // 按目标修订验证一份集：解析清单来源、验证清单/签名/防回滚/逐产物；
            // 旧修订快照还要核对列表头的修订号就是目标修订号（不信文件名，不跨修订拼接）
            // snapshot 带回是否用了旧修订快照
            Diagnostic VerifySetForRevision(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, uint64_t revision, ManifestFacts& facts, bool& snapshot) {
                RevisionTarget target;
                ManifestSource source;
                if (!ResolveRevisionSource(fs, setDirectory, revision, target, source)) return kFailBootList;
                const Diagnostic reason = VerifyKernelSet(fs, setDirectory, expectedBuildId, seenSecurityVersion, revocation, facts, source);
                if (reason.code != nullptr) return reason;
                if (target.snapshot && facts.bootListRevision != revision) return kFailBootList;
                snapshot = target.snapshot;
                return Diagnostic{};
            }

            // 标识是否非全零
            bool HasAny(const uint8_t* bytes, uint32_t count) {
                uint8_t any = 0;
                for (uint32_t i = 0; i < count; ++i) any = static_cast<uint8_t>(any | bytes[i]);
                return any != 0;
            }

            // 从交接事实的原始字段还原吊销事实：Revocation::Facts 是非平凡类型（有默认成员
            // 初始化器），不能进全局 SelectedSet——那会让 BaleenCore 产生 .init_array；
            // 交接事实按原始字段存放，这里按需还原成选择器内部使用的 Facts
            Revocation::Facts RevocationFactsOf(const HandoffFacts& handoff) {
                Revocation::Facts facts;
                facts.epoch = handoff.revocationEpoch;
                facts.minimumSecurityVersion = handoff.revocationFloor;
                memcpy(facts.revokedKeyMask, handoff.revokedKeyMask, Revocation::kKeyMaskBytes);
                return facts;
            }

            // 用验证通过的事实填选版结果：装载路径取清单登记的 .os 条目，集标识、修订号与
            // 交接事实一并带回；snapshot 表示这份清单来自旧修订快照
            Diagnostic FillSelected(const char* setDirectory, const ManifestFacts& facts, bool snapshot, uint64_t attemptId, const Revocation::Facts& revocation, SelectedSet& selected) {
                if (!BuildKernelPath(setDirectory, facts.kernelPath, facts.kernelPathBytes, selected.kernelPath)) return kFailNotKernelSetPath;
                const uint32_t length = strlen(setDirectory);
                if (length + 1 > kSetPathBytes) return kFailNotKernelSetPath;
                memcpy(selected.setDirectory, setDirectory, length + 1);
                memcpy(selected.buildId, facts.buildId, sizeof(selected.buildId));
                selected.verified = true;
                selected.attemptId = attemptId;
                selected.moduleRevision = facts.bootListRevision;
                selected.snapshot = snapshot;
                selected.handoff.manifest = facts;
                // 吊销事实按原始字段带出：SelectedSet 必须是平凡可默认构造的 POD，
                // 否则引导侧全局 s_selected 会产生 .init_array（BaleenCore 链接布局不允许）
                selected.handoff.revocationEpoch = revocation.epoch;
                selected.handoff.revocationFloor = revocation.minimumSecurityVersion;
                memcpy(selected.handoff.revokedKeyMask, revocation.revokedKeyMask, Revocation::kKeyMaskBytes);
                return Diagnostic{};
            }

            // 提交状态记录的结果：代次耗尽与普通写失败必须分开，前者要停机而不是按降级路径继续
            enum class CommitResult : uint8_t {
                Ok,                   // 已提交
                WriteFailed,          // 写不成立：报 WARN 后按降级路径继续
                GenerationExhausted,  // 所有有效槽位都到代次上限：状态区域必须修复
            };

            // 写失败后是否属于代次耗尽：与 BootState::Write 的判定顺序一致，可写性先于代次，
            // 只读介质上的写失败是降级形态而不是耗尽；参数非法或读失败时无从判断，按普通写失败处理
            bool GenerationExhausted(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes) {
                if (!medium.Writable()) return false;
                const BootState::Inspection inspection = BootState::Inspect(medium, stateOffset, stateBytes);
                for (uint32_t slot = 0; slot < BootState::kSlots; ++slot) {
                    if (inspection.slots[slot].status != BootState::CopyStatus::Valid) continue;
                    if (inspection.slots[slot].generation == BootState::kGenerationMax) return true;
                }
                return false;
            }

            // 提交状态记录：所有状态转换的唯一写入口
            // 写成功后若介质没有覆盖本次写入的刷新原语，报一条持久化边界——
            // 那次提交可能只落在设备缓存里，掉电会让它回退（回退一次转换，不损坏记录）
            // 代次耗尽必须由调用方停机：不修复状态区域就再也写不进新记录
            CommitResult CommitState(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const BootState::Record& record, uint64_t& generation) {
                // 代次耗尽先于写判定：只读介质上的全一代有效记录也必须停机，
                // 不能因为“写不进去”就退化成普通写失败继续启动
                if (record.generation == BootState::kGenerationMax) return CommitResult::GenerationExhausted;
                if (BootState::Write(medium, stateOffset, stateBytes, record, generation)) {
                    if (!medium.HasFlushPrimitive()) WriteDiagnostic(PrintTargets::Tag::Warn, kWarnNoFlushPrimitive);
                    return CommitResult::Ok;
                }
                if (GenerationExhausted(medium, stateOffset, stateBytes)) return CommitResult::GenerationExhausted;
                return CommitResult::WriteFailed;
            }

            // 从布局描述的内核路径里拆出缺省集：路径必须是 <setRoot><BuildId 64 位十六进制>/<相对产物路径>
            // 相对产物路径由清单里的 .os 条目决定（可以在子目录）；形态不符一律返回 false，不做猜测
            bool ParseDefaultArtifact(const char* setRoot, const char* kernelPath, uint8_t buildId[32], const char*& relative, uint32_t& relativeBytes) {
                if (!Matches(kernelPath, setRoot)) return false;
                const char* cursor = kernelPath + strlen(setRoot);
                for (uint32_t i = 0; i < 64; ++i) {
                    const int value = HexValue(cursor[i]);
                    if (value < 0) return false;
                    if (i % 2 == 0) buildId[i / 2] = static_cast<uint8_t>(value << 4);
                    else buildId[i / 2] = static_cast<uint8_t>(buildId[i / 2] | value);
                }
                if (cursor[64] != '/') return false;
                relative = cursor + 65;
                relativeBytes = strlen(relative);
                return relativeBytes != 0;
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

            // 缺省集：布局路径必须是 KernelSets/<BuildId>/<相对产物路径>，清单验证通过后
            // 装载路径取清单登记的 .os 条目；布局路径里的相对部分必须与它逐字节一致
            // seenSecurityVersion 是设备已见最高版本：没有记录时取 0（只做内嵌下限检查），
            // 有记录的回退路径必须把记录里的值传下来，缺省集同样参与防回滚比较
            // revocation 是有效吊销事实：缺省集与其它集一样受吊销与清单验签约束
            // base 是回退前的有效记录（没有记录时为 nullptr）：回退保留它的状态并写回事实；
            // 没有记录时以 Confirmed 记录把验证过的缺省集钉为已知良好集，ModuleRevision 取本次
            // 列表的 revision（与出厂首次启动同一口径；Clear 之后的重建也走这条）
            Diagnostic SelectDefault(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes,
                                     uint64_t seenSecurityVersion, const char* setRoot, const char* layoutKernelPath,
                                     const Revocation::Facts& revocation, const BootState::Record* base, SelectedSet& selected) {
                uint8_t buildId[32] = {};
                const char* relative = nullptr;
                uint32_t relativeBytes = 0;
                if (!ParseDefaultArtifact(setRoot, layoutKernelPath, buildId, relative, relativeBytes)) return kFailNotKernelSetPath;
                char setDirectory[kSetPathBytes] = {};
                BuildSetDirectory(setRoot, buildId, setDirectory);
                ManifestFacts facts;
                const Diagnostic reason = VerifyKernelSet(fs, setDirectory, buildId, seenSecurityVersion, revocation, facts);
                if (reason.code != nullptr) return reason;
                // 装载路径只认清单登记的 .os：布局路径与它不一致就不能装载，否则会“验 A 装 B”
                if (facts.kernelPathBytes != relativeBytes || memcmp(facts.kernelPath, relative, relativeBytes) != 0) return kFailLayoutKernelMismatch;
                if (const Diagnostic filled = FillSelected(setDirectory, facts, false, 0, revocation, selected); filled.code != nullptr) return filled;

                // 回退到缺省集不等于放弃已见版本与吊销并集：有记录时保持状态、只抬升 seen 与并入
                // 吊销并集；没有记录时用一条 Confirmed 记录把验证过的缺省集钉为已知良好集，同时保存
                // seen、吊销并集与当前列表修订。介质不可写或没有状态区域时不写；写不成立只报 WARN，
                // 本次启动照常；代次耗尽则必须停机——缺省集启动同样不能让状态区域停在不可写状态
                if (!medium.Writable() || stateOffset == 0) return Diagnostic{};
                BootState::Record next;
                bool write = false;
                if (base != nullptr) {
                    next = *base;
                    if (facts.securityVersion > next.seenSecurityVersion) {
                        next.seenSecurityVersion = facts.securityVersion;
                        write = true;
                    }
                    if (FactsAdvanced(revocation, next.revocation)) {
                        next.revocation = revocation;
                        write = true;
                    }
                } else {
                    bool any = facts.securityVersion != 0 || revocation.epoch != 0 || revocation.minimumSecurityVersion != 0;
                    for (uint32_t i = 0; i < Revocation::kKeyMaskBytes; ++i) any = any || revocation.revokedKeyMask[i] != 0;
                    if (any) {
                        next = BootState::Record{};
                        next.state = BootState::State::Confirmed;
                        memcpy(next.confirmedBuildId, facts.buildId, 32);
                        memcpy(next.confirmedManifestDigest, facts.manifestDigest, 32);
                        next.seenSecurityVersion = facts.securityVersion;
                        next.revocation = revocation;
                        // 新建记录按当前列表修订写 ModuleRevision：这是“回退目标修订号”，
                        // 出厂首次启动与 Clear 之后的重建走同一条
                        next.moduleRevision = facts.bootListRevision;
                        write = true;
                    }
                }
                if (write) {
                    uint64_t generation = 0;
                    const CommitResult commit = CommitState(medium, stateOffset, stateBytes, next, generation);
                    if (commit == CommitResult::GenerationExhausted) return kFailStateGeneration;
                    if (commit == CommitResult::WriteFailed) WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                }
                return Diagnostic{};
            }

            // 回退已知良好集：写 Failed（可选）后选良好集，没有良好集时启动缺省集
            Diagnostic SelectFallback(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* setRoot, const char* layoutKernelPath, const Revocation::Facts& revocation, const BootState::Record& record, SelectedSet& selected, bool writeFailed);

            // 已知良好集：按记录里的回退目标修订解析清单来源并验证；已见版本抬升或吊销事实
            // 前进时写回同状态记录（ModuleRevision 与状态一起原样保留，不因写回而改动）
            Diagnostic SelectConfirmed(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* setRoot, const Revocation::Facts& revocation, const BootState::Record& record, SelectedSet& selected) {
                char setDirectory[kSetPathBytes] = {};
                BuildSetDirectory(setRoot, record.confirmedBuildId, setDirectory);
                ManifestFacts facts;
                bool snapshot = false;
                const Diagnostic reason = VerifySetForRevision(fs, setDirectory, record.confirmedBuildId, record.seenSecurityVersion, revocation, record.moduleRevision, facts, snapshot);
                if (reason.code != nullptr) return reason;
                if (const Diagnostic filled = FillSelected(setDirectory, facts, snapshot, 0, revocation, selected); filled.code != nullptr) return filled;
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
                    const CommitResult commit = CommitState(medium, stateOffset, stateBytes, next, generation);
                    if (commit == CommitResult::GenerationExhausted) return kFailStateGeneration;
                    if (commit == CommitResult::WriteFailed) WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
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
                // 候选就是集目录里当前修订的清单与列表：候选修订号从列表头解析，
                // 不写记录字段（ModuleRevision 只描述已知良好侧的退目标）
                ManifestFacts facts;
                const Diagnostic reason = VerifyKernelSet(fs, candidateDirectory, record.candidateBuildId, record.seenSecurityVersion, revocation, facts);
                if (reason.code != nullptr) {
                    // 候选验证失败：记录候选失败，回退良好集
                    WriteDiagnostic(PrintTargets::Tag::Warn, kWarnCandidate, reason.text);
                    return SelectFallback(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected, true);
                }

                // 候选通过：先填选版结果，再写 Trying（剩余次数减一），尝试身份取写入代次；
                // 写不成立时不用已填的结果，按降级路径回退
                SelectedSet candidateSelected;
                if (const Diagnostic filled = FillSelected(candidateDirectory, facts, false, 0, revocation, candidateSelected); filled.code != nullptr) return filled;
                BootState::Record next = record;
                next.state = BootState::State::Trying;
                next.triesRemaining = static_cast<uint8_t>(record.triesRemaining - 1);
                // 已试次数只增：断电丢掉一次提交时它不会倒退，排障时能看出“这一版试过几次”
                next.triesDone = record.triesDone == 255 ? 255 : static_cast<uint8_t>(record.triesDone + 1);
                next.attemptId = 0;   // 由写入代次填出
                next.revocation = revocation;   // 状态转换时一并固化吊销事实
                if (facts.securityVersion > record.seenSecurityVersion) next.seenSecurityVersion = facts.securityVersion;
                uint64_t generation = 0;
                const CommitResult commit = CommitState(medium, stateOffset, stateBytes, next, generation);
                if (commit == CommitResult::GenerationExhausted) return kFailStateGeneration;
                if (commit == CommitResult::WriteFailed) {
                    WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                    return SelectFallback(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected, false);
                }
                candidateSelected.attemptId = generation;
                selected = candidateSelected;
                return Diagnostic{};
            }

            // 已知良好集：有良好集就验证并选它，没有（全零）就启动布局描述指定的缺省集
            // 这条规则只有一个判定：候选失败回退与 Failed 稳态都要走它——出厂介质上第一次更新就失败时
            // 记录里从来没有良好集，若此时按“良好集”去开 /KernelSets/000…/ 会把整台机器锁死
            Diagnostic SelectGoodSet(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* setRoot, const char* layoutKernelPath, const Revocation::Facts& revocation, const BootState::Record& record, SelectedSet& selected) {
                // 没有已知良好集时启动缺省集：把设备已见最高版本一起传下去，缺省集也受防回滚比较约束；
                // 缺省集验证通过后把事实写回同状态记录
                if (!HasAny(record.confirmedBuildId, 32)) {
                    WriteDiagnostic(PrintTargets::Tag::Warn, kWarnNoConfirmedSet);
                    return SelectDefault(fs, medium, stateOffset, stateBytes, record.seenSecurityVersion, setRoot, layoutKernelPath, revocation, &record, selected);
                }
                return SelectConfirmed(fs, medium, stateOffset, stateBytes, setRoot, revocation, record, selected);
            }

            // 回退已知良好集：写 Failed（可选）后验证并选择良好集；没有良好集时启动缺省集
            // 写 Failed 成功时后续按新记录走，避免已见版本抬升写回候选状态
            // 没有可回退修订（ModuleRevision == 0 且当前列表不可用）时
            // 按候选失败处理，最终落到布局描述指定的缺省集，不在已知良好集上硬试
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
                    const CommitResult commit = CommitState(medium, stateOffset, stateBytes, next, generation);
                    if (commit == CommitResult::GenerationExhausted) return kFailStateGeneration;
                    if (commit == CommitResult::Ok) current = next;
                    else WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                }
                // 已知良好集存在但回退目标解析不出当前列表的修订：按“没有可回退修订”落到缺省集
                if (HasAny(current.confirmedBuildId, 32) && current.moduleRevision == 0) {
                    char confirmedDirectory[kSetPathBytes] = {};
                    BuildSetDirectory(setRoot, current.confirmedBuildId, confirmedDirectory);
                    uint64_t currentRevision = 0;
                    if (!ReadBootListRevision(fs, confirmedDirectory, currentRevision)) {
                        WriteDiagnostic(PrintTargets::Tag::Warn, kWarnNoConfirmedSet);
                        return SelectDefault(fs, medium, stateOffset, stateBytes, current.seenSecurityVersion, setRoot, layoutKernelPath, revocation, &current, selected);
                    }
                }
                return SelectGoodSet(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, current, selected);
            }
        }

        Diagnostic BeginSetSelection(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* layoutKernelPath, uint32_t layoutFlags, SelectedSet& selected) {
            selected = SelectedSet{};

            // 吊销记录：系统卷里的签名记录。缺文件是正常形态；不可用只报 WARN 后继续，
            // 理由是记录只能增加限制，忽略它不给攻击者收益，而停机让“写坏一个文件”就能拒绝服务
            Revocation::Facts fromRecord;
            const char* revocationReason = nullptr;
            if (Revocation::Load(fs, kRevocationPath, &Crypto::FindTrustAnchor, &Crypto::Ed25519Verify, fromRecord, revocationReason) == Revocation::LoadResult::Unusable)
                WriteDiagnostic(PrintTargets::Tag::Warn, kWarnRevocation, revocationReason);

            // 集根：从缺省集路径推导；推不出就拒绝——不存在跳过清单验签的直启路径
            char setRoot[kSetPathBytes] = {};
            if (!DeriveSetRoot(layoutKernelPath, setRoot)) return kFailNotKernelSetPath;

            // 状态记录：读失败与参数非法不能当成“无记录”——那会按零事实启动，静默丢掉
            // 设备已见最高版本与持久吊销并集；只有“没有状态区域”与“空区域”才是无记录形态
            BootState::Record record;
            const BootState::ReadResult read = BootState::Read(medium, stateOffset, stateBytes, record);
            if (read == BootState::ReadResult::Corrupt) {
                // 介质可以在布局描述里声明"记录坏了也退到缺省集"（kFlagDegradeWithoutRecord）
                // 那条路要付出两样东西：设备已见最高安全版本与记录里的吊销并集都随记录一起丢了，
                // 因此防回滚只剩内嵌下限与系统卷里的签名吊销记录；所以默认不这么做，
                // 只有介质侧显式声明时才降级，并且必须留下一条 WARN 让降级可见
                if ((layoutFlags & LayoutDescriptor::kFlagDegradeWithoutRecord) == 0) return kFailStateCorrupt;
                WriteDiagnostic(PrintTargets::Tag::Warn, kWarnDegradeWithoutRecord);
                return SelectDefault(fs, medium, stateOffset, stateBytes, 0, setRoot, layoutKernelPath, fromRecord, nullptr, selected);
            }
            // 读失败：设备事实未知，拒绝按零事实启动（否则一次 I/O 错误就把防回滚降一档）
            if (read == BootState::ReadResult::IoError) return kFailStateRead;
            // 参数非法：布局或介质不满足状态区契约，同样不能当成“无记录”
            if (read == BootState::ReadResult::InvalidArea) return kFailStateArea;
            // 没有状态区域与空区域按“无记录”处理：验证缺省集，可写时把看到的事实持久化
            if (read == BootState::ReadResult::NoRegion || read == BootState::ReadResult::Empty)
                return SelectDefault(fs, medium, stateOffset, stateBytes, 0, setRoot, layoutKernelPath, fromRecord, nullptr, selected);
            if (read != BootState::ReadResult::Ok) return kFailStateRead;

            // 持久事实并入签名记录：记录被删除、损坏或回滚成旧纪元都不解除已经落地的吊销
            Revocation::Facts revocation = fromRecord;
            Revocation::Merge(revocation, record.revocation);

            switch (record.state) {
                case BootState::State::Confirmed:
                case BootState::State::Failed:
                    // 两种稳态都走同一条“有良好集就选它、没有就启动缺省集”的规则：
                    // Failed 是没有待试候选的正常收场，出厂介质上可能从来没有良好集
                    return SelectGoodSet(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected);
                case BootState::State::Pending:
                case BootState::State::Trying:
                    return SelectCandidate(fs, medium, stateOffset, stateBytes, setRoot, layoutKernelPath, revocation, record, selected);
                default:
                    return kFailStateCorrupt;
            }
        }

        Diagnostic SelectAfterLoadFailure(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* layoutKernelPath, uint32_t layoutFlags, const SelectedSet& failed, bool preloadFailed, const Diagnostic& failure, SelectedSet& selected) {
            (void)layoutFlags;
            selected = SelectedSet{};

            // 集根与第一阶段同一份推导：推不出就拒绝，不存在未验证的直启回退
            char setRoot[kSetPathBytes] = {};
            if (!DeriveSetRoot(layoutKernelPath, setRoot)) return kFailNotKernelSetPath;

            // 已知良好集（或缺省集）在装载/预装阶段失败：没有可回退的目标，原样返回失败原因终止；
            // 不重试同一组合，也不切换到别的集——“已知良好集也不能提供必需能力”即停止条件
            if (failed.attemptId == 0) return failure;

            // 本次选版合并后的吊销事实：交接事实按原始字段存放（SelectedSet 必须是 POD），
            // 回退写 Failed 与重选良好集时把它固化进记录
            const Revocation::Facts failedRevocation = RevocationFactsOf(failed.handoff);

            // 候选试启动失败：读回记录并把候选判失败；写不成只报 WARN，按降级路径继续回退
            BootState::Record record;
            const BootState::ReadResult read = BootState::Read(medium, stateOffset, stateBytes, record);
            if (read == BootState::ReadResult::Corrupt) return kFailStateCorrupt;
            if (read == BootState::ReadResult::IoError) return kFailStateRead;
            if (read == BootState::ReadResult::InvalidArea) return kFailStateArea;
            // 候选预装失败的回退是 WARN：本次不终止，改选已知良好集或上一修订
            if (preloadFailed) WriteDiagnostic(PrintTargets::Tag::Warn, kWarnCandidatePreload, failure.text);

            bool hasRecord = read == BootState::ReadResult::Ok;
            BootState::Record current = record;
            if (hasRecord) {
                // 只有记录仍是这次试启动的那一份 Trying 时才写 Failed：状态被外部推进过就不覆盖，
                // 避免把一次迟到的失败判定盖到已经确认的记录上
                if (record.state != BootState::State::Trying || record.attemptId != failed.attemptId) {
                    WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                } else {
                    BootState::Record next = record;
                    next.state = BootState::State::Failed;
                    next.attemptId = 0;
                    next.triesRemaining = 0;
                    next.revocation = failedRevocation;   // 状态转换时一并固化吊销事实
                    uint64_t generation = 0;
                    const CommitResult commit = CommitState(medium, stateOffset, stateBytes, next, generation);
                    if (commit == CommitResult::GenerationExhausted) return kFailStateGeneration;
                    if (commit == CommitResult::WriteFailed) WriteDiagnostic(PrintTargets::Tag::Warn, kWarnStateWrite);
                    else current = next;
                }
            }

            // 回退目标：有已知良好集就选它（按记录里的回退目标修订号解析清单来源），
            // 没有就启动布局描述指定的缺省集；解析不出目标的修订时同样落到缺省集
            bool targetConfirmed = HasAny(current.confirmedBuildId, 32);
            uint8_t targetBuildId[32] = {};
            uint64_t targetRevision = 0;
            if (targetConfirmed) {
                memcpy(targetBuildId, current.confirmedBuildId, 32);
                targetRevision = current.moduleRevision;
                if (targetRevision == 0) {
                    // 旧记录没有修订号：按当前列表的 revision 解释；列表读不到就按“没有可回退修订”
                    // 落到缺省集
                    char confirmedDirectory[kSetPathBytes] = {};
                    BuildSetDirectory(setRoot, targetBuildId, confirmedDirectory);
                    if (!ReadBootListRevision(fs, confirmedDirectory, targetRevision)) targetConfirmed = false;
                }
            }
            if (!targetConfirmed) {
                const char* relative = nullptr;
                uint32_t relativeBytes = 0;
                if (!ParseDefaultArtifact(setRoot, layoutKernelPath, targetBuildId, relative, relativeBytes)) return kFailNotKernelSetPath;
                char defaultDirectory[kSetPathBytes] = {};
                BuildSetDirectory(setRoot, targetBuildId, defaultDirectory);
                if (!ReadBootListRevision(fs, defaultDirectory, targetRevision)) targetRevision = 0;
            }

            // 防环：回退目标与失败候选是同一集、同一修订时不重试同一组合
            // 修订号解析不出来时无法证明两者不同，同样按“良好集也不可用”处理，不猜
            if (targetRevision == 0) return failure;
            if (memcmp(targetBuildId, failed.buildId, 32) == 0 && targetRevision == failed.moduleRevision) return failure;

            // 选回退目标：已知良好集走修订解析；缺省集按布局路径验证并写回事实
            if (targetConfirmed) {
                if (const Diagnostic reason = SelectConfirmed(fs, medium, stateOffset, stateBytes, setRoot, failedRevocation, current, selected); reason.code != nullptr) {
                    // 已知良好集自身验证失败：终止，返回它给出的原因（目标不可验证，不是候选的问题）
                    return reason;
                }
                return Diagnostic{};
            }
            return SelectDefault(fs, medium, stateOffset, stateBytes, current.seenSecurityVersion, setRoot, layoutKernelPath, failedRevocation, hasRecord ? &current : nullptr, selected);
        }
    }
}
