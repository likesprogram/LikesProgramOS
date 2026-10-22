/* main.cpp
    CheckRevision：引导状态记录 ModuleRevision 与模块修订选择 / 回退的宿主侧检查

    检查对象是目标侧源码：Packages/Common/Src/BootState.cpp 与 Packages/Baleen/Core/Src/SelectSet.cpp，
    按宿主选项编译后直接运行——记录字节表、修订选择、两阶段回退都是将要交付的那份实现
    清单验证（VerifyKernelSet）与当前列表修订号（ReadBootListRevision）由本工具提供替身：
    本工具核对的是“选版按哪一份来源解析修订、失败怎么回退”，真实的签名、结构与摘要判定
    由 CheckRevocation 与实机路径覆盖
    诊断输出（WriteDiagnostic）同样由替身收进夹具供断言
*/

#include <BootState.hpp>
#include <LayoutDescriptor.hpp>
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
    namespace RV = Baleen::Core::Revocation;

    // 引导状态区域的字节偏移与长度：与布局描述的默认值一致
    constexpr uint64_t kStateOffset = 0x21000;
    constexpr uint32_t kStateBytes = 4096;
    // 夹具介质容量：覆盖状态区域并留出余量
    constexpr uint64_t kMediumBytes = 0x24000;
    // 集根与集内内核产物名：与系统卷包的落点约定一致
    constexpr const char* kSetRoot = "/LikesProgram/System/KernelSets/";
    constexpr const char* kKernelName = "LikesProgram.os";
    // 记录字段偏移：由记录字节表固定，不随实现变化
    constexpr uint32_t kFieldGeneration = 0x008;
    constexpr uint32_t kFieldModuleRevision = 0x0D8;
    constexpr uint32_t kFieldReservedTail = 0x0E0;
    constexpr uint32_t kFieldChecksum = 0x0EC;
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 16;
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
    // 集内内核镜像路径：布局描述给出的缺省集路径形态
    std::string KernelPath(const uint8_t* buildId) { return SetDirectory(buildId) + "/" + kKernelName; }
    // 旧修订快照的清单文件名：Manifest.<rev>.mft
    std::string SnapshotManifestName(uint64_t revision) { return "Manifest." + std::to_string(revision) + ".mft"; }
    // 旧修订快照的列表文件名：Modules.<rev>.list
    std::string SnapshotBootListName(uint64_t revision) { return "Modules." + std::to_string(revision) + ".list"; }
    // 夹具侧的清单身份摘要：夹具用集标识派生，只要求逐字节稳定
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

    // 内存块设备：承载状态区域并支持可写性与写失败注入
    // 逻辑块固定 512 字节（覆盖记录的正常形态；4Kn 的整块读—改—写由 CheckSelect 覆盖）
    class MemoryMedium final : public Storage::BlockDevice {
    public:
        // 构造：容量与可写性
        MemoryMedium(uint64_t bytes, bool writable) : m_bytes(static_cast<std::size_t>(bytes), 0), m_writable(writable) {}
        // 逻辑块大小
        uint32_t BlockBytes() const override { return 512; }
        // 设备容量
        uint64_t CapacityBytes() const override { return m_bytes.size(); }
        // 是否可写
        bool Writable() const override { return m_writable; }
        // 读一段介质内容
        Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override {
            if (offset > m_bytes.size() || bytes > m_bytes.size() - offset) return Storage::Error::InvalidArgument;
            ++m_reads;
            if (m_failRead) return Storage::Error::Io;
            std::memcpy(buffer, m_bytes.data() + offset, bytes);
            return Storage::Error::Ok;
        }
        // 写一段介质内容；写失败注入让“写返回错误”可复现
        Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override {
            if (offset > m_bytes.size() || bytes > m_bytes.size() - offset) return Storage::Error::InvalidArgument;
            ++m_writes;
            if (!m_writable) return Storage::Error::ReadOnly;
            if (m_failWrite) return Storage::Error::Io;
            std::memcpy(m_bytes.data() + offset, buffer, bytes);
            return Storage::Error::Ok;
        }
        // 刷新写缓存：内存介质提交即持久
        Storage::Error Flush() override { return Storage::Error::Ok; }
        // 注入写失败
        void FailWrite(bool value) { m_failWrite = value; }
        // 注入读失败
        void FailRead(bool value) { m_failRead = value; }
        // 写尝试次数：包含返回失败的尝试
        unsigned Writes() const { return m_writes; }
        // 读调用次数
        unsigned Reads() const { return m_reads; }
        // 取介质内容的一段副本；越界即中止
        Bytes Slice(uint64_t offset, uint32_t bytes) const {
            Require(offset <= m_bytes.size() && bytes <= m_bytes.size() - offset, "夹具读取越界");
            return Bytes(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), m_bytes.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
        }
        // 直接改写介质内容：用于构造损坏副本与非法记录
        void Patch(uint64_t offset, const uint8_t* data, uint32_t bytes) {
            Require(offset <= m_bytes.size() && bytes <= m_bytes.size() - offset, "夹具写入越界");
            std::memcpy(m_bytes.data() + offset, data, bytes);
        }
    private:
        Bytes m_bytes;              // 介质内容
        bool m_writable = true;     // 是否可写
        bool m_failWrite = false;   // 写失败注入
        bool m_failRead = false;    // 读失败注入
        unsigned m_writes = 0;      // 写尝试次数
        unsigned m_reads = 0;       // 读调用次数
    };

    // 空文件系统：选版只把 FileSystem 转交给吊销记录加载，替身不使用它；默认所有路径都不存在
    class NullFileSystem final : public Storage::FileSystem {
    public:
        // 打开：一律按 NotFound 返回（空卷没有吊销记录）
        Storage::Error Open(const char* path, Storage::FileId& id) override {
            (void)path;
            (void)id;
            return Storage::Error::NotFound;
        }
    };

    // 夹具登记的一份清单来源：当前修订（清单名为空）或旧修订快照
    struct SourceEntry {
        std::string manifestName;   // 清单来源文件名：空表示当前修订的 Manifest.mft
        uint64_t revision = 1;      // 该来源列表头的 revision
        uint64_t securityVersion = 0;   // 该来源清单的安全版本
        bool verified = true;       // 清单验证是否通过
        Baleen::Diagnostic failure{};   // 验证不通过时的原因
    };

    // 一次清单验证调用的记录：核对选版按哪一份来源解析修订
    struct VerifyCall {
        std::string directory;      // 被验证的集目录
        std::string manifestName;   // 被验证的清单文件名（空表示当前修订）
        std::string bootListName;   // 被读取的列表文件名（空表示当前修订）
        uint64_t seenSecurityVersion = 0;   // 调用方给的已见最高安全版本
    };

    // 捕获的一条诊断行
    struct DiagLine {
        Baleen::PrintTargets::Tag tag;   // 标签
        std::string code;                // 编号
        std::string text;                // 原因文本
        std::string detail;              // 补充现场
    };

    // 校验夹具：内存介质、清单验证替身与诊断记录器
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
        // 登记一份清单来源：清单名为空表示当前修订；verified 为 false 时按给出的编号与文本失败
        void AddSource(const char* manifestName, uint64_t revision, uint64_t securityVersion, bool verified = true, const char* code = nullptr, const char* text = nullptr) {
            SourceEntry source;
            source.manifestName = manifestName == nullptr ? "" : manifestName;
            source.revision = revision;
            source.securityVersion = securityVersion;
            source.verified = verified;
            source.failure = Baleen::Diagnostic{code, text};
            m_sources.push_back(source);
        }
        // 设置当前修订列表的修订号：修订选择用它判断目标修订是不是当前修订
        void SetCurrentRevision(uint64_t revision) { m_currentRevision = revision; m_hasCurrentRevision = true; }
        // 清掉当前修订的答案：模拟当前列表读不到
        void ClearCurrentRevision() { m_hasCurrentRevision = false; }
        // 替身读当前修订列表的修订号
        bool CurrentRevision(uint64_t& revision) const {
            if (!m_hasCurrentRevision) return false;
            revision = m_currentRevision;
            return true;
        }
        // 替身的验证入口：按清单来源命中登记条目，返回验证结论并填出清单事实
        // 真实的签名、结构与摘要判定由 CheckRevocation 覆盖；这里核对的是“选了哪一份来源、传了什么”
        Baleen::Diagnostic Verify(const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, Baleen::Core::ManifestFacts& facts, const Baleen::Core::ManifestSource& source) {
            VerifyCall call;
            call.directory = setDirectory;
            call.manifestName = source.manifestName == nullptr ? "" : source.manifestName;
            call.bootListName = source.bootListName == nullptr ? "" : source.bootListName;
            call.seenSecurityVersion = seenSecurityVersion;
            m_calls.push_back(call);
            for (const auto& entry : m_sources) {
                if (entry.manifestName != call.manifestName) continue;
                if (!entry.verified) return entry.failure;
                // 防回滚：清单安全版本低于设备已见最高即拒绝（与真实实现同一条判据）
                if (entry.securityVersion < seenSecurityVersion) return Baleen::Diagnostic{"E-CORE-66", "The manifest SecurityVersion is below what the device or the bootloader allows"};
                std::memcpy(facts.buildId, expectedBuildId, 32);
                FillDigest(facts.manifestDigest, expectedBuildId);
                facts.securityVersion = entry.securityVersion;
                facts.bootListRevision = entry.revision;
                facts.moduleMajor = 1;
                facts.artifactCount = 1;
                facts.manifestBytes = sizeof(m_manifestCopy);
                facts.manifestCopy = m_manifestCopy;
                facts.bootListCopy = m_bootListCopy;
                facts.bootListBytes = BuildBootListCopy(entry.revision);
                facts.listedArtifacts = nullptr;
                facts.listedArtifactCount = 0;
                Require(std::string(kKernelName).size() <= KernelSet::ManifestLayout::kEntryPathMaxBytes, "夹具内核路径超过上限");
                std::memcpy(facts.kernelPath, kKernelName, std::string(kKernelName).size());
                facts.kernelPath[kKernelName != nullptr ? std::strlen(kKernelName) : 0] = 0;
                facts.kernelPathBytes = static_cast<uint16_t>(std::strlen(kKernelName));
                // 清单副本：开头是清单标记，BuildId 落在清单头的 BuildId 字段偏移
                std::memset(m_manifestCopy, 0, sizeof(m_manifestCopy));
                std::memcpy(m_manifestCopy, "LPMS", 4);
                std::memcpy(m_manifestCopy + KernelSet::ManifestLayout::kFieldBuildId, expectedBuildId, 32);
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
        // 第一阶段选版
        Baleen::Diagnostic Begin(const char* layoutKernelPath, Baleen::Core::SelectedSet& selected) {
            return Baleen::Core::BeginSetSelection(m_fileSystem, m_medium, kStateOffset, kStateBytes, layoutKernelPath, 0, selected);
        }
        // 第二阶段：装载或预装失败后的回退
        Baleen::Diagnostic AfterLoadFailure(const char* layoutKernelPath, const Baleen::Core::SelectedSet& failed, bool preloadFailed, const Baleen::Diagnostic& failure, Baleen::Core::SelectedSet& selected) {
            return Baleen::Core::SelectAfterLoadFailure(m_fileSystem, m_medium, kStateOffset, kStateBytes, layoutKernelPath, 0, failed, preloadFailed, failure, selected);
        }
        // 按生产写入方的同一套接口写一份记录
        bool WriteRecord(const Record& record, uint64_t& generation) {
            return BootState::Write(m_medium, kStateOffset, kStateBytes, record, generation);
        }
        // 读一份记录
        BootState::ReadResult ReadRecord(Record& record) {
            return BootState::Read(m_medium, kStateOffset, kStateBytes, record);
        }
        // 槽位级检查
        BootState::Inspection InspectRecords() { return BootState::Inspect(m_medium, kStateOffset, kStateBytes); }
        // 受控恢复：清零全部槽位
        bool ClearArea() { return BootState::Clear(m_medium, kStateOffset, kStateBytes); }
        // 受控恢复：按有效副本修复
        bool RepairArea(uint64_t& generation) { return BootState::Repair(m_medium, kStateOffset, kStateBytes, generation); }
        // 人工接受：下调已见最高安全版本
        bool AcceptSeen(uint64_t version, uint64_t& generation) { return BootState::AcceptSeenSecurityVersion(m_medium, kStateOffset, kStateBytes, version, generation); }
    private:
        // 填列表副本：与真实列表同一个固定头，revision 按来源填；返回字节数
        uint32_t BuildBootListCopy(uint64_t revision) {
            const std::string text = "LPML 1\nmajor 1\nrevision " + std::to_string(revision) + "\n";
            Require(text.size() < sizeof(m_bootListCopy), "夹具列表副本缓冲不足");
            std::memset(m_bootListCopy, 0, sizeof(m_bootListCopy));
            std::memcpy(m_bootListCopy, text.data(), text.size());
            return static_cast<uint32_t>(text.size());
        }
        static Fixture* s_current;          // 替身当前的夹具
        MemoryMedium m_medium;              // 夹具介质
        NullFileSystem m_fileSystem;        // 空文件系统
        bool m_hasCurrentRevision = false;  // 当前列表修订号是否有答案
        uint64_t m_currentRevision = 0;     // 当前列表修订号
        std::vector<SourceEntry> m_sources;    // 登记的清单来源
        std::vector<VerifyCall> m_calls;       // 清单验证调用记录
        std::vector<DiagLine> m_diagnostics;   // 捕获的诊断行
        uint8_t m_manifestCopy[0x100] = {};    // 清单副本（替身）：验证通过时 facts 指向它
        uint8_t m_bootListCopy[0x100] = {};    // 列表副本（替身）
    };

    Fixture* Fixture::s_current = nullptr;

    // 已确认记录：候选为空
    Record ConfirmedRecord(const uint8_t* confirmedId, uint64_t seenSecurityVersion = 0) {
        Record record;
        record.state = State::Confirmed;
        std::memcpy(record.confirmedBuildId, confirmedId, 32);
        FillDigest(record.confirmedManifestDigest, confirmedId);
        record.seenSecurityVersion = seenSecurityVersion;
        return record;
    }

    // 候选记录：Pending / Trying / Failed；confirmedId 为空时不带已知良好集
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

    // 夹具侧的槽位构造：按记录字节表拼接一个 512 字节槽位，校验和用夹具自己的 CRC-32
    // 生产代码不会产生字段违规的记录，构造这类副本是为了核对读取方的逐项校验
    Bytes RawCopy(const Record& record, uint8_t stateValue, uint8_t flags, uint32_t reserved, bool checksumCorrect) {
        Bytes copy(BootState::kCopyBytes, 0);
        Put32(copy, 0x000, 0x54534C42u);                    // Magic "BLST"
        Put16(copy, 0x004, BootState::kVersion);            // Version
        Put16(copy, 0x006, BootState::kRecordBytes);        // Bytes
        Put64(copy, 0x008, record.generation);              // Generation
        copy[0x010] = stateValue;                           // State
        copy[0x011] = record.triesRemaining;                // TriesRemaining
        copy[0x012] = record.triesDone;                     // TriesDone
        copy[0x013] = flags;                                // Flags
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
        Put64(copy, kFieldModuleRevision, record.moduleRevision);   // ModuleRevision
        const uint32_t crc = FixtureCrc32(copy.data(), kFieldChecksum);
        Put32(copy, kFieldChecksum, checksumCorrect ? crc : (crc ^ 1u));
        return copy;
    }

    // 重算一份原始副本的校验和：改过字段的副本要重新修校验和，才是在测字段校验而不是校验和校验
    void FixChecksum(Bytes& copy) { Put32(copy, kFieldChecksum, FixtureCrc32(copy.data(), kFieldChecksum)); }

    // 把一份原始槽位写进指定的槽位偏移
    void PutCopy(Fixture& fixture, uint64_t copyOffset, const Bytes& copy) {
        Require(copy.size() == BootState::kCopyBytes, "原始副本长度不符");
        fixture.Medium().Patch(kStateOffset + copyOffset, copy.data(), static_cast<uint32_t>(copy.size()));
    }

    // 核对选中的集：目录、内核路径、标识、已验证、试启动身份与模块修订
    void RequireSelected(const Baleen::Core::SelectedSet& selected, const uint8_t* buildId, std::uint64_t attemptId, uint64_t moduleRevision, bool snapshot) {
        Require(std::string(selected.setDirectory) == SetDirectory(buildId), "选中的集目录不符：" + std::string(selected.setDirectory));
        Require(std::string(selected.kernelPath) == KernelPath(buildId), "选中的内核路径不符：" + std::string(selected.kernelPath));
        Require(std::memcmp(selected.buildId, buildId, 32) == 0, "选中的集标识不符");
        Require(selected.verified, "选中的集未经清单验证");
        Require(selected.attemptId == attemptId, "试启动身份不符：" + std::to_string(selected.attemptId));
        Require(selected.moduleRevision == moduleRevision, "选中的模块修订号不符：" + std::to_string(selected.moduleRevision));
        Require(selected.snapshot == snapshot, std::string("选中的清单来源不符：期望") + (snapshot ? "旧修订快照" : "当前修订"));
    }

    // 核对接手：失败时不得定下目标集
    void RequireNoSelected(const Baleen::Core::SelectedSet& selected) {
        Require(selected.setDirectory[0] == '\0', "失败时不得定下目标集");
        Require(!selected.verified, "失败时不得声称集已验证");
    }

    // 核对夹具里出现过的诊断编号与条数
    void RequireDiagnostic(const Fixture& fixture, const char* code, unsigned count = 1) {
        unsigned seen = 0;
        for (const auto& line : fixture.Diagnostics()) if (line.code == code) ++seen;
        Require(seen == count, std::string("诊断 ") + code + " 出现 " + std::to_string(seen) + " 次，期望 " + std::to_string(count) + " 次");
    }

    // —— 记录：ModuleRevision 的布局、校验与写方义务 ——

    // 记录布局：ModuleRevision 落在槽位 0xD8..0xE0，记录有效长度 0xF0、八槽环形不变
    void CaseRecordLayout() {
        static_assert(BootState::kRecordBytes == 0xF0, "记录有效长度固定为 0xF0");
        static_assert(BootState::kCopyBytes == 0x200, "一份槽位必须是 0x200 字节");
        static_assert(BootState::kSlots == 8, "状态区域必须是八槽环形");
        static_assert(kFieldModuleRevision == 0x0D8, "ModuleRevision 的偏移固定为 0xD8");
        // 写方向：字段必须落在槽位的 0xD8..0xE0
        {
            Fixture fixture;
            const auto buildId = Id(0x11);
            Record record = ConfirmedRecord(buildId.data());
            record.moduleRevision = 0x0102030405060708ull;
            uint64_t generation = 0;
            Require(fixture.WriteRecord(record, generation), "带 ModuleRevision 的记录必须能写入");
            const Bytes field = fixture.Medium().Slice(kStateOffset + kFieldModuleRevision, 8);
            for (unsigned i = 0; i < 8; ++i) {
                Require(field[i] == static_cast<uint8_t>(0x0102030405060708ull >> (8 * i)), "ModuleRevision 的落点必须是槽位偏移 0xD8");
            }
        }
        // 读方向：夹具按记录字节表构造 0xD8 起的 8 字节，读取方必须解出
        {
            Fixture fixture;
            const auto buildId = Id(0x11);
            Record record = ConfirmedRecord(buildId.data());
            record.generation = 3;
            record.moduleRevision = 0x1122334455667788ull;
            PutCopy(fixture, 0, RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true));
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "带非零 ModuleRevision 的记录必须被接受");
            Require(read.moduleRevision == 0x1122334455667788ull, "ModuleRevision 必须从槽位偏移 0xD8 解出");
        }
    }

    // 保留区：0x0E0..0x0EC 的 12 字节必须全零，任一字节非零按损坏拒绝
    void CaseReservedTail() {
        const auto buildId = Id(0x11);
        for (uint32_t offset = kFieldReservedTail; offset < kFieldChecksum; ++offset) {
            Fixture fixture;
            Record record = ConfirmedRecord(buildId.data());
            record.generation = 5;
            record.moduleRevision = 4;
            Bytes copy = RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true);
            copy[offset] = 1;
            FixChecksum(copy);
            PutCopy(fixture, 0, copy);
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Corrupt,
                "保留区字节 0x" + std::to_string(offset) + " 非零必须按损坏拒绝");
        }
    }

    // 校验和覆盖 ModuleRevision：改 0xD8 不重算即拒；重算后接受并解出新值
    void CaseChecksumCoversModuleRevision() {
        Fixture fixture;
        const auto buildId = Id(0x11);
        Record record = ConfirmedRecord(buildId.data());
        record.generation = 2;
        Bytes copy = RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true);
        Put64(copy, kFieldModuleRevision, 9);   // 只改字段、校验和仍是旧值
        PutCopy(fixture, 0, copy);
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Corrupt, "ModuleRevision 改动后校验和对不上必须拒绝");
        FixChecksum(copy);
        PutCopy(fixture, 0, copy);
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "校验和重算后必须接受");
        Require(read.moduleRevision == 9, "校验和覆盖范围内必须解出新的 ModuleRevision");
    }

    // 读写往返：0（无修订）与最大修订号都必须原样读回
    void CaseReadWriteRoundTrip() {
        const auto buildId = Id(0x11);
        const uint64_t revisions[] = {0, 7, 0xFFFFFFFFFFFFFFFFull};
        for (const uint64_t revision : revisions) {
            Fixture fixture;
            Record record = ConfirmedRecord(buildId.data());
            record.moduleRevision = revision;
            uint64_t generation = 0;
            Require(fixture.WriteRecord(record, generation), "第 " + std::to_string(revision) + " 号修订写入失败");
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "第 " + std::to_string(revision) + " 号修订必须可读");
            Require(read.moduleRevision == revision, "第 " + std::to_string(revision) + " 号修订必须原样读回");
        }
    }

    // 发布候选继承回退目标：已有 Confirmed 记录上的 ModuleRevision 不因候选发布而改动；空区域起步为 0
    void CasePublishInheritsRevision() {
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        std::array<uint8_t, 32> digest{};
        FillDigest(digest.data(), candidate.data());
        {
            Fixture fixture;
            Record base = ConfirmedRecord(confirmed.data(), 9);
            base.moduleRevision = 5;
            uint64_t generation = 0;
            Require(fixture.WriteRecord(base, generation), "基线记录写入失败");
            Require(BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, candidate.data(), digest.data(), 3, generation), "发布候选失败");
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Pending, "发布后应停在 Pending");
            Require(read.moduleRevision == 5, "候选发布不得改动回退目标修订号");
        }
        {
            Fixture fixture;   // 空区域：没有已知良好侧，按无修订（0）起步
            uint64_t generation = 0;
            Require(BootState::PublishCandidate(fixture.Medium(), kStateOffset, kStateBytes, candidate.data(), digest.data(), 3, generation), "空区域发布候选失败");
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "发布后必须可读");
            Require(read.moduleRevision == 0, "空区域发布的 Pending 必须按无修订起步");
        }
    }

    // 确认提交写已确认修订：身份相符才写、修订号为零拒绝、写进 Confirmed 记录
    void CaseConfirmWritesRevision() {
        Fixture fixture;
        const auto candidate = Id(0x22);
        Record trying = CandidateRecord(State::Trying, candidate.data(), nullptr, 3);
        trying.moduleRevision = 5;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(trying, generation), "Trying 记录写入失败");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Trying, "Trying 记录必须可读");
        const uint64_t attempt = read.attemptId;
        Require(attempt == generation, "Trying 的尝试身份取写入代次");
        Require(!BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, attempt, 0, generation), "修订号为零的确认必须拒绝");
        Require(!BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, attempt + 1, 9, generation), "身份不符的确认必须拒绝");
        Require(BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, attempt, 9, generation), "身份相符的确认必须成功");
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Confirmed, "确认后应是 Confirmed");
        Require(read.moduleRevision == 9, "确认必须把本次装载所用修订号写进记录");
    }

    // 维护动作保留修订字段：Repair 重建槽位、AcceptSeen 下调已见版本都不动 ModuleRevision
    void CaseMaintenanceKeepsRevision() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        Record base = ConfirmedRecord(confirmed.data(), 9);
        base.moduleRevision = 7;
        base.revocation.epoch = 4;
        base.revocation.minimumSecurityVersion = 7;
        base.revocation.revokedKeyMask[0] = 0x08;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(base, generation), "第一份记录写入失败");
        Require(fixture.WriteRecord(base, generation), "第二份记录写入失败");
        // 写坏代次较小的槽位 A，再按有效副本修复
        Bytes slotA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        slotA[kFieldGeneration] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, slotA.data(), static_cast<uint32_t>(slotA.size()));
        uint64_t repaired = 0;
        Require(fixture.RepairArea(repaired), "按有效副本修复失败");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "修复后必须可读");
        Require(read.moduleRevision == 7, "修复必须原样保留 ModuleRevision");
        Require(read.revocation.epoch == 4 && read.revocation.minimumSecurityVersion == 7 && read.revocation.revokedKeyMask[0] == 0x08, "修复必须保留撤销事实");
        // 人工接受只下调已见版本，ModuleRevision 不动
        Require(fixture.AcceptSeen(3, repaired), "下调已见版本应成功");
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "接受后必须可读");
        Require(read.seenSecurityVersion == 3 && read.moduleRevision == 7, "人工接受不得改动 ModuleRevision");
    }

    // Clear 之后按当前列表修订重建：空区域首次启动的 Confirmed 记录写当前 Modules.list 的 revision
    void CaseClearRebuildsRevision() {
        Fixture fixture;
        const auto confirmed = Id(0x0D);
        fixture.AddSource("", 4, 5);
        fixture.SetCurrentRevision(4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data(), 0), generation), "铺底记录写入失败");
        Require(fixture.ClearArea(), "受控清零失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Begin(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "清零后缺省集选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0, 4, false);
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "重建的 Confirmed 记录必须可读");
        Require(read.state == State::Confirmed && read.generation == 1, "清零后重建必须是代次 1 的 Confirmed");
        Require(read.moduleRevision == 4, "重建记录必须按当前列表修订写 ModuleRevision");
    }

    // 旧记录（ModuleRevision == 0）兼容：按当前列表的 revision 解释，写回不升级字段
    void CaseOldRecordCompatibility() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSource("", 7, 0);
        fixture.SetCurrentRevision(7);
        Record old = ConfirmedRecord(confirmed.data());
        old.generation = 3;
        PutCopy(fixture, 0, RawCopy(old, static_cast<uint8_t>(State::Confirmed), 0, 0, true));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.moduleRevision == 0, "旧记录必须可读且按无修订");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Begin(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "旧记录选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0, 7, false);
        Require(fixture.Medium().Writes() == 0, "没有事实变化时不得把旧记录升级成带修订号");
        Require(fixture.Calls().size() == 1 && fixture.Calls().front().manifestName.empty(), "旧记录必须按当前修订取清单");
        // 维护动作同样保留 0：报告用的 Inspection 与 Repair 都不猜修订
        Require(fixture.InspectRecords().record.moduleRevision == 0, "槽位级检查必须报告旧记录的无修订");
        uint64_t generation = 0;
        Require(fixture.RepairArea(generation), "旧记录的修复应成功");
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.moduleRevision == 0, "修复必须保留旧记录的无修订字段");
    }

    // —— 修订选择算法：当前修订 / 旧修订快照 / 目标不可选 ——

    // 目标修订等于当前列表修订：取当前 Manifest.mft / Modules.list，不碰旧修订快照
    void CaseSelectCurrentRevision() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSource("", 5, 0);
        fixture.SetCurrentRevision(5);
        Record record = ConfirmedRecord(confirmed.data());
        record.moduleRevision = 5;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(record, generation), "记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Begin(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "当前修订选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0, 5, false);
        Require(fixture.Calls().size() == 1, "修订选择只应验证选中的那一份来源");
        Require(fixture.Calls().front().manifestName.empty() && fixture.Calls().front().bootListName.empty(), "目标等于当前修订时必须读当前清单与列表");
    }

    // 目标修订不等于当前列表修订：取 Manifest.<rev>.mft / Modules.<rev>.list
    void CaseSelectSnapshotRevision() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSource("", 5, 0);
        fixture.AddSource(SnapshotManifestName(2).c_str(), 2, 0);
        fixture.SetCurrentRevision(5);
        Record record = ConfirmedRecord(confirmed.data());
        record.moduleRevision = 2;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(record, generation), "记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Begin(KernelPath(confirmed.data()).c_str(), selected);
        Require(reason.code == nullptr, "旧修订选版失败：" + Text(reason));
        RequireSelected(selected, confirmed.data(), 0, 2, true);
        Require(fixture.Calls().size() == 1, "修订选择只应验证选中的那一份来源");
        Require(fixture.Calls().front().manifestName == SnapshotManifestName(2), "必须取旧修订快照 Manifest.2.mft，实际 " + fixture.Calls().front().manifestName);
        Require(fixture.Calls().front().bootListName == SnapshotBootListName(2), "必须取旧修订快照列表 Modules.2.list");
    }

    // 目标修订不可选：快照验签失败或快照文件不存在，都终止、不猜、不跨修订拼接、不写状态
    void CaseSelectTargetUnavailable() {
        const auto confirmed = Id(0x11);
        // 快照存在但验签失败：把验证原因原样返回
        {
            Fixture fixture;
            fixture.AddSource("", 5, 0);
            fixture.AddSource(SnapshotManifestName(2).c_str(), 2, 0, false, "E-CORE-64", "The manifest signature doesn't verify");
            fixture.SetCurrentRevision(5);
            Record record = ConfirmedRecord(confirmed.data());
            record.moduleRevision = 2;
            uint64_t generation = 0;
            Require(fixture.WriteRecord(record, generation), "记录写入失败");
            Baleen::Core::SelectedSet selected;
            const auto reason = fixture.Begin(KernelPath(confirmed.data()).c_str(), selected);
            Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-64", "快照验签失败必须终止：" + Text(reason));
            RequireNoSelected(selected);
            Require(fixture.Medium().Writes() == 1, "目标不可选时不得写记录");
        }
        // 快照根本不存在：同样终止，不落到当前修订或别的修订
        {
            Fixture fixture;
            fixture.AddSource("", 5, 0);
            fixture.SetCurrentRevision(5);
            Record record = ConfirmedRecord(confirmed.data());
            record.moduleRevision = 2;
            uint64_t generation = 0;
            Require(fixture.WriteRecord(record, generation), "记录写入失败");
            Baleen::Core::SelectedSet selected;
            const auto reason = fixture.Begin(KernelPath(confirmed.data()).c_str(), selected);
            Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-61", "快照缺失必须终止：" + Text(reason));
            RequireNoSelected(selected);
            Require(fixture.Calls().size() == 1 && fixture.Calls().front().manifestName == SnapshotManifestName(2), "目标修订只解析一次快照来源，不回退去试当前修订");
        }
    }

    // —— 两阶段回退：防环、候选预装失败、良好集失败终止、只读介质降级 ——

    // 防环：回退目标与失败候选是同一集、同一修订时不重试同一组合，按良好集不可用终止
    void CaseFallbackSameCombination() {
        Fixture fixture;
        const auto same = Id(0x33);
        fixture.AddSource("", 5, 0);
        fixture.SetCurrentRevision(5);
        Record pending = CandidateRecord(State::Pending, same.data(), same.data(), BootState::kDefaultTries);
        pending.moduleRevision = 5;   // 回退目标 = 候选集 + 候选修订
        uint64_t generation = 0;
        Require(fixture.WriteRecord(pending, generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        Require(fixture.Begin(KernelPath(same.data()).c_str(), selected).code == nullptr, "候选选版失败");
        RequireSelected(selected, same.data(), 2, 5, false);
        const Baleen::Diagnostic loadFailure{"E-CORE-57", "Load the kernel image into memory"};
        Baleen::Core::SelectedSet fallback;
        const auto reason = fixture.AfterLoadFailure(KernelPath(same.data()).c_str(), selected, false, loadFailure, fallback);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-57", "回退目标等于失败候选时必须按良好集不可用终止：" + Text(reason));
        RequireNoSelected(fallback);
        Require(fixture.Calls().size() == 1, "不得重试同一组合");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Failed, "候选失败仍要留下 Failed 现场");
    }

    // 候选预装失败：写 Failed、WARN E-CORE-98，按记录里的回退目标修订改选上一修订
    void CaseCandidatePreloadFailureFallback() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSource("", 5, 4);
        fixture.AddSource(SnapshotManifestName(2).c_str(), 2, 4);
        fixture.SetCurrentRevision(5);
        Record pending = CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries);
        pending.moduleRevision = 2;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(pending, generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto beginReason = fixture.Begin(KernelPath(confirmed.data()).c_str(), selected);
        Require(beginReason.code == nullptr, "候选选版失败：" + Text(beginReason));
        RequireSelected(selected, candidate.data(), 2, 5, false);
        const Baleen::Diagnostic preloadFailure{"E-CORE-96", "The module load list is invalid"};
        Baleen::Core::SelectedSet fallback;
        const auto reason = fixture.AfterLoadFailure(KernelPath(confirmed.data()).c_str(), selected, true, preloadFailure, fallback);
        Require(reason.code == nullptr, "候选预装失败必须回退而不是终止：" + Text(reason));
        RequireSelected(fallback, confirmed.data(), 0, 2, true);
        RequireDiagnostic(fixture, "E-CORE-98");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Failed, "候选预装失败必须写 Failed");
        Require(read.attemptId == 0 && read.triesRemaining == 0, "Failed 的尝试身份与剩余次数必须清零");
        Require(std::memcmp(read.candidateBuildId, candidate.data(), 32) == 0, "Failed 必须保留失败候选的现场");
        Require(read.moduleRevision == 2, "回退目标修订号必须原样保留");
    }

    // 已知良好集装载失败：没有可回退的目标，原样终止，不写状态转换
    void CaseKnownGoodFailureTerminates() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        fixture.AddSource("", 1, 0);
        fixture.SetCurrentRevision(1);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Baleen::Core::SelectedSet selected;
        Require(fixture.Begin(KernelPath(confirmed.data()).c_str(), selected).code == nullptr, "已知良好集选版失败");
        Require(selected.attemptId == 0, "确认集启动不是试启动");
        const Baleen::Diagnostic loadFailure{"E-CORE-57", "Load the kernel image into memory"};
        Baleen::Core::SelectedSet fallback;
        const auto reason = fixture.AfterLoadFailure(KernelPath(confirmed.data()).c_str(), selected, false, loadFailure, fallback);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-57", "已知良好集失败必须原样终止：" + Text(reason));
        RequireNoSelected(fallback);
        Require(fixture.Medium().Writes() == 1, "已知良好集失败不写状态转换");
    }

    // 只读介质（写不成 Failed）：WARN 后按降级路径回退已知良好集，介质上仍停 Trying
    void CaseReadOnlyDegrades() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        fixture.AddSource("", 1, 4);
        fixture.SetCurrentRevision(1);
        Record pending = CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(pending, generation), "Pending 记录写入失败");
        Baleen::Core::SelectedSet selected;
        Require(fixture.Begin(KernelPath(confirmed.data()).c_str(), selected).code == nullptr, "候选选版失败");
        Require(selected.attemptId != 0, "候选必须先写 Trying 再交权");
        // 写失败等价于只读介质：Failed 落不了盘，只能按降级路径回退
        fixture.Medium().FailWrite(true);
        const Baleen::Diagnostic preloadFailure{"E-CORE-94", "The module file header isn't valid"};
        Baleen::Core::SelectedSet fallback;
        const auto reason = fixture.AfterLoadFailure(KernelPath(confirmed.data()).c_str(), selected, true, preloadFailure, fallback);
        Require(reason.code == nullptr, "写不成 Failed 时应按降级路径继续回退：" + Text(reason));
        RequireSelected(fallback, confirmed.data(), 0, 1, false);
        RequireDiagnostic(fixture, "E-CORE-98");
        RequireDiagnostic(fixture, "E-CORE-70");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Trying, "写不成时介质上仍是 Trying，下次启动按未确认处理");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckRevision: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 依次跑全部用例
    void Execute() {
        Case("记录布局：ModuleRevision 落在 0xD8", CaseRecordLayout);
        Case("保留区 0x0E0..0x0EC 必须全零", CaseReservedTail);
        Case("Checksum 覆盖 ModuleRevision", CaseChecksumCoversModuleRevision);
        Case("ModuleRevision 读写往返", CaseReadWriteRoundTrip);
        Case("发布候选继承回退目标修订", CasePublishInheritsRevision);
        Case("确认提交写已确认修订", CaseConfirmWritesRevision);
        Case("维护动作保留 ModuleRevision", CaseMaintenanceKeepsRevision);
        Case("Clear 后按当前列表修订重建", CaseClearRebuildsRevision);
        Case("旧记录零修订兼容", CaseOldRecordCompatibility);
        Case("修订选择：目标等于当前修订", CaseSelectCurrentRevision);
        Case("修订选择：目标取旧修订快照", CaseSelectSnapshotRevision);
        Case("修订选择：目标不可选终止", CaseSelectTargetUnavailable);
        Case("回退：同一组合不重试", CaseFallbackSameCombination);
        Case("回退：候选预装失败改选上一修订", CaseCandidatePreloadFailureFallback);
        Case("回退：已知良好集失败终止", CaseKnownGoodFailureTerminates);
        Case("回退：只读介质降级", CaseReadOnlyDegrades);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckRevision: " << g_ran << "/" << kCaseCount << " cases passed" << std::endl;
    }
}

