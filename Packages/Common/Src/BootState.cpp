/* BootState.cpp
    引导状态记录的实现：槽位校验、最高代次选择、最旧槽位写入、刷盘、读回校验与受控恢复

    记录字节表与写入顺序：
    校验用 CRC-32（IEEE 802.3 参数），整数按小端读写，保留字段必须为零
    ModuleRevision 在 0x0D8..0x0E0（u64，回退目标修订号，0 = 无修订），0x0E0..0x0EC 是
    12 字节保留；Checksum 覆盖 [0x000, 0x0EC)，格式口径不变、只扩保留区
    **格式尚未发布，只有一个版本 1**：读写用同一张字段表，不留兼容分支；
    版本号不是 1 的记录按损坏处理，不猜字段表
    一次提交按设备逻辑块落盘：块不大于槽位时写一个槽位（512 是块的整数倍）；
    4Kn 上读—改—写目标槽位所在的整块（本布局下就是整个区域），其余槽位字节原样写回，
    提交因此仍只改变一个槽位的内容，掉电语义与单扇区写相同
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
                // 记录格式：标记“BLST”，版本 1 的 0xF0 字段表
                constexpr uint32_t kMagic = 0x54534C42u;
                // 槽位内字段偏移
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
                // 回退目标修订号：保留区 0x0D8..0x0E0 的 8 字节，0x0E0..0x0EC 仍是 12 字节保留；
                // Checksum 覆盖 [0x000, 0x0EC)，新字段因此在覆盖范围内
                constexpr uint32_t kFieldModuleRevision = 0x0D8;
                constexpr uint32_t kFieldReserved2 = 0x0E0;
                constexpr uint32_t kFieldChecksum = 0x0EC;
                // 保留区长度：0x014 的 4 字节与 0x0E0 起的 12 字节，都必须为零
                constexpr uint32_t kReservedBytes = 12;

                // 字段偏移与记录长度互相对齐：偏移写错会让校验和覆盖不到新字段或留出缝
                static_assert(kFieldModuleRevision + 8 == kFieldReserved2, "ModuleRevision 与保留区的边界必须相接");
                static_assert(kFieldReserved2 + kReservedBytes == kFieldChecksum, "保留区必须恰好填到校验和之前");
                static_assert(kFieldChecksum + 4 == kRecordBytes, "校验和必须是记录的最后 4 字节");
                static_assert(kFieldModuleRevision == 0xD8, "ModuleRevision 的偏移固定为 0xD8");

                // 设备逻辑块大小：块设备契约要求偏移与长度按它对齐
                // 块大小不是 2 的幂或超过区域长度时返回 0：状态区域要按整块读写，块比区域大就没有可用形态
                uint32_t BlockBytesOf(const Storage::BlockDevice& medium) {
                    const uint32_t blockBytes = medium.BlockBytes();
                    if (blockBytes == 0 || (blockBytes & (blockBytes - 1)) != 0) return 0;
                    if (blockBytes > kAreaBytes) return 0;
                    return blockBytes;
                }

                // 状态区域是否可用块设备读写：起点同时满足设备逻辑块对齐与 4096 对齐，长度至少覆盖整个区域
                // 前者是设备整块读写的契约（偏移与长度按逻辑块对齐），后者是布局契约
                // （规格 §四要求 StateOffset 是 4096 的整数倍）；4Kn 上区域恰好一个逻辑块
                bool AreaReady(uint64_t stateOffset, uint32_t stateBytes, uint32_t blockBytes) {
                    if (blockBytes == 0 || stateOffset == 0 || stateBytes < kAreaBytes) return false;
                    if ((stateOffset & (blockBytes - 1)) != 0) return false;
                    return (stateOffset & (kAreaBytes - 1)) == 0;
                }

                // 区域读缓冲：全部槽位一次读完
                uint8_t s_area[kAreaBytes];

                // 整块提交与读回校验的中转：设备逻辑块大于槽位时（4Kn）用它做读—改—写
                uint8_t s_block[kAreaBytes];

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

                // 读回校验：与写入顺序第 5 步一致，只核对标记与校验和
                bool CopyIntact(const uint8_t* copy) {
                    if (Get32(copy + kFieldMagic) != kMagic) return false;
                    return Get32(copy + kFieldChecksum) == Crc32(copy, kFieldChecksum);
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

                // 单个槽位的校验结论
                enum class CopyState : uint8_t {
                    NotWritten,   // 标记全零：该槽位未写过
                    Valid,        // 结构、组合与校验和都通过
                    Invalid,      // 写过但不可用
                };

                // 校验一个槽位并解出记录：只有一个版本，字段表就是本文件里那一张
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
                    // 0 是合法值：旧格式记录没有这个字段，读方按“无修订”解释，不拒绝
                    record.moduleRevision = Get64(copy + kFieldModuleRevision);
                    if (!CombinationValid(record)) return CopyState::Invalid;
                    return CopyState::Valid;
                }

                // 读出所有槽位：返回每个槽位的校验结论
                // 没有状态区域、区域参数非法与读盘失败分开报告：后两者意味着设备事实未知，
                // 调用方不能把它们当成“无记录”继续，否则已见版本与吊销并集会静默丢失
                ReadResult ReadSlots(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, Record* records, CopyState* states) {
                    if (stateOffset == 0) return ReadResult::NoRegion;
                    const uint32_t blockBytes = BlockBytesOf(medium);
                    if (!AreaReady(stateOffset, stateBytes, blockBytes)) return ReadResult::InvalidArea;
                    if (medium.Read(stateOffset, s_area, kAreaBytes) != Storage::Error::Ok) return ReadResult::IoError;
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
                const uint32_t blockBytes = BlockBytesOf(medium);
                if (!AreaReady(stateOffset, stateBytes, blockBytes)) return false;
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

                // 代次：所有有效槽位的最大值加一；达到上限说明写入路径失控，必须由调用方修复
                if (generation == kGenerationMax) return false;
                ++generation;

                // 尝试身份缺省取写入代次：Trying 记录必须非零，由代次填出
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
                // ModuleRevision：0 是合法值，表示无修订（旧格式记录或没有可回退修订）
                Put64(copy + kFieldModuleRevision, effective.moduleRevision);
                Put32(copy + kFieldChecksum, Crc32(copy, kFieldChecksum));

                // 提交按设备逻辑块落盘：块不大于槽位时一次写一个槽位（512 是块的整数倍）；
                // 块大于槽位时（4Kn）读—改—写目标槽位所在的整块，除目标槽位外的字节原样写回
                uint64_t commitOffset = stateOffset + targetSlot * kCopyBytes;
                uint32_t commitBytes = kCopyBytes;
                const uint8_t* commitSource = copy;
                uint32_t slotInBlock = 0;
                if (blockBytes > kCopyBytes) {
                    const uint32_t slotsPerBlock = blockBytes / kCopyBytes;
                    const uint32_t blockIndex = targetSlot / slotsPerBlock;
                    slotInBlock = (targetSlot % slotsPerBlock) * kCopyBytes;
                    commitOffset = stateOffset + blockIndex * blockBytes;
                    commitBytes = blockBytes;
                    memcpy(s_block, s_area + blockIndex * blockBytes, blockBytes);
                    memcpy(s_block + slotInBlock, copy, kCopyBytes);
                    commitSource = s_block;
                }
                if (medium.Write(commitOffset, commitSource, commitBytes) != Storage::Error::Ok) return false;

                // 刷介质缓存（写入顺序第 4 步）：后端没有刷新原语时基类返回 Ok，
                // 该路径的持久化边界取决于后端是否提供刷新原语
                if (medium.Flush() != Storage::Error::Ok) return false;

                // 读回校验（写入顺序第 5 步）：读回提交的那个逻辑块，核验目标槽位自身完整
                // 写不成立时按失败处理；其余槽位的字节没有被改变，另一份有效记录仍然可用
                if (blockBytes > kCopyBytes) {
                    if (medium.Read(commitOffset, s_block, commitBytes) != Storage::Error::Ok) return false;
                    if (!CopyIntact(s_block + slotInBlock)) return false;
                } else {
                    uint8_t verify[kCopyBytes] = {};
                    if (medium.Read(commitOffset, verify, commitBytes) != Storage::Error::Ok) return false;
                    if (!CopyIntact(verify)) return false;
                }
                writtenGeneration = generation;
                return true;
            }

            bool Clear(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes) {
                const uint32_t blockBytes = BlockBytesOf(medium);
                if (!AreaReady(stateOffset, stateBytes, blockBytes)) return false;
                if (!medium.Writable()) return false;
                for (uint32_t i = 0; i < kAreaBytes; ++i) s_maintenance[i] = 0;
                // 区域长度是设备逻辑块的整数倍（块不超过 4096 且为 2 的幂），整段一次写零；
                // 清零是显式的维护动作，不是一次状态提交，掉电时部分清零由操作者重跑
                if (medium.Write(stateOffset, s_maintenance, kAreaBytes) != Storage::Error::Ok) return false;
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

                // 记录损坏、读失败或参数非法时不动它：与选择器同一条规则，不猜测目标集；
                // 只有空区域是出厂形态，可以从零起步
                Record record;
                const ReadResult read = Read(medium, stateOffset, stateBytes, record);
                if (read == ReadResult::Corrupt || read == ReadResult::IoError || read == ReadResult::InvalidArea) return false;
                // 空区域（含没有状态区域的形态）：没有已知良好集、已见版本与撤销事实可保留，从零起步
                if (read == ReadResult::Empty || read == ReadResult::NoRegion) record = Record{};

                record.state = State::Pending;
                record.triesRemaining = triesRemaining;
                // 新候选的已试次数从零开始：它是“这一版试过几次”的账，换候选就重新记
                record.triesDone = 0;
                record.attemptId = 0;
                // ModuleRevision 原样继承（含 0）：它描述的是已知良好侧的回退目标，候选发布不改变它；
                // 空区域起步的 Record{} 没有已知良好侧，字段因此为 0
                memcpy(record.candidateBuildId, candidateBuildId, 32);
                memcpy(record.candidateManifestDigest, candidateManifestDigest, 32);
                return Write(medium, stateOffset, stateBytes, record, writtenGeneration);
            }

            bool ConfirmAttempt(Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, uint64_t attemptId, uint64_t moduleRevision, uint64_t& writtenGeneration) {
                if (attemptId == 0) return false;
                // 新写方写出的 Confirmed 记录 ModuleRevision 必须非零：
                // 候选修订号由当前列表头解析，确认方传 0 说明调用方没有取到修订事实，拒绝写入
                if (moduleRevision == 0) return false;
                Record record;
                if (Read(medium, stateOffset, stateBytes, record) != ReadResult::Ok) return false;
                // 比较再提交：身份不符就不写，避免一次迟到的确认覆盖引导器已经推进的状态
                if (record.state != State::Trying || record.attemptId != attemptId) return false;

                Record next = record;
                next.state = State::Confirmed;
                // 本次装载所用修订号成为回退目标：下次模块补丁发布后按它取回旧修订快照
                next.moduleRevision = moduleRevision;
                memcpy(next.confirmedBuildId, record.candidateBuildId, 32);
                memcpy(next.confirmedManifestDigest, record.candidateManifestDigest, 32);
                for (uint32_t i = 0; i < 32; ++i) {
                    next.candidateBuildId[i] = 0;
                    next.candidateManifestDigest[i] = 0;
                }
                next.attemptId = 0;
                next.triesRemaining = 0;
                // 已试次数不在这里清零：它是“这一版试过几次”的审计账，只增不减
                return Write(medium, stateOffset, stateBytes, next, writtenGeneration);
            }
        }
    }
}
