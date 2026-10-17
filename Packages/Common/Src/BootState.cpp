/* BootState.cpp
    引导状态记录的实现：槽位校验、最高代次选择、最旧槽位写入、刷盘、读回校验与受控恢复

    记录字节表与写入顺序见 Docs/Specs/Baleen/引导状态记录.md 第二、四节；
    校验用 CRC-32（IEEE 802.3 参数），整数按小端读写，保留字段必须为零
    **格式尚未发布，只有一个版本 1**：读写用同一张字段表，不留兼容分支；
    版本号不是 1 的记录按损坏处理，不猜字段表
    一次提交只写一份副本（一个 512 字节扇区），提交因此落在设备的单个写单元里
    本文件放在 Packages/Common：引导期选择器与运行期写入方（Updater / Init）用同一份实现，
    "同一后端、同一格式"因此是一份代码而不是两份约定；无运行库环境只用到 memcpy，就地声明
*/

#include <BootState.hpp>

#include <stddef.h>

// 目标侧无运行库：只用到 memcpy，按最小 C 运行库的约定声明，符号由各阶段提供
extern "C" void* memcpy(void* dest, const void* source, size_t bytes);

namespace Baleen {
    namespace Core {
        namespace BootState {
            namespace {
                // 记录格式：标记“BLST”，版本 1 的 0xF0 字段表；字段偏移与规格第二节逐条对应
                constexpr uint32_t kMagic = 0x54534C42u;
                // 副本内字段偏移
                constexpr uint32_t kFieldMagic = 0x000;
                constexpr uint32_t kFieldVersion = 0x004;
                constexpr uint32_t kFieldBytes = 0x006;
                constexpr uint32_t kFieldGeneration = 0x008;
                constexpr uint32_t kFieldState = 0x010;
                constexpr uint32_t kFieldTries = 0x011;
                constexpr uint32_t kFieldTriesDone = 0x012;
                constexpr uint32_t kFieldFlags = 0x013;
                constexpr uint32_t kFieldReserved = 0x014;
                constexpr uint32_t kFieldAttemptId = 0x018;
                constexpr uint32_t kFieldConfirmedBuildId = 0x020;
                constexpr uint32_t kFieldConfirmedDigest = 0x040;
                constexpr uint32_t kFieldCandidateBuildId = 0x060;
                constexpr uint32_t kFieldCandidateDigest = 0x080;
                constexpr uint32_t kFieldSeenSecurityVersion = 0x0A0;
                constexpr uint32_t kFieldRevocationEpoch = 0x0A8;
                constexpr uint32_t kFieldRevocationFloor = 0x0B0;
                constexpr uint32_t kFieldRevokedKeyMask = 0x0B8;
                constexpr uint32_t kFieldReserved2 = 0x0D8;
                constexpr uint32_t kFieldChecksum = 0x0EC;
                // 保留区长度：两版都是 20 字节
                constexpr uint32_t kReservedBytes = 20;

                // 区域读缓冲：全部槽位一次读完
                uint8_t s_area[kAreaBytes];

                // 记录是否全零：用于区分“未写过”与“写过但不可用”
                bool IsZero(const uint8_t* bytes, uint32_t count) {
                    uint8_t any = 0;
                    for (uint32_t i = 0; i < count; ++i) any |= bytes[i];
                    return any == 0;
                }

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

                // 按小端写入 16 位
                void Put16(uint8_t* field, uint16_t value) {
                    field[0] = static_cast<uint8_t>(value);
                    field[1] = static_cast<uint8_t>(value >> 8);
                }

                // 按小端写入 32 位
                void Put32(uint8_t* field, uint32_t value) {
                    for (uint32_t i = 0; i < 4; ++i) field[i] = static_cast<uint8_t>(value >> (8 * i));
                }

                // 按小端写入 64 位
                void Put64(uint8_t* field, uint64_t value) {
                    for (uint32_t i = 0; i < 8; ++i) field[i] = static_cast<uint8_t>(value >> (8 * i));
                }

