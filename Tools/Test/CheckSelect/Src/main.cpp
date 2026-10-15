/* main.cpp
    CheckSelect：BaleenCore 选版与引导状态记录的宿主侧检查，手工执行，不构成默认构建依赖

    检查对象是目标侧源码：Packages/Baleen/Core 的 SelectSet.cpp 与 BootState.cpp，
    按宿主选项编译后直接运行——选版决策、状态记录的副本选择与写入顺序都是将要交付的那份实现
    两处替身由本工具提供，因为它们不属于本工具的检查对象：
    1. 清单验证（VerifyKernelSet）：按集目录返回夹具登记的结果，只核对选版怎么调用它、
       结果怎么影响状态转换；清单结构、签名与摘要的真实验证由 CheckCrypto 与实机路径覆盖
    2. 诊断输出（WriteDiagnostic）：不装控制台，把诊断行收进夹具供断言

    运行期写入方（Updater 的 Pending、Init 的 Confirmed 提交）尚未实现，本工具按同一记录格式
    在介质上构造 Pending 记录，这是 Pending 与候选路径在开发期的可复现入口；
    记录字节表、副本布局与写入顺序见 Docs/Specs/Baleen/引导状态记录.md
*/

#include <BootState.hpp>
#include <ManifestVerify.hpp>
#include <SelectSet.hpp>