// 清单验证替身：按夹具登记的清单来源返回结论，并核对选版传了哪一份来源
// 真实的清单结构、签名与摘要验证由 ManifestVerify.cpp / CheckRevocation 承担，不在本工具里重复实现
namespace Baleen {
    namespace Core {
        Diagnostic VerifyKernelSet(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, ManifestFacts& facts, const ManifestSource& source) {
            (void)fs;
            (void)revocation;
            if (Fixture::Current() == nullptr) return Diagnostic{"E-CORE-61", "Read the kernel set manifest"};
            return Fixture::Current()->Verify(setDirectory, expectedBuildId, seenSecurityVersion, facts, source);
        }

        // 当前修订列表修订号的替身：生产实现读集目录里的 Modules.list 固定头；
        // 本工具没有存储层，按夹具登记的当前修订返回，供修订选择路由使用
        bool ReadBootListRevision(Storage::FileSystem& fs, const char* setDirectory, uint64_t& revision) {
            (void)fs;
            (void)setDirectory;
            if (Fixture::Current() == nullptr) return false;
            return Fixture::Current()->CurrentRevision(revision);
        }
    }

    // 诊断输出替身：不装控制台，把诊断行收进夹具供断言
    void WriteDiagnostic(PrintTargets::Tag tag, const Diagnostic& diagnostic, const char* detail) {
        if (Fixture::Current() == nullptr) return;
        Fixture::Current()->Capture(tag, diagnostic, detail);
    }
}

// 信任锚与验签替身：选版把这两个函数交给吊销记录加载用，本工具的介质上没有吊销记录
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

// 入口：跑全部用例
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string arg = argv[1];
            if (arg == "-h" || arg == "--help") {
                std::cout <<
                    "CheckRevision — 引导状态记录 ModuleRevision 与模块修订选择 / 回退的宿主侧检查\n"
                    "\n"
                    "用法：CheckRevision\n"
                    "\n"
                    "检查对象是 Packages/Common/Src/BootState.cpp 与 Packages/Baleen/Core/Src/SelectSet.cpp\n"
                    "（宿主侧编译同一份实现）；清单验证与当前列表修订号由本工具的替身提供，\n"
                    "真实的签名 / 结构 / 摘要判定由 CheckRevocation 与实机路径覆盖\n";
                return 0;
            }
            Fail("无法识别的参数：" + arg);
        }
        Execute();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckRevision: " << error.what() << '\n';
        return 1;
    }
}