                // CRC-32：IEEE 802.3 参数，反射多项式 0xEDB88320，初值全一，结果取反
                uint32_t Crc32(const uint8_t* data, uint32_t bytes) {
                    uint32_t crc = 0xFFFFFFFFu;
                    for (uint32_t i = 0; i < bytes; ++i) {
                        crc ^= data[i];
                        for (uint32_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                    }
                    return ~crc;
                }

                // 字段组合是否与状态一致：不一致的记录视为无效
                bool CombinationValid(const Record& record) {
                    // 尝试次数上限对四个状态都成立，先拦一次；后面的分支只判断与状态相关的组合
                    if (record.triesRemaining > kMaxTries) return false;
                    const bool confirmedZero = IsZero(record.confirmedBuildId, 32);
                    const bool candidateZero = IsZero(record.candidateBuildId, 32);
                    switch (record.state) {
                        case State::Confirmed:
                            return record.attemptId == 0 && !confirmedZero && candidateZero;
                        case State::Pending:
                            return record.attemptId == 0 && !candidateZero;
                        case State::Trying:
                            return record.attemptId != 0 && !candidateZero;
                        case State::Failed:
                            return record.attemptId == 0 && !candidateZero;
                        default:
                            return false;
                    }
                }

                // 单份副本的校验结论
                enum class CopyState : uint8_t {
                    NotWritten,   // 标记全零：该副本未写过
                    Valid,        // 结构、组合与校验和都通过
                    Invalid,      // 写过但不可用
                };

                // 校验一份副本并解出记录：只有一个版本，字段表就是本文件里那一张
                CopyState CheckCopy(const uint8_t* copy, Record& record) {
                    if (IsZero(copy, 4)) return CopyState::NotWritten;
                    if (Get32(copy + kFieldMagic) != kMagic) return CopyState::Invalid;
                    if (Get16(copy + kFieldVersion) != kVersion) return CopyState::Invalid;   // 不是版本 1 一律不猜字段表
                    if (Get16(copy + kFieldBytes) != kRecordBytes) return CopyState::Invalid;
                    if (copy[kFieldFlags] != 0) return CopyState::Invalid;
                    if (Get32(copy + kFieldReserved) != 0) return CopyState::Invalid;
                    if (!IsZero(copy + kFieldReserved2, kReservedBytes)) return CopyState::Invalid;
                    if (Get32(copy + kFieldChecksum) != Crc32(copy, kFieldChecksum)) return CopyState::Invalid;
                    record.revocation = Revocation::Facts{};
                    record.revocation.epoch = Get64(copy + kFieldRevocationEpoch);
                    record.revocation.minimumSecurityVersion = Get64(copy + kFieldRevocationFloor);
                    memcpy(record.revocation.revokedKeyMask, copy + kFieldRevokedKeyMask, Revocation::kKeyMaskBytes);
                    record.generation = Get64(copy + kFieldGeneration);
                    if (record.generation == 0) return CopyState::Invalid;
                    record.state = static_cast<State>(copy[kFieldState]);
                    record.triesRemaining = copy[kFieldTries];
                    record.triesDone = copy[kFieldTriesDone];
                    record.attemptId = Get64(copy + kFieldAttemptId);
                    memcpy(record.confirmedBuildId, copy + kFieldConfirmedBuildId, 32);
                    memcpy(record.confirmedManifestDigest, copy + kFieldConfirmedDigest, 32);
                    memcpy(record.candidateBuildId, copy + kFieldCandidateBuildId, 32);
                    memcpy(record.candidateManifestDigest, copy + kFieldCandidateDigest, 32);
                    record.seenSecurityVersion = Get64(copy + kFieldSeenSecurityVersion);
                    if (!CombinationValid(record)) return CopyState::Invalid;
                    return CopyState::Valid;
                }

                // 读出所有槽位：返回每个槽位的校验结论
                ReadResult ReadSlots(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, Record* records, CopyState* states) {
                    if (stateOffset == 0 || stateBytes < kAreaBytes) return ReadResult::Unavailable;
                    if (medium.Read(stateOffset, s_area, kAreaBytes) != Storage::Error::Ok) return ReadResult::Unavailable;
                    for (uint32_t slot = 0; slot < kSlots; ++slot) states[slot] = CheckCopy(s_area + slot * kCopyBytes, records[slot]);
                    return ReadResult::Ok;
                }

                // 按所有槽位的结论选出记录：取最高代次的那个，最高代次出现两次即视为损坏
                // 没有任何有效槽位时区分"从未写过"（空区域）与"写过但都不可用"（损坏）
                ReadResult SelectRecord(const CopyState* states, const Record* records, Record& record) {
                    uint64_t best = 0;
                    uint32_t bestSlot = 0;
                    uint32_t bestCount = 0;
                    bool wroteAnything = false;
                    for (uint32_t slot = 0; slot < kSlots; ++slot) {
                        if (states[slot] != CopyState::NotWritten) wroteAnything = true;
                        if (states[slot] != CopyState::Valid) continue;
                        if (records[slot].generation > best) {
                            best = records[slot].generation;
                            bestSlot = slot;
                            bestCount = 1;
                        } else if (records[slot].generation == best) {
                            ++bestCount;
                        }
                    }
                    if (bestCount == 0) return wroteAnything ? ReadResult::Corrupt : ReadResult::Empty;
                    // 最高代次出现两次：写方违规或异常损坏，两份都不采用（不猜测）
                    if (bestCount > 1) return ReadResult::Corrupt;
                    record = records[bestSlot];
                    return ReadResult::Ok;
                }

                // 内部结论转成对外结论
                CopyStatus ToStatus(CopyState state) {
                    switch (state) {
                        case CopyState::NotWritten: return CopyStatus::NotWritten;
                        case CopyState::Valid: return CopyStatus::Valid;
                        default: return CopyStatus::Invalid;
                    }
                }

                // 维护操作的整块缓冲：清零与读回确认共用，不在栈上开
                uint8_t s_maintenance[kAreaBytes];
            }