#include <Diagnostics.hpp>
#include <PrintTarget.hpp>
#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace BootState = Baleen::Core::BootState;
    using Record = Baleen::Core::BootState::Record;
    using State = Baleen::Core::BootState::State;
    using Bytes = std::vector<uint8_t>;

    // 引导状态区域的字节偏移与长度：与布局描述的默认值一致
    constexpr uint64_t kStateOffset = 0x21000;
    constexpr uint32_t kStateBytes = 4096;
    // 夹具介质容量：覆盖状态区域并留出余量（末尾几个字节用于越界注入）
    constexpr uint64_t kMediumBytes = 0x24000;
    // 集根与集内内核产物名：与系统卷包的落点约定一致
    constexpr const char* kSetRoot = "/LikesProgram/System/KernelSets/";
    constexpr const char* kKernelName = "LikesProgram.os";
    // 选版启用前的扁平落点：没有 KernelSets/ 标记，按缺省集直启
    constexpr const char* kFlatKernelPath = "/LikesProgram/System/LikesProgram.os";
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 40;
    // 已跑用例数
    unsigned g_ran = 0;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }
    // 诊断原因的可读写法
    std::string Text(const Baleen::Diagnostic& reason) {
        if (reason.code == nullptr) return "（无原因）";
        return std::string(reason.code) + " " + (reason.text == nullptr ? "" : reason.text);
    }
    // 按小端写入 2 字节
    void Put16(Bytes& data, std::size_t at, uint16_t value) {
        data[at] = static_cast<uint8_t>(value);
        data[at + 1] = static_cast<uint8_t>(value >> 8);
    }
    // 按小端写入 4 字节
    void Put32(Bytes& data, std::size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
    // 按小端写入 8 字节
    void Put64(Bytes& data, std::size_t at, uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) data[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
    // 用同一个字节填满集标识
    std::array<uint8_t, 32> Id(uint8_t value) {
        std::array<uint8_t, 32> id{};
        id.fill(value);
        return id;
    }
    // 集标识的 64 位小写十六进制写法
    std::string IdText(const uint8_t* buildId) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        for (unsigned i = 0; i < 32; ++i) {
            text += kDigits[buildId[i] >> 4];
            text += kDigits[buildId[i] & 0x0F];
        }
        return text;
    }
    // 集目录：集根加集标识
    std::string SetDirectory(const uint8_t* buildId) { return std::string(kSetRoot) + IdText(buildId); }
    // 集内内核镜像路径
    std::string KernelPath(const uint8_t* buildId) { return SetDirectory(buildId) + "/" + kKernelName; }
    // 干净的消息摘要：夹具用集标识派生，只要求逐字节稳定
    void FillDigest(uint8_t digest[32], const uint8_t* buildId) {
        for (unsigned i = 0; i < 32; ++i) digest[i] = static_cast<uint8_t>(buildId[i] ^ 0x5Au);
    }
    // 夹具侧的 CRC-32：与 BootState.cpp 的实现相互独立，用于构造校验和正确但字段违规的副本
    uint32_t FixtureCrc32(const uint8_t* data, uint32_t bytes) {
        uint32_t crc = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < bytes; ++i) {
            crc ^= data[i];
            for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    // 内存块设备：承载状态区域，记录读写次数并支持失败注入
    class MemoryMedium final : public Storage::BlockDevice {
    public:
        // 构造：容量与可写性
        MemoryMedium(uint64_t bytes, bool writable) : m_bytes(static_cast<std::size_t>(bytes), 0), m_writable(writable) {}
        // 逻辑块大小：状态区域按 512 的整数倍对齐，这里取 512
        uint32_t BlockBytes() const override { return 512; }
        // 设备容量
        uint64_t CapacityBytes() const override { return m_bytes.size(); }
        // 是否可写
        bool Writable() const override { return m_writable; }
        // 读一段介质内容
        Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override {
            m_events.push_back('R');
            ++m_reads;
            if (m_failRead) return Storage::Error::Io;
            if (!InRange(offset, bytes)) return Storage::Error::InvalidArgument;
            std::memcpy(buffer, m_bytes.data() + offset, bytes);
            return Storage::Error::Ok;
        }
        // 写一段介质内容；m_dropWrite 模拟“写返回成功但没有落盘”
        Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override {
            m_events.push_back('W');
            ++m_writes;
            if (!m_writable) return Storage::Error::ReadOnly;
            if (!InRange(offset, bytes)) return Storage::Error::InvalidArgument;
            if (m_failWrite) return Storage::Error::Io;
            if (m_dropWrite) return Storage::Error::Ok;
            std::memcpy(m_bytes.data() + offset, buffer, bytes);
            return Storage::Error::Ok;
        }
        // 刷新写缓存：记事件、计数并支持失败注入；基类语义是“无缓存”，这里模拟一个有缓存的后端
        Storage::Error Flush() override {
            m_events.push_back('F');
            ++m_flushes;
            if (m_failFlush) return Storage::Error::Io;
            return Storage::Error::Ok;
        }
        // 注入读失败
        void FailRead(bool value) { m_failRead = value; }
        // 注入写失败
        void FailWrite(bool value) { m_failWrite = value; }
        // 注入刷盘失败
        void FailFlush(bool value) { m_failFlush = value; }
        // 注入“写成功但不落盘”：读回校验必须发现
        void DropWrite(bool value) { m_dropWrite = value; }
        // 读调用次数
        unsigned Reads() const { return m_reads; }
        // 写尝试次数：包含返回失败的尝试
        unsigned Writes() const { return m_writes; }
        // 刷新调用次数：包含返回失败的调用
        unsigned Flushes() const { return m_flushes; }
        // 操作序列：'W' 写、'F' 刷新、'R' 读，用于核对写入顺序
        const std::vector<char>& Events() const { return m_events; }
        // 取介质内容的一段副本；越界即中止
        Bytes Slice(uint64_t offset, uint32_t bytes) const {
            Require(InRange(offset, bytes), "夹具读取越界");
            return Bytes(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), m_bytes.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
        }
        // 直接改写介质内容：用于构造损坏副本与非法记录
        void Patch(uint64_t offset, const uint8_t* data, uint32_t bytes) {
            Require(InRange(offset, bytes), "夹具写入越界");
            std::memcpy(m_bytes.data() + offset, data, bytes);
        }
    private:
        // 偏移与长度是否落在介质内
        bool InRange(uint64_t offset, uint32_t bytes) const { return offset <= m_bytes.size() && bytes <= m_bytes.size() - offset; }
        Bytes m_bytes;              // 介质内容
        bool m_writable;            // 是否可写
        bool m_failRead = false;    // 读失败注入
        bool m_failWrite = false;   // 写失败注入
        bool m_failFlush = false;   // 刷盘失败注入
        bool m_dropWrite = false;   // “写成功但不落盘”注入
        unsigned m_reads = 0;       // 读调用次数
        unsigned m_writes = 0;      // 写尝试次数
        unsigned m_flushes = 0;     // 刷新调用次数
        std::vector<char> m_events; // 操作序列
    };

    // 空文件系统：选版只把 FileSystem 转交给清单验证，替身不使用它
    class NullFileSystem final : public Storage::FileSystem {
    };

    // 夹具登记的一条集：目录、标识与清单事实
    struct SetEntry {
        std::string directory;      // 集目录路径
        uint8_t buildId[32] = {};   // 集标识
        uint64_t securityVersion = 0;   // 清单的安全版本
        uint32_t artifacts = 1;     // 产物条目数
        uint8_t signerKeyId = 1;    // 清单的签名密钥编号
        bool verified = true;       // 清单验证是否通过
        Baleen::Diagnostic failure{};   // 验证不通过时的原因
    };

    // 一次清单验证调用的记录：核对选版传了什么
    struct VerifyCall {
        std::string directory;              // 被验证的集目录
        uint8_t expectedBuildId[32] = {};   // 调用方给的期望标识
        uint64_t seenSecurityVersion = 0;   // 调用方给的已见最高安全版本
        Baleen::Core::Revocation::Facts revocation{};   // 调用方给的有效吊销事实
    };

    // 捕获的一条诊断行
    struct DiagLine {
        Baleen::PrintTargets::Tag tag;   // 标签
        std::string code;                // 编号
        std::string text;                // 原因文本
        std::string detail;              // 补充现场
    };

    // 选版夹具：内存介质、清单验证替身与诊断记录器
    class Fixture {
    public:
        // 构造：可写性，并把自己登记为替身的当前夹具
        explicit Fixture(bool writable = true) : m_medium(kMediumBytes, writable) { s_current = this; }
        // 析构：撤掉当前夹具
        ~Fixture() { s_current = nullptr; }
        // 不允许拷贝：介质与记录是独占的
        Fixture(const Fixture&) = delete;
        Fixture& operator=(const Fixture&) = delete;
        // 当前夹具：替身经它取结果
        static Fixture* Current() { return s_current; }
        // 夹具介质
        MemoryMedium& Medium() { return m_medium; }
        // 登记一条集：验证默认通过
        void AddSet(const uint8_t* buildId, uint64_t securityVersion) {
            SetEntry entry;
            entry.directory = SetDirectory(buildId);
            std::memcpy(entry.buildId, buildId, sizeof(entry.buildId));
            entry.securityVersion = securityVersion;
            m_sets.push_back(entry);
        }
        // 让一条已登记的集验证失败：给稳定编号与原因文本
        void FailSet(const uint8_t* buildId, const char* code, const char* text) {
            for (auto& entry : m_sets) {
                if (entry.directory != SetDirectory(buildId)) continue;
                entry.verified = false;
                entry.failure = Baleen::Diagnostic{code, text};
                return;
            }
            Fail("夹具没有登记这条集：" + SetDirectory(buildId));
        }
        // 替身的验证入口：命中登记条目则返回其结果，未登记按“清单不存在”处理
        // 吊销事实按规格参与判定（被吊销的签名密钥、记录抬高的下限），与真实 ManifestVerify 一致；
        // 真实判定本身由 CheckRevocation 覆盖，这里核对的是选版有没有把事实传下来
        Baleen::Diagnostic Verify(const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Baleen::Core::Revocation::Facts& revocation, Baleen::Core::ManifestFacts& facts) {
            VerifyCall call;
            call.directory = setDirectory;
            std::memcpy(call.expectedBuildId, expectedBuildId, sizeof(call.expectedBuildId));
            call.seenSecurityVersion = seenSecurityVersion;
            call.revocation = revocation;
            m_calls.push_back(call);
            for (const auto& entry : m_sets) {
                if (entry.directory != setDirectory) continue;
                // 选版必须按记录里的集标识推导目录：标识不符按集身份不一致拒绝
                if (std::memcmp(entry.buildId, expectedBuildId, 32) != 0) return Baleen::Diagnostic{"E-CORE-65", "The manifest BuildId doesn't match the selected set"};
                if (!entry.verified) return entry.failure;
                if (Baleen::Core::Revocation::IsKeyRevoked(revocation.revokedKeyMask, entry.signerKeyId)) return Baleen::Diagnostic{"E-CORE-74", "The manifest signature key is revoked"};
                if (entry.securityVersion < revocation.minimumSecurityVersion) return Baleen::Diagnostic{"E-CORE-66", "The manifest SecurityVersion is below what the device or the bootloader allows"};
                // 防回滚（规格第三节第 6 条）：清单安全版本低于设备已见最高即拒绝，
                // 替身实现这一条是为了核对选版有没有把设备的已见最高传下来
                if (entry.securityVersion < seenSecurityVersion) return Baleen::Diagnostic{"E-CORE-66", "The manifest SecurityVersion is below what the device or the bootloader allows"};
                std::memcpy(facts.buildId, entry.buildId, sizeof(facts.buildId));
                FillDigest(facts.manifestDigest, entry.buildId);
                facts.securityVersion = entry.securityVersion;
                facts.artifactCount = entry.artifacts;
                facts.manifestBytes = 0x100;
                return Baleen::Diagnostic{};
            }
            return Baleen::Diagnostic{"E-CORE-61", "Read the kernel set manifest"};
        }
        // 替身的诊断记录入口
        void Capture(Baleen::PrintTargets::Tag tag, const Baleen::Diagnostic& diagnostic, const char* detail) {
            DiagLine line;
            line.tag = tag;
            line.code = diagnostic.code == nullptr ? "" : diagnostic.code;
            line.text = diagnostic.text == nullptr ? "" : diagnostic.text;
            line.detail = detail == nullptr ? "" : detail;
            m_diagnostics.push_back(line);
        }
        // 已捕获的诊断行
        const std::vector<DiagLine>& Diagnostics() const { return m_diagnostics; }
        // 清单验证的调用记录
        const std::vector<VerifyCall>& Calls() const { return m_calls; }
        // 跑一次选版
        Baleen::Diagnostic Select(const char* layoutKernelPath, Baleen::Core::SelectedSet& selected, uint64_t stateOffset = kStateOffset, uint32_t stateBytes = kStateBytes) {
            return Baleen::Core::SelectKernelSet(m_fileSystem, m_medium, stateOffset, stateBytes, layoutKernelPath, selected);
        }
        // 按生产写入方的同一套接口写一份记录：开发期对 Updater / Init 的替身
        bool WriteRecord(const Record& record, uint64_t& generation) {
            return BootState::Write(m_medium, kStateOffset, kStateBytes, record, generation);
        }
        // 读一份记录
        BootState::ReadResult ReadRecord(Record& record) {
            return BootState::Read(m_medium, kStateOffset, kStateBytes, record);
        }
        // 副本级检查：两份副本各自的结论与选中记录
        BootState::Inspection InspectRecords() { return BootState::Inspect(m_medium, kStateOffset, kStateBytes); }
        // 受控恢复：清零两份副本
        bool ClearArea() { return BootState::Clear(m_medium, kStateOffset, kStateBytes); }
        // 受控恢复：按有效副本修复
        bool RepairArea(uint64_t& generation) { return BootState::Repair(m_medium, kStateOffset, kStateBytes, generation); }
        // 人工接受：下调已见最高安全版本
        bool AcceptSeenVersion(uint64_t version, uint64_t& generation) { return BootState::AcceptSeenSecurityVersion(m_medium, kStateOffset, kStateBytes, version, generation); }
    private:
        static Fixture* s_current;      // 替身当前的夹具
        MemoryMedium m_medium;          // 夹具介质
        NullFileSystem m_fileSystem;    // 空文件系统
        std::vector<SetEntry> m_sets;   // 登记的集
        std::vector<VerifyCall> m_calls;    // 清单验证调用记录
        std::vector<DiagLine> m_diagnostics;   // 捕获的诊断行
    };

    Fixture* Fixture::s_current = nullptr;

    // 已见最高集的记录：Confirmed、候选为空
    Record ConfirmedRecord(const uint8_t* confirmedId, uint64_t seenSecurityVersion = 0) {
        Record record;
        record.state = State::Confirmed;
        std::memcpy(record.confirmedBuildId, confirmedId, 32);
        FillDigest(record.confirmedManifestDigest, confirmedId);
        record.seenSecurityVersion = seenSecurityVersion;
        return record;
    }

    // 候选记录：Pending / Trying / Failed；hasConfirmed 决定是否带上已知良好集
    Record CandidateRecord(State state, const uint8_t* candidateId, const uint8_t* confirmedId, uint8_t triesRemaining) {
        Record record;
        record.state = state;
        record.triesRemaining = triesRemaining;
        std::memcpy(record.candidateBuildId, candidateId, 32);
        FillDigest(record.candidateManifestDigest, candidateId);
        if (confirmedId != nullptr) {
            std::memcpy(record.confirmedBuildId, confirmedId, 32);
            FillDigest(record.confirmedManifestDigest, confirmedId);
        }
        return record;
    }

    // 夹具侧的副本构造：按规格第二节的字节表拼接 2048 字节副本，校验和用夹具自己的 CRC-32
    // 生产代码不会产生字段违规的记录，构造这类副本是为了核对读取方的逐项校验
    Bytes RawCopy(const Record& record, uint8_t stateValue, uint16_t flags, uint32_t reserved, bool checksumCorrect) {
        Bytes copy(BootState::kCopyBytes, 0);
        Put32(copy, 0x000, 0x54534C42u);                    // Magic "BLST"
        Put16(copy, 0x004, BootState::kVersion);            // Version
        Put16(copy, 0x006, BootState::kRecordBytes);        // Bytes
        Put64(copy, 0x008, record.generation);              // Generation
        copy[0x010] = stateValue;                           // State
        copy[0x011] = record.triesRemaining;                // TriesRemaining
        Put16(copy, 0x012, flags);                          // Flags
        Put32(copy, 0x014, reserved);                       // Reserved
        Put64(copy, 0x018, record.attemptId);               // AttemptId
        std::memcpy(copy.data() + 0x020, record.confirmedBuildId, 32);
        std::memcpy(copy.data() + 0x040, record.confirmedManifestDigest, 32);
        std::memcpy(copy.data() + 0x060, record.candidateBuildId, 32);
        std::memcpy(copy.data() + 0x080, record.candidateManifestDigest, 32);
        Put64(copy, 0x0A0, record.seenSecurityVersion);     // SeenSecurityVersion
        Put64(copy, 0x0A8, record.revocation.epoch);        // RevocationEpoch
        Put64(copy, 0x0B0, record.revocation.minimumSecurityVersion);   // RevocationFloor
        std::memcpy(copy.data() + 0x0B8, record.revocation.revokedKeyMask, 32);
        const uint32_t crc = FixtureCrc32(copy.data(), 0x0EC);
        Put32(copy, 0x0EC, checksumCorrect ? crc : (crc ^ 1u));
        return copy;
    }

    // 重算一份原始副本的校验和：改过字段的副本要重新修校验和，才是在测字段校验而不是校验和校验
    void FixChecksum(Bytes& copy) { Put32(copy, 0x0EC, FixtureCrc32(copy.data(), 0x0EC)); }

    // 把一份原始副本放进指定副本槽
    void PutCopy(Fixture& fixture, uint64_t copyOffset, const Bytes& copy) {
        Require(copy.size() == BootState::kCopyBytes, "原始副本长度不符");
        fixture.Medium().Patch(kStateOffset + copyOffset, copy.data(), static_cast<uint32_t>(copy.size()));
    }

    // 核对选中集：目录、内核路径、标识、已验证与试启动身份
    void RequireSelected(const Baleen::Core::SelectedSet& selected, const uint8_t* buildId, uint64_t attemptId) {
        Require(std::string(selected.setDirectory) == SetDirectory(buildId), "选中的集目录不符：" + std::string(selected.setDirectory));
        Require(std::string(selected.kernelPath) == KernelPath(buildId), "选中的内核路径不符：" + std::string(selected.kernelPath));
        Require(std::memcmp(selected.buildId, buildId, 32) == 0, "选中的集标识不符");
        Require(selected.verified, "选中的集未经清单验证");
        Require(selected.attemptId == attemptId, "试启动身份不符：" + std::to_string(selected.attemptId));
    }

    // 核对夹具里出现过的诊断编号与条数
    void RequireDiagnostic(const Fixture& fixture, const char* code, unsigned count = 1) {
        unsigned seen = 0;
        for (const auto& line : fixture.Diagnostics()) if (line.code == code) ++seen;
        Require(seen == count, std::string("诊断 ") + code + " 出现 " + std::to_string(seen) + " 次，期望 " + std::to_string(count) + " 次");
    }

    // 核对最后一次写入之后的顺序是“刷新 → 读回”：规格第四节第 3..5 步
    void RequireWriteFlushReadOrder(Fixture& fixture) {
        const auto& events = fixture.Medium().Events();
        std::size_t write = events.size();
        for (std::size_t i = events.size(); i > 0; --i) {
            if (events[i - 1] == 'W') { write = i - 1; break; }
        }
        Require(write + 2 < events.size() && events[write + 1] == 'F' && events[write + 2] == 'R', "写入之后的顺序必须是写 → 刷盘 → 读回");
    }

    // —— 记录读写：副本选择、写入顺序、损坏与写失败 ——

    // 空区域按“无记录”处理：缺省集直启、不写盘、无诊断
    void CaseEmptyArea() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 0);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "空区域返回了致命原因：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        Require(fixture.Medium().Writes() == 0, "空区域不该写盘");
        Require(fixture.Diagnostics().empty(), "空区域不该有诊断");
    }

    // 没有状态区域按“无记录”处理：缺省集直启
    void CaseNoStateArea() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 0);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected, 0, 0);
        Require(reason.code == nullptr, "没有状态区域返回了致命原因：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        Require(fixture.Medium().Reads() == 0, "没有状态区域不该读盘");
        Require(fixture.Medium().Writes() == 0, "没有状态区域不该写盘");
    }

    // 读状态区域失败按“无记录”处理：缺省集直启
    void CaseStateReadFailure() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 0);
        fixture.Medium().FailRead(true);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "读失败返回了致命原因：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        Require(fixture.Medium().Writes() == 0, "读失败不该写盘");
    }

    // 首次写入落副本 A、代次为 1，写入前区域必须是空的
    void CaseFirstWriteLandsInCopyA() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        Record record = ConfirmedRecord(confirmed.data());
        uint64_t generation = 0;
        Require(fixture.WriteRecord(record, generation), "空区域首次写入必须成立");
        Require(generation == 1, "空区域首次写入的代次必须是 1，实际 " + std::to_string(generation));
        Require(fixture.Medium().Writes() == 1, "首次写入只允许一次写调用");
        const Bytes copyA = fixture.Medium().Slice(kStateOffset, 4);
        const Bytes copyB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes, 4);
        Require(std::memcmp(copyA.data(), "BLST", 4) == 0, "首次写入必须落在副本 A");
        Require(std::memcmp(copyB.data(), "\0\0\0\0", 4) == 0, "首次写入不该碰副本 B");
    }

    // 第二次写入落非活动副本 B、代次为 2；两份有效时读取取代次更大的一份
    void CaseSecondWriteRotatesToCopyB() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
        Record pending = CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries);
        Require(fixture.WriteRecord(pending, generation), "第二份记录写入失败");
        Require(generation == 2, "第二份记录的代次必须是 2，实际 " + std::to_string(generation));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "两份副本里必须能读出记录");
        Require(read.generation == 2 && read.state == State::Pending, "读取必须取代次更大的副本");
        const Bytes copyB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes, 4);
        Require(std::memcmp(copyB.data(), "BLST", 4) == 0, "第二次写入必须落在副本 B");
    }

    // 第三次写入落回代次较小的副本 A、代次为 3：写入永不覆盖当前有效副本
    void CaseThirdWriteRotatesBackToCopyA() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries), generation), "第二份记录写入失败");
        Record failed = CandidateRecord(State::Failed, Id(0x22).data(), confirmed.data(), 0);
        Require(fixture.WriteRecord(failed, generation), "第三份记录写入失败");
        Require(generation == 3, "第三份记录的代次必须是 3，实际 " + std::to_string(generation));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "两份副本里必须能读出记录");
        Require(read.generation == 3 && read.state == State::Failed, "读取必须取代次最大的副本");
        // 两份副本的代次分别是 1 / 2：第三份只能写代次较小的那份
        const Bytes headA = fixture.Medium().Slice(kStateOffset + 0x008, 8);
        const Bytes headB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes + 0x008, 8);
        uint64_t generationA = 0, generationB = 0;
        for (unsigned i = 0; i < 8; ++i) {
            generationA |= static_cast<uint64_t>(headA[i]) << (8 * i);
            generationB |= static_cast<uint64_t>(headB[i]) << (8 * i);
        }
        Require(generationA == 3 && generationB == 2, "第三份必须写代次较小的副本 A（A=" + std::to_string(generationA) + "，B=" + std::to_string(generationB) + "）");
    }

    // 一份副本写坏：回落到另一份完整有效的副本（掉电语义：正在写的一份可能不完整）
    void CaseOneCopyCorrupt() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries), generation), "第二份记录写入失败");
        // 第二份落在副本 B：把它改坏，读取必须回落到副本 A 的 Confirmed 记录
        Bytes copyB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes, BootState::kCopyBytes);
        copyB[0x060] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, copyB.data(), static_cast<uint32_t>(copyB.size()));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "一份写坏时必须取另一份");
        Require(read.generation == 1 && read.state == State::Confirmed, "回落到的记录必须是被写坏那份之外的另一份");
    }

    // 两份副本都写坏：按记录损坏处理，不猜测目标集
    void CaseBothCopiesCorrupt() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Bytes copyA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        copyA[0x030] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, copyA.data(), static_cast<uint32_t>(copyA.size()));
        Bytes copyB(BootState::kCopyBytes, 0);
        copyB[0] = 0x42;           // 只有写入痕迹，没有可用记录
        fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, copyB.data(), static_cast<uint32_t>(copyB.size()));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Corrupt, "两份都不可用必须判为损坏");
    }

    // 两份有效但代次相等：视为写方违规，两份都不采用
    void CaseEqualGenerations() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        const Bytes copyA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, copyA.data(), static_cast<uint32_t>(copyA.size()));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Corrupt, "代次相等必须判为损坏");
    }

    // 夹具按字节表构造的记录被生产读取方接受：两份 CRC-32 与字段偏移互验
    void CaseRawCopyAccepted() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        Record record = ConfirmedRecord(confirmed.data(), 7);
        record.generation = 9;
        record.triesRemaining = 0;
        const Bytes copy = RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true);
        PutCopy(fixture, 0, copy);
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "夹具按字节表构造的记录必须被接受");
        Require(read.generation == 9 && read.state == State::Confirmed && read.seenSecurityVersion == 7, "读出的字段不符");
        Require(std::memcmp(read.confirmedBuildId, confirmed.data(), 32) == 0, "读出的集标识不符");
        Require(std::memcmp(read.confirmedManifestDigest, record.confirmedManifestDigest, 32) == 0, "读出的清单摘要不符");
    }

    // 字段违规的副本一律拒绝：校验和正确也不能改变结论
    void CaseMalformedCopiesRejected() {
        const auto confirmed = Id(0x11);
        Record record = ConfirmedRecord(confirmed.data());
        record.generation = 5;
        // name 是用例名，make 是改动副本的写法，valid 是读取方应有的结论
        struct Variant {
            const char* name;                       // 违规项
            bool valid;                             // 是否应有有效结论
            void (*make)(Bytes&);                   // 构造改动
        };
        const auto makeBadChecksum = +[](Bytes& copy) { copy[0x0EC] ^= 0xFF; };
        const auto makeStateUnknown = +[](Bytes& copy) { copy[0x010] = 9; FixChecksum(copy); };
        const auto makeFlags = +[](Bytes& copy) { Put16(copy, 0x012, 1); FixChecksum(copy); };
        const auto makeReserved = +[](Bytes& copy) { Put32(copy, 0x014, 1); FixChecksum(copy); };
        const auto makeReservedTail = +[](Bytes& copy) { copy[0x0D8] = 1; FixChecksum(copy); };
        const auto makeGenerationZero = +[](Bytes& copy) { Put64(copy, 0x008, 0); FixChecksum(copy); };
        const auto makeTriesTooMany = +[](Bytes& copy) { copy[0x011] = 200; FixChecksum(copy); };
        const auto makeConfirmedWithCandidate = +[](Bytes& copy) { copy[0x060] = 0x22; FixChecksum(copy); };
        const auto makeConfirmedWithoutId = +[](Bytes& copy) { std::memset(copy.data() + 0x020, 0, 32); FixChecksum(copy); };
        const auto makePendingWithAttempt = +[](Bytes& copy) {
            copy[0x010] = 2;
            Put64(copy, 0x018, 3);
            FixChecksum(copy);
        };
        const Variant variants[] = {
            { "基线（字段全部合法）", true, nullptr },
            { "校验和不符", false, makeBadChecksum },
            { "状态取值越界", false, makeStateUnknown },
            { "Flags 非零", false, makeFlags },
            { "保留字段非零", false, makeReserved },
            { "保留字段尾部非零", false, makeReservedTail },
            { "代次为零", false, makeGenerationZero },
            { "尝试次数超上限", false, makeTriesTooMany },
            { "Confirmed 带候选字段", false, makeConfirmedWithCandidate },
            { "Confirmed 缺集标识", false, makeConfirmedWithoutId },
            { "Pending 带尝试身份", false, makePendingWithAttempt },
        };
        for (const auto& variant : variants) {
            Fixture fixture;
            fixture.AddSet(confirmed.data(), 0);
            Bytes copy = RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true);
            if (variant.make != nullptr) variant.make(copy);
            PutCopy(fixture, 0, copy);
            Record read;
            const auto result = fixture.ReadRecord(read);
            if (variant.valid) {
                Require(result == BootState::ReadResult::Ok, std::string("变体“") + variant.name + "”必须被接受");
                Baleen::Core::SelectedSet selected;
                const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
                Require(reason.code == nullptr, std::string("变体“") + variant.name + "”选版失败：" + Text(reason));
                RequireSelected(selected, confirmed.data(), 0);
            } else {
                Require(result == BootState::ReadResult::Corrupt, std::string("变体“") + variant.name + "”必须被拒绝");
                Baleen::Core::SelectedSet selected;
                const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
                Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-69", std::string("变体“") + variant.name + "”必须以 E-CORE-69 停机");
                Require(selected.setDirectory[0] == '\0', std::string("变体“") + variant.name + "”不该定下目标集");
            }
        }
    }

    // 代次达到全 FF：拒绝写入，不落盘
    void CaseGenerationOverflow() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        Record record = ConfirmedRecord(confirmed.data());
        record.generation = 0xFFFFFFFFFFFFFFFFull;
        PutCopy(fixture, 0, RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true));
        const Bytes before = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes * 2);
        uint64_t generation = 0;
        Require(!fixture.WriteRecord(ConfirmedRecord(confirmed.data(), 4), generation), "代次全 FF 时必须拒绝写入");
        const Bytes after = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes * 2);
        Require(before == after, "代次全 FF 时不该改动介质");
    }

    // 只读介质：写不成立、不进入候选，按降级路径选已知良好集
    void CaseReadOnlyMedium() {
        Fixture fixture(false);
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        Record pending = CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries);
        pending.generation = 1;
        PutCopy(fixture, 0, RawCopy(pending, static_cast<uint8_t>(State::Pending), 0, 0, true));
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "只读介质返回了致命原因：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        Require(fixture.Medium().Writes() == 0, "只读介质不该发起写调用");
        RequireDiagnostic(fixture, "E-CORE-70");
    }

    // 写失败：不落盘、另一份副本字节不变
    void CaseWriteFailureKeepsOtherCopy() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
        const Bytes copyA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        fixture.Medium().FailWrite(true);
        Require(!fixture.WriteRecord(CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries), generation), "写失败必须返回不成立");
        Require(fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes) == copyA, "写失败不该改动当前有效副本");
        const Bytes copyB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes, 4);
        Require(std::memcmp(copyB.data(), "\0\0\0\0", 4) == 0, "写失败不该在另一份副本留下内容");
    }

    // 读回校验失败：写“成功”但没落盘，按写失败处理
    void CaseReadBackMismatch() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
        fixture.Medium().DropWrite(true);
        Require(!fixture.WriteRecord(CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries), generation), "读回不符必须返回不成立");
    }

    // 副本级检查：报告两份副本各自的结论与选中记录，能指出坏的是哪一份
    void CaseInspectCopies() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data(), 5), generation), "记录写入失败");
        auto inspection = fixture.InspectRecords();
        Require(inspection.result == BootState::ReadResult::Ok, "一份有效时区域结论必须有效");
        Require(inspection.copyA.status == BootState::CopyStatus::Valid && inspection.copyA.generation == 1, "副本 A 必须报告有效与代次 1");
        Require(inspection.copyB.status == BootState::CopyStatus::NotWritten && inspection.copyB.generation == 0, "副本 B 必须报告未写过");
        // 再写一份到副本 B，然后把副本 A 改坏：报告要指出坏的是哪一份
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries), generation), "第二份记录写入失败");
        Bytes copyA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        copyA[0x030] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, copyA.data(), static_cast<uint32_t>(copyA.size()));
        inspection = fixture.InspectRecords();
        Require(inspection.copyA.status == BootState::CopyStatus::Invalid, "写坏的一份必须报告不可用");
        Require(inspection.copyB.status == BootState::CopyStatus::Valid && inspection.copyB.generation == 2, "另一份必须报告有效与自身代次");
        Require(inspection.result == BootState::ReadResult::Ok && inspection.record.generation == 2, "区域结论必须取有效的那一份");
    }

    // 受控清零：两份副本清零、区域回到出厂形态；只读介质上拒绝
    void CaseClearArea() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Require(fixture.ClearArea(), "可写介质上的清零必须成立");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Empty, "清零后区域必须按空处理");
        const auto inspection = fixture.InspectRecords();
        Require(inspection.copyA.status == BootState::CopyStatus::NotWritten && inspection.copyB.status == BootState::CopyStatus::NotWritten, "清零后两份副本都必须未写过");
        Fixture readOnly(false);
        Require(!readOnly.ClearArea(), "只读介质上的清零必须失败");
    }

    // 按有效副本修复：代次加一、两份都有效、记录内容不变；没有可用记录时拒绝
    void CaseRepairCopies() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data(), 7), generation), "第一份记录写入失败");
        Record pending = CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries);
        pending.seenSecurityVersion = 7;
        Require(fixture.WriteRecord(pending, generation), "第二份记录写入失败");
        Record before;
        Require(fixture.ReadRecord(before) == BootState::ReadResult::Ok, "修复前必须能读出记录");
        // 写坏代次较小的那份（副本 A，代次 1）：另一份（副本 B，代次 2）完好
        Bytes copyA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        copyA[0x008] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, copyA.data(), static_cast<uint32_t>(copyA.size()));
        Require(fixture.InspectRecords().copyA.status == BootState::CopyStatus::Invalid, "夹具必须先把副本 A 写坏");
        Require(fixture.RepairArea(generation), "有有效副本时必须能修复");
        Require(generation == 3, "修复写入的代次必须是 3（两份最大值加一）");
        const auto inspection = fixture.InspectRecords();
        Require(inspection.copyA.status == BootState::CopyStatus::Valid && inspection.copyB.status == BootState::CopyStatus::Valid, "修复后两份都必须有效");
        Require(inspection.record.state == before.state && inspection.record.seenSecurityVersion == before.seenSecurityVersion, "修复不得改变状态与已见版本");
        Require(inspection.record.triesRemaining == before.triesRemaining, "修复不得改变剩余尝试次数");
        Require(std::memcmp(inspection.record.candidateBuildId, candidate.data(), 32) == 0, "修复必须保留候选集");
        uint64_t ignored = 0;
        Fixture empty;
        Require(!empty.RepairArea(ignored), "空区域不得修复");
        Fixture broken;
        Bytes garbage(BootState::kCopyBytes, 0);
        garbage[0] = 0x42;
        broken.Medium().Patch(kStateOffset, garbage.data(), static_cast<uint32_t>(garbage.size()));
        broken.Medium().Patch(kStateOffset + BootState::kCopyBytes, garbage.data(), static_cast<uint32_t>(garbage.size()));
        Require(!broken.RepairArea(ignored), "两份都不可用时不得修复");
    }

    // 人工接受：下调已见最高安全版本；抬高、相等或没有记录都拒绝
    void CaseAcceptSeenVersion() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data(), 9), generation), "记录写入失败");
        Require(!fixture.AcceptSeenVersion(9, generation), "相等值不得接受");
        Require(!fixture.AcceptSeenVersion(10, generation), "抬高不得接受");
        Require(fixture.AcceptSeenVersion(3, generation), "下调必须成立");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "接受后必须能读出记录");
        Require(read.seenSecurityVersion == 3 && read.state == State::Confirmed, "接受只改已见版本，不改状态");
        Require(read.generation == 2, "接受写入的代次必须是 2");
        Fixture empty;
        Require(!empty.AcceptSeenVersion(1, generation), "没有记录时不得接受");
    }

    // 掉电演练：整份副本只落下一部分时，读取方必须仍然拿到一份完整有效副本，不判损坏
    // 逐字节截断两个方向的写入，核对"任何时刻至少有一份完整有效副本"这条不变量；
    // 截断到记录有效长度（0xF0，含校验和）时新副本自身已完整，读取取代次更大的那一份
    void CasePowerLossDrill() {
        // 方向一：副本 A 已有效（代次 1），向副本 B 写代次 2 的副本，逐字节截断
        {
            Fixture fixture;
            const auto confirmed = Id(0x11);
            uint64_t generation = 0;
            Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
            Record pending = CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries);
            pending.generation = 2;
            const Bytes full = RawCopy(pending, static_cast<uint8_t>(State::Pending), 0, 0, true);
            const Bytes pristine = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes * 2);
            for (uint32_t kept = 0; kept < BootState::kCopyBytes; ++kept) {
                fixture.Medium().Patch(kStateOffset, pristine.data(), static_cast<uint32_t>(pristine.size()));
                if (kept > 0) fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, full.data(), kept);
                Record read;
                Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "副本 B 只落 " + std::to_string(kept) + " 字节时读取必须成立");
                if (kept < BootState::kRecordBytes) {
                    Require(read.generation == 1 && read.state == State::Confirmed, "副本 B 只落 " + std::to_string(kept) + " 字节（未过记录长度）时必须回落到副本 A");
                } else {
                    Require(read.generation == 2 && read.state == State::Pending, "副本 B 落满记录长度 " + std::to_string(kept) + " 字节时新副本自身已完整");
                }
            }
        }
        // 方向二：两份都有效（A 代次 1、B 代次 2），向副本 A 写代次 3 的副本，逐字节截断
        {
            Fixture fixture;
            const auto confirmed = Id(0x11);
            const auto candidate = Id(0x22);
            uint64_t generation = 0;
            Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
            Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries), generation), "第二份记录写入失败");
            Record failed = CandidateRecord(State::Failed, candidate.data(), confirmed.data(), 0);
            failed.generation = 3;
            const Bytes full = RawCopy(failed, static_cast<uint8_t>(State::Failed), 0, 0, true);
            const Bytes pristine = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes * 2);
            for (uint32_t kept = 0; kept < BootState::kCopyBytes; ++kept) {
                fixture.Medium().Patch(kStateOffset, pristine.data(), static_cast<uint32_t>(pristine.size()));
                if (kept > 0) fixture.Medium().Patch(kStateOffset, full.data(), kept);
                Record read;
                Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "副本 A 只落 " + std::to_string(kept) + " 字节时读取必须成立");
                if (kept < BootState::kRecordBytes) {
                    Require(read.generation == 2 && read.state == State::Pending, "副本 A 只落 " + std::to_string(kept) + " 字节（未过记录长度）时必须回落到副本 B");
                } else {
                    Require(read.generation == 3 && read.state == State::Failed, "副本 A 落满记录长度 " + std::to_string(kept) + " 字节时新副本自身已完整");
                }
            }
        }
    }

    // 写入顺序：写 → 刷介质缓存 → 读回校验（规格第四节第 3..5 步）
    void CaseWriteFlushReadOrder() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Require(fixture.Medium().Flushes() == 1, "一次写入必须刷一次介质缓存");
        RequireWriteFlushReadOrder(fixture);
    }

    // 刷盘失败按写失败处理：不进行状态转换，回退已知良好集；已写出的字节不回收，
    // 下一次启动按 Trying 判定的方向由掉电语义覆盖（规格第四节）
    void CaseFlushFailure() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries), generation), "Pending 记录写入失败");
        fixture.Medium().FailFlush(true);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "回退选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-70");
        Require(fixture.Medium().Flushes() == 2, "候选路径必须尝试刷盘：夹具一次、候选一次");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "刷盘失败不影响已写出的字节可读");
        Require(read.state == State::Trying, "刷盘失败后介质上留下的 Trying 由下次启动按未确认处理");
    }

    // —— 选版：四态决策、回退与写入门槛 ——

    // Confirmed：选集并取回清单事实，不写记录
    void CaseConfirmedSelects() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSet(confirmed.data(), 0);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "Confirmed 选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        Require(fixture.Medium().Writes() == 1, "Confirmed 不该写记录");
        Require(fixture.Diagnostics().empty(), "Confirmed 选版不该有诊断");
    }

    // Confirmed 且清单安全版本更高：同状态抬升已见最高版本，代次加一
    void CaseConfirmedLiftsSeenVersion() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSet(confirmed.data(), 5);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data(), 2), generation), "记录写入失败");
        Require(generation == 1, "首次写入代次必须是 1");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "Confirmed 选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        Require(fixture.Medium().Writes() == 2, "抬升已见版本必须写一份新记录");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "抬升后必须能读出记录");
        Require(read.generation == 2, "抬升记录的代次必须是 2");
        Require(read.state == State::Confirmed && read.seenSecurityVersion == 5, "抬升记录必须是同状态且已见版本更新");
        Require(std::memcmp(read.confirmedBuildId, confirmed.data(), 32) == 0, "抬升记录必须保留已知良好集");
    }

    // Confirmed 集验证失败：返回致命原因，不写记录
    void CaseConfirmedVerifyFailure() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSet(confirmed.data(), 0);
        fixture.FailSet(confirmed.data(), "E-CORE-64", "The manifest signature doesn't verify");
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-64", "必须把清单验证的原因原样返回：" + Text(reason));
        Require(fixture.Medium().Writes() == 1, "验证失败不该写记录");
    }

    // 损坏记录：不猜测目标集，以 E-CORE-69 停机
    void CaseCorruptRecordStops() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSet(confirmed.data(), 0);
        Bytes copy(BootState::kCopyBytes, 0);
        copy[0] = 0x42;
        PutCopy(fixture, 0, copy);
        PutCopy(fixture, BootState::kCopyBytes, copy);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-69", "损坏记录必须以 E-CORE-69 停机：" + Text(reason));
        Require(selected.setDirectory[0] == '\0', "损坏记录不该定下目标集");
        Require(fixture.Medium().Writes() == 0, "损坏记录不该写盘");
    }

    // Pending（运行期发布方在开发期的替身）进入候选：写 Trying、尝试身份取代次、次数递减
    void CasePendingStartsCandidate() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 7);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries), generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "候选选版失败：" + Text(reason));
        RequireSelected(selected, candidate.data(), 2);
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "Trying 记录必须读得回来");
        Require(selected.attemptId == read.generation, "试启动身份必须是 Trying 记录的代次");
        Require(read.state == State::Trying, "候选通过后必须写 Trying");
        Require(read.triesRemaining == BootState::kDefaultTries - 1, "尝试次数必须递减一次");
        Require(read.attemptId == read.generation && read.attemptId == 2, "Trying 的尝试身份必须取写入代次");
        Require(read.seenSecurityVersion == 7, "已见最高版本必须在同一份记录里抬升");
        Require(std::memcmp(read.candidateBuildId, candidate.data(), 32) == 0, "Trying 必须保留候选集");
        Require(std::memcmp(read.confirmedBuildId, confirmed.data(), 32) == 0, "Trying 必须保留已知良好集");
        Require(fixture.Diagnostics().empty(), "候选通过不该有诊断");
    }

    // Trying 且还有剩余次数：再写一次 Trying、次数再递减、尝试身份更新
    void CaseTryingRetries() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Trying, candidate.data(), confirmed.data(), 2), generation), "Trying 记录写入失败");
        Require(generation == 1, "第一次写入的代次必须是 1");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "重试选版失败：" + Text(reason));
        RequireSelected(selected, candidate.data(), 2);
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "重试后必须能读出记录");
        Require(read.state == State::Trying && read.triesRemaining == 1, "重试必须再写 Trying 并把次数减到 1");
        Require(read.attemptId == 2, "重试的尝试身份必须是新的写入代次");
    }

    // Trying 且次数用尽：写 Failed 并回退已知良好集
    void CaseTryingExhausted() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Trying, candidate.data(), confirmed.data(), 0), generation), "Trying 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "回退选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-73");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "Failed 记录必须读得回来");
        Require(read.state == State::Failed && read.triesRemaining == 0, "次数用尽必须写 Failed");
        Require(read.attemptId == 0, "Failed 的尝试身份必须为零");
        Require(std::memcmp(read.candidateBuildId, candidate.data(), 32) == 0, "Failed 必须保留候选集");
        Require(std::memcmp(read.confirmedBuildId, confirmed.data(), 32) == 0, "Failed 必须保留已知良好集");
    }

    // Pending 带回的可用次数为零：写 Failed 并回退已知良好集
    void CasePendingNoTries() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), 0), generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "回退选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-73");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "Failed 记录必须读得回来");
        Require(read.state == State::Failed, "次数为零必须写 Failed");
    }

    // 候选验证失败：写 Failed 并回退已知良好集，原因随诊断带出
    void CaseCandidateVerifyFailure() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        fixture.FailSet(candidate.data(), "E-CORE-62", "The kernel set manifest is invalid");
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries), generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "回退选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-72");
        Require(fixture.Diagnostics().front().detail == "The kernel set manifest is invalid", "候选失败的原因必须随诊断带出");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "Failed 记录必须读得回来");
        Require(read.state == State::Failed, "候选验证失败必须写 Failed");
        Require(read.triesRemaining == 0, "Failed 的剩余次数必须归零");
    }

    // 候选验证失败且没有已知良好集：写 Failed 后启动缺省集
    void CaseCandidateFailureWithoutConfirmed() {
        Fixture fixture;
        const auto candidate = Id(0x22);
        const auto defaultId = Id(0x0D);
        fixture.AddSet(candidate.data(), 4);
        fixture.AddSet(defaultId.data(), 0);
        fixture.FailSet(candidate.data(), "E-CORE-64", "The manifest signature doesn't verify");
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), nullptr, BootState::kDefaultTries), generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "缺省集直启失败：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-72");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "Failed 记录必须读得回来");
        Require(read.state == State::Failed, "没有良好集也要留下 Failed");
    }

    // 回退到缺省集时带设备已见最高版本：低于已见最高的缺省集被拒绝，不定下目标集
    void CaseDefaultSetRollback() {
        Fixture fixture;
        const auto candidate = Id(0x22);
        const auto defaultId = Id(0x0D);
        fixture.AddSet(candidate.data(), 4);
        fixture.AddSet(defaultId.data(), 3);          // 低于设备已见最高
        fixture.FailSet(candidate.data(), "E-CORE-64", "The manifest signature doesn't verify");
        Record pending = CandidateRecord(State::Pending, candidate.data(), nullptr, BootState::kDefaultTries);
        pending.seenSecurityVersion = 9;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(pending, generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-66", "低于已见最高的缺省集必须以 E-CORE-66 拒绝：" + Text(reason));
        Require(!selected.verified, "拒绝时不该定下已验证的目标集");
        RequireDiagnostic(fixture, "E-CORE-72");
        bool sawDefault = false;
        for (const auto& call : fixture.Calls()) {
            if (call.directory != SetDirectory(defaultId.data())) continue;
            sawDefault = true;
            Require(call.seenSecurityVersion == 9, "缺省集必须带设备已见最高版本验证");
        }
        Require(sawDefault, "缺省集没有进入清单验证");
    }

    // 没有状态记录时缺省集只做内嵌下限检查：已见最高按 0 传（规格第六节第 4 条）
    void CaseDefaultSetWithoutRecord() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 3);          // 比任何记录都低：没有记录时不该被拦
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "空区域直启失败：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        Require(fixture.Calls().size() == 1 && fixture.Calls().front().seenSecurityVersion == 0, "无记录时必须以 0 做防回滚比较");
    }

    // 写不成立就不进入候选：介质上仍是 Pending，回退已知良好集
    void CaseCandidateWriteFailure() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries), generation), "Pending 记录写入失败");
        const Bytes before = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes * 2);
        fixture.Medium().FailWrite(true);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "回退选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-70");
        Require(fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes * 2) == before, "写不成立时不该改动介质");
    }

    // Failed 记录：直接选已知良好集，不再碰候选
    void CaseFailedSelectsConfirmed() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 0);
        fixture.AddSet(candidate.data(), 4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Failed, candidate.data(), confirmed.data(), 0), generation), "Failed 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "Failed 选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0);
        Require(fixture.Medium().Writes() == 1, "Failed 不该写记录");
        Require(fixture.Diagnostics().empty(), "Failed 选版不该有诊断");
        Require(fixture.Calls().size() == 1 && fixture.Calls().front().directory == SetDirectory(confirmed.data()), "Failed 只应验证已知良好集");
    }

    // 选版启用前的扁平缺省落点：直启、不读状态区域、不验证清单
    void CaseFlatDefaultPath() {
        Fixture fixture;
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(kFlatKernelPath, selected);
        Require(reason.code == nullptr, "扁平落点选版失败：" + Text(reason));
        Require(std::string(selected.kernelPath) == kFlatKernelPath, "扁平落点必须原样直启");
        Require(!selected.verified, "扁平落点没有集身份，不做清单验证");
        Require(selected.attemptId == 0, "扁平落点没有试启动身份");
        Require(fixture.Medium().Reads() == 0, "扁平落点不该读状态区域");
        Require(fixture.Calls().empty(), "扁平落点不该做清单验证");
    }

    // 缺省路径过长：以 E-CORE-69 停机，不碰介质
    void CaseDefaultPathTooLong() {
        Fixture fixture;
        const std::string path(600, 'a');
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(path.c_str(), selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-69", "过长路径必须以 E-CORE-69 停机：" + Text(reason));
        Require(fixture.Medium().Reads() == 0 && fixture.Medium().Writes() == 0, "过长路径不该碰介质");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckSelect: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 状态写入方的两个用例：与其余用例同属引导状态记录的回归，定义在文件末尾的替身块之后，
    // 替身块要等 Baleen::Crypto 的桩就位，所以这里先声明
    void CasePublishCandidate();
    void CaseConfirmAttempt();

    // 依次跑全部用例
    void Execute() {
        Case("空区域按无记录处理", CaseEmptyArea);
        Case("没有状态区域按无记录处理", CaseNoStateArea);
        Case("读状态区域失败按无记录处理", CaseStateReadFailure);
        Case("首次写入落副本 A", CaseFirstWriteLandsInCopyA);
        Case("第二次写入落副本 B", CaseSecondWriteRotatesToCopyB);
        Case("第三次写入落回副本 A", CaseThirdWriteRotatesBackToCopyA);
        Case("一份副本写坏仍取另一份", CaseOneCopyCorrupt);
        Case("两份都写坏判为损坏", CaseBothCopiesCorrupt);
        Case("两份有效但代次相等判为损坏", CaseEqualGenerations);
        Case("夹具按字节表构造的记录被接受", CaseRawCopyAccepted);
        Case("字段违规的副本一律拒绝", CaseMalformedCopiesRejected);
        Case("代次全 FF 拒绝写入", CaseGenerationOverflow);
        Case("只读介质不进入候选", CaseReadOnlyMedium);
        Case("写失败不改动当前有效副本", CaseWriteFailureKeepsOtherCopy);
        Case("读回不符按写失败处理", CaseReadBackMismatch);
        Case("副本级检查报告两份结论", CaseInspectCopies);
        Case("受控清零回到出厂形态", CaseClearArea);
        Case("按有效副本修复且不改变内容", CaseRepairCopies);
        Case("人工接受只允许下调已见版本", CaseAcceptSeenVersion);
        Case("发布候选：首次写入与保留既有状态", CasePublishCandidate);
        Case("提交确认：比较再提交", CaseConfirmAttempt);
        Case("掉电演练：部分写入不破坏另一份副本", CasePowerLossDrill);
        Case("写入顺序为写-刷盘-读回", CaseWriteFlushReadOrder);
        Case("刷盘失败按写失败降级", CaseFlushFailure);
        Case("Confirmed 选已知良好集", CaseConfirmedSelects);
        Case("Confirmed 抬升已见最高版本", CaseConfirmedLiftsSeenVersion);
        Case("Confirmed 验证失败返回致命原因", CaseConfirmedVerifyFailure);
        Case("损坏记录不猜测目标集", CaseCorruptRecordStops);
        Case("Pending 进入候选并写 Trying", CasePendingStartsCandidate);
        Case("Trying 未确认按剩余次数重试", CaseTryingRetries);
        Case("Trying 次数用尽写 Failed", CaseTryingExhausted);
        Case("Pending 次数为零写 Failed", CasePendingNoTries);
        Case("候选验证失败写 Failed 并回退", CaseCandidateVerifyFailure);
        Case("候选失败且无良好集启动缺省集", CaseCandidateFailureWithoutConfirmed);
        Case("缺省集回退受防回滚约束", CaseDefaultSetRollback);
        Case("无记录时缺省集只做下限检查", CaseDefaultSetWithoutRecord);
        Case("写不成立不进入候选", CaseCandidateWriteFailure);
        Case("Failed 直接选已知良好集", CaseFailedSelectsConfirmed);
        Case("扁平缺省落点直启", CaseFlatDefaultPath);
        Case("缺省路径过长停机", CaseDefaultPathTooLong);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckSelect: " << g_ran << " 项用例全部通过" << std::endl;
    }
}

// 清单验证替身：按夹具登记的集目录返回结果，并核对调用方给的期望标识
// 真实的清单结构、签名与摘要验证由 ManifestVerify.cpp 承担，不在本工具里重复实现
namespace Baleen {
    namespace Core {
        Diagnostic VerifyKernelSet(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, ManifestFacts& facts) {
            (void)fs;
            if (Fixture::Current() == nullptr) return Diagnostic{"E-CORE-61", "Read the kernel set manifest"};
            return Fixture::Current()->Verify(setDirectory, expectedBuildId, seenSecurityVersion, revocation, facts);
        }
    }

    // 诊断输出替身：不装控制台，把诊断行收进夹具供断言
    void WriteDiagnostic(PrintTargets::Tag tag, const Diagnostic& diagnostic, const char* detail) {
        if (Fixture::Current() == nullptr) return;
        Fixture::Current()->Capture(tag, diagnostic, detail);
    }
}

// 信任锚与验签替身：选版把这两个函数交给吊销记录加载用，本工具的介质上没有吊销记录
// （空文件系统在结构检查之前就返回 Missing），所以这里只为链接提供同一组符号；
// 真实的锚查找、验签与吊销判定由 CheckRevocation 与 CheckCrypto 覆盖，不在本工具里重复实现
namespace Baleen {
    namespace Crypto {
        // 按编号取公钥：没有锚表，一律不可信
        const uint8_t* FindTrustAnchor(uint8_t keyId) {
            (void)keyId;
            return nullptr;
        }

        // 验证签名：本工具不提供验签能力
        bool Ed25519Verify(const uint8_t* publicKey, const uint8_t* signature, const uint8_t* message, uint32_t messageBytes) {
            (void)publicKey;
            (void)signature;
            (void)message;
            (void)messageBytes;
            return false;
        }
    }
}

// 状态写入方的用例：与上面的用例同属引导状态记录的回归，替身块之后重新打开匿名命名空间，
// 声明在 Execute 之前，定义在这里
namespace {
    // 发布候选：运行期发布方（Updater）的写入步骤——空区域首次写入，已有记录保留良好集、已见版本与撤销事实
    void CasePublishCandidate() {
        const auto candidate = Id(0x22);
        std::array<uint8_t, 32> digest{};
        FillDigest(digest.data(), candidate.data());

        // 空区域：首次写入取代次 1，没有已知良好集可保留
        {
            Fixture fixture;
            uint64_t generation = 0;
            Require(BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, candidate.data(), digest.data(), BootState::kDefaultTries, generation), "空区域发布候选应成功");
            Require(generation == 1, "首次写入的代次应为 1");
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "发布后应能读回记录");
            Require(read.state == State::Pending && read.triesRemaining == BootState::kDefaultTries, "发布后应是 Pending 且带回默认尝试次数");
            Require(std::memcmp(read.candidateBuildId, candidate.data(), 32) == 0, "读回的候选标识不符");
            Require(std::memcmp(read.confirmedBuildId, Id(0x00).data(), 32) == 0, "首次发布不应带已知良好集");
        }
        // 已有记录：只换候选与剩余次数，良好集、已见版本与撤销事实原样保留
        {
            Fixture fixture;
            const auto confirmed = Id(0x11);
            Record base = ConfirmedRecord(confirmed.data(), 9);
            base.revocation.epoch = 4;
            base.revocation.minimumSecurityVersion = 7;
            base.revocation.revokedKeyMask[0] = 0x08;
            uint64_t generation = 0;
            Require(fixture.WriteRecord(base, generation), "基线记录写入失败");
            const uint64_t before = generation;
            Require(BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, candidate.data(), digest.data(), 5, generation), "已有记录上发布候选应成功");
            Require(generation == before + 1, "发布的代次应加一");
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "发布后应能读回");
            Require(std::memcmp(read.confirmedBuildId, confirmed.data(), 32) == 0, "发布候选不应丢掉已知良好集");
            Require(read.seenSecurityVersion == 9, "发布候选不应丢掉已见最高版本");
            Require(read.revocation.epoch == 4 && read.revocation.minimumSecurityVersion == 7 && read.revocation.revokedKeyMask[0] == 0x08, "发布候选不应丢掉撤销事实");
            Require(read.state == State::Pending && read.triesRemaining == 5, "发布后应是带指定次数的 Pending");
            Require(std::memcmp(read.candidateBuildId, candidate.data(), 32) == 0, "候选字段应换成新候选");
        }
        // 参数不合法：拒绝且不落盘
        {
            Fixture fixture;
            uint64_t generation = 0;
            Require(!BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, candidate.data(), digest.data(), 0, generation), "尝试次数为 0 应被拒绝");
            Require(!BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, candidate.data(), digest.data(), 200, generation), "尝试次数超上限应被拒绝");
            Require(!BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, Id(0x00).data(), digest.data(), 3, generation), "候选标识全零应被拒绝");
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Empty, "被拒绝的发布不应落盘");
        }
    }

    // 提交确认：运行期确认方（Init）的写入步骤——比较再提交，候选一次转成已知良好集
    void CaseConfirmAttempt() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSet(confirmed.data(), 5);
        fixture.AddSet(candidate.data(), 7);
        std::array<uint8_t, 32> digest{};
        FillDigest(digest.data(), candidate.data());
        uint64_t generation = 0;
        Require(BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, candidate.data(), digest.data(), BootState::kDefaultTries, generation), "空区域发布候选应成功");

        // 记录还是 Pending：确认不成立
        Require(!BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, 1, generation), "Pending 记录上的确认应被拒绝");
        // 选一次：写 Trying，尝试身份取写入代次
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(candidate.data()).c_str(), selected);
        Require(reason.code == nullptr, "候选选版失败：" + Text(reason));
        Require(selected.attemptId == 2, "试启动身份应取写入代次 2");
        // 身份不符：不写
        Require(!BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, selected.attemptId + 1, generation), "身份不符的确认应被拒绝");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Trying, "被拒绝的确认不应改动记录");
        // 身份相符：一次写入提交，候选转成已知良好集
        Require(BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, selected.attemptId, generation), "身份相符的确认应成功");
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Confirmed, "确认后应是 Confirmed");
        Require(std::memcmp(read.confirmedBuildId, candidate.data(), 32) == 0, "确认应把候选转成已知良好集");
        Require(std::memcmp(read.candidateBuildId, Id(0x00).data(), 32) == 0, "确认后候选字段应清零");
        Require(read.attemptId == 0 && read.triesRemaining == 0, "确认后尝试身份与剩余次数应清零");
        // 迟到或重复的确认：记录已不是 Trying，拒绝且不写新代次
        Require(!BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, selected.attemptId, generation), "已确认的记录再次确认应被拒绝");
        Record again;
        Require(fixture.ReadRecord(again) == BootState::ReadResult::Ok && again.generation == read.generation, "被拒绝的确认不应写新代次");
    }
}

// 入口：跑全部用例
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string arg = argv[1];
            if (arg == "-h" || arg == "--help") {
                std::cout <<
                    "CheckSelect — BaleenCore 选版与引导状态记录的宿主侧检查\n"
                    "\n"
                    "用法：CheckSelect\n"
                    "\n"
                    "检查对象是 Packages/Baleen/Core 的 SelectSet.cpp 与 BootState.cpp（宿主侧编译）\n"
                    "清单验证与诊断输出由本工具的替身提供；Pending 记录由本工具按同一格式构造，\n"
                    "是运行期发布方（Updater）尚未实现时候选路径的可复现入口\n";
                return 0;
            }
            Fail("无法识别的参数：" + arg);
        }
        Execute();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckSelect: " << error.what() << '\n';
        return 1;
    }
}