            ReadResult Read(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, Record& record) {
                Record records[kSlots];
                CopyState states[kSlots] = {};
                if (const ReadResult result = ReadSlots(medium, stateOffset, stateBytes, records, states); result != ReadResult::Ok) return result;
                return SelectRecord(states, records, record);
            }

            Inspection Inspect(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes) {
                Inspection inspection;
                Record records[kSlots];
                CopyState states[kSlots] = {};
                inspection.result = ReadSlots(medium, stateOffset, stateBytes, records, states);
                for (uint32_t slot = 0; slot < kSlots; ++slot) {
                    inspection.slots[slot].status = ToStatus(states[slot]);
                    if (states[slot] == CopyState::Valid) inspection.slots[slot].generation = records[slot].generation;
                }
                if (inspection.result == ReadResult::Ok) inspection.result = SelectRecord(states, records, inspection.record);
                return inspection;
            }

            bool Write(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const Record& record, uint64_t& writtenGeneration) {
                if (stateOffset == 0 || stateBytes < kAreaBytes) return false;
                if (!medium.Writable()) return false;

                Record records[kSlots];
                CopyState states[kSlots] = {};
                if (ReadSlots(medium, stateOffset, stateBytes, records, states) != ReadResult::Ok) return false;

                // 目标槽位：代次最低的那个（未写过视为最低，优先于任何有效槽位）
                // 不变量：当前最新的槽位永不被覆盖——写完之后它仍是有效的旧记录，直到新槽位落盘
                uint64_t generation = 0;
                uint32_t targetSlot = 0;
                uint64_t targetGeneration = 0;
                bool targetPicked = false;
                for (uint32_t slot = 0; slot < kSlots; ++slot) {
                    const uint64_t slotGeneration = states[slot] == CopyState::Valid ? records[slot].generation : 0;
                    if (states[slot] == CopyState::Valid && slotGeneration > generation) generation = slotGeneration;
                    if (!targetPicked || slotGeneration < targetGeneration) {
                        targetSlot = slot;
                        targetGeneration = slotGeneration;
                        targetPicked = true;
                    }
                }

                // 代次：所有有效槽位的最大值加一；达到全一说明写入路径失控
                if (generation == 0xFFFFFFFFFFFFFFFFull) return false;
                ++generation;

                // 尝试身份缺省取写入代次（规格第二、四节）：Trying 记录必须非零，由代次填出
                Record effective = record;
                if (effective.state == State::Trying && effective.attemptId == 0) effective.attemptId = generation;
                if (!CombinationValid(effective)) return false;

                // 构造整份槽位：记录加零填充，校验和在末尾
                uint8_t copy[kCopyBytes] = {};
                Put32(copy + kFieldMagic, kMagic);
                Put16(copy + kFieldVersion, kVersion);
                Put16(copy + kFieldBytes, kRecordBytes);
                Put64(copy + kFieldGeneration, generation);
                copy[kFieldState] = static_cast<uint8_t>(effective.state);
                copy[kFieldTries] = effective.triesRemaining;
                copy[kFieldTriesDone] = effective.triesDone;
                copy[kFieldFlags] = 0;
                Put32(copy + kFieldReserved, 0);
                Put64(copy + kFieldAttemptId, effective.attemptId);
                memcpy(copy + kFieldConfirmedBuildId, effective.confirmedBuildId, 32);
                memcpy(copy + kFieldConfirmedDigest, effective.confirmedManifestDigest, 32);
                memcpy(copy + kFieldCandidateBuildId, effective.candidateBuildId, 32);
                memcpy(copy + kFieldCandidateDigest, effective.candidateManifestDigest, 32);
                Put64(copy + kFieldSeenSecurityVersion, effective.seenSecurityVersion);
                Put64(copy + kFieldRevocationEpoch, effective.revocation.epoch);
                Put64(copy + kFieldRevocationFloor, effective.revocation.minimumSecurityVersion);
                memcpy(copy + kFieldRevokedKeyMask, effective.revocation.revokedKeyMask, Revocation::kKeyMaskBytes);
                Put32(copy + kFieldChecksum, Crc32(copy, kFieldChecksum));

                const uint64_t target = stateOffset + targetSlot * kCopyBytes;
                if (medium.Write(target, copy, kCopyBytes) != Storage::Error::Ok) return false;

                // 刷介质缓存（写入顺序第 4 步）：后端没有刷新原语时基类返回 Ok，
                // 该路径的持久化边界见 Docs/Specs/Baleen/引导状态记录.md 第五节
                if (medium.Flush() != Storage::Error::Ok) return false;

                // 读回校验：写不成立时按失败处理，另一份副本不受影响
                uint8_t verify[kCopyBytes] = {};
                if (medium.Read(target, verify, kCopyBytes) != Storage::Error::Ok) return false;
                if (Get32(verify + kFieldMagic) != kMagic) return false;
                if (Get32(verify + kFieldChecksum) != Crc32(verify, kFieldChecksum)) return false;
                writtenGeneration = generation;
                return true;
            }

            bool Clear(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes) {
                if (stateOffset == 0 || stateBytes < kAreaBytes) return false;
                if (!medium.Writable()) return false;
                for (uint32_t i = 0; i < kAreaBytes; ++i) s_maintenance[i] = 0;
                for (uint32_t at = 0; at < kAreaBytes; at += kCopyBytes) {
                    if (medium.Write(stateOffset + at, s_maintenance, kCopyBytes) != Storage::Error::Ok) return false;
                }
                if (medium.Flush() != Storage::Error::Ok) return false;
                // 读回确认全零：清零不成立时操作者必须知道，而不是以为已经恢复
                if (medium.Read(stateOffset, s_maintenance, kAreaBytes) != Storage::Error::Ok) return false;
                for (uint32_t i = 0; i < kAreaBytes; ++i) {
                    if (s_maintenance[i] != 0) return false;
                }
                return true;
            }

            bool Repair(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, uint64_t& writtenGeneration) {
                Record record;
                // 没有可用记录时不猜测目标集：空区域、全部槽位不可用、最高代次出现两次都拒绝
                if (Read(medium, stateOffset, stateBytes, record) != ReadResult::Ok) return false;
                // 用读出的那份重建每一个不可用槽位：每次写落在代次最低的槽位并抬一代，
                // 因此修完的槽位代次互不相同，也不会与源槽位撞代次（最多修 kSlots - 1 个）
                for (uint32_t round = 0; round < kSlots - 1; ++round) {
                    Inspection inspection = Inspect(medium, stateOffset, stateBytes);
                    bool damaged = false;
                    for (uint32_t slot = 0; slot < kSlots; ++slot) {
                        if (inspection.slots[slot].status != CopyStatus::Valid) damaged = true;
                    }
                    if (!damaged) return true;
                    if (!Write(medium, stateOffset, stateBytes, record, writtenGeneration)) return false;
                }
                return true;
            }

            bool AcceptSeenSecurityVersion(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, uint64_t acceptedVersion, uint64_t& writtenGeneration) {
                Record record;
                if (Read(medium, stateOffset, stateBytes, record) != ReadResult::Ok) return false;
                // 只用于下调：抬高由选择器随清单更新自动完成，不给人工抬高开后门
                if (acceptedVersion >= record.seenSecurityVersion) return false;
                record.seenSecurityVersion = acceptedVersion;
                return Write(medium, stateOffset, stateBytes, record, writtenGeneration);
            }

            bool PublishCandidate(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const uint8_t candidateBuildId[32], const uint8_t candidateManifestDigest[32], uint8_t triesRemaining, uint64_t& writtenGeneration) {
                if (triesRemaining == 0 || triesRemaining > kMaxTries) return false;
                if (IsZero(candidateBuildId, 32)) return false;

                // 记录损坏时不动它：与选择器同一条规则，不猜测目标集
                Record record;
                const ReadResult read = Read(medium, stateOffset, stateBytes, record);
                if (read == ReadResult::Corrupt || read == ReadResult::Unavailable) return false;
                // 空区域是出厂形态：没有已知良好集、已见版本与撤销事实可保留，从零起步（代次由 Write 取 1）
                if (read == ReadResult::Empty) record = Record{};

                record.state = State::Pending;
                record.triesRemaining = triesRemaining;
                // 新候选的已试次数从零开始：它是“这一版试过几次”的账，换候选就重新记
                record.triesDone = 0;
                record.attemptId = 0;
                memcpy(record.candidateBuildId, candidateBuildId, 32);
                memcpy(record.candidateManifestDigest, candidateManifestDigest, 32);
                return Write(medium, stateOffset, stateBytes, record, writtenGeneration);
            }

            bool ConfirmAttempt(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, uint64_t attemptId, uint64_t& writtenGeneration) {
                if (attemptId == 0) return false;
                Record record;
                if (Read(medium, stateOffset, stateBytes, record) != ReadResult::Ok) return false;
                // 比较再提交：身份不符就不写，避免一次迟到的确认覆盖引导器已经推进的状态
                if (record.state != State::Trying || record.attemptId != attemptId) return false;

                Record next = record;
                next.state = State::Confirmed;
                memcpy(next.confirmedBuildId, record.candidateBuildId, 32);
                memcpy(next.confirmedManifestDigest, record.candidateManifestDigest, 32);
                for (uint32_t i = 0; i < 32; ++i) {
                    next.candidateBuildId[i] = 0;
                    next.candidateManifestDigest[i] = 0;
                }
                next.attemptId = 0;
                next.triesRemaining = 0;
                next.triesDone = 0;   // 候选转正：这一版已经跑通，重试的账清零
                return Write(medium, stateOffset, stateBytes, next, writtenGeneration);
            }
        }
    }
}
