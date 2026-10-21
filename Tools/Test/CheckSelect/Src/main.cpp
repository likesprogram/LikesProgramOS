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

    // 引导状态区域的字节偏移与长度：与布局描述的默认值一致
    constexpr uint64_t kStateOffset = 0x21000;
    constexpr uint32_t kStateBytes = 4096;
    // 夹具介质容量：覆盖状态区域并留出余量（末尾几个字节用于越界注入）
    constexpr uint64_t kMediumBytes = 0x24000;
    // 集根与集内内核产物名：与系统卷包的落点约定一致
    constexpr const char* kSetRoot = "/LikesProgram/System/KernelSets/";
    constexpr const char* kKernelName = "LikesProgram.os";
    // 选版启用前的扁平落点：不再是可直启形态，选择器必须拒绝
    constexpr const char* kFlatKernelPath = "/LikesProgram/System/LikesProgram.os";
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 60;
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
    // 集内某个产物的完整卷内路径：集目录 + 清单登记的相对路径
    std::string ArtifactPath(const uint8_t* buildId, const std::string& relative) { return SetDirectory(buildId) + "/" + relative; }
    // 集内内核镜像路径（清单登记在集目录根上的默认形态）
    std::string KernelPath(const uint8_t* buildId) { return ArtifactPath(buildId, kKernelName); }
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
    // 逻辑块大小可配置：默认 512（512e），4Kn 用例取 4096；读写按与真实适配器相同的
    // 块设备契约校验偏移与长度，未按块对齐一律 InvalidArgument
    class MemoryMedium final : public Storage::BlockDevice {
    public:
        // 构造：容量、可写性与逻辑块大小
        MemoryMedium(uint64_t bytes, bool writable, uint32_t blockBytes = 512)
            : m_bytes(static_cast<std::size_t>(bytes), 0), m_blockBytes(blockBytes), m_writable(writable) {
            Require(blockBytes != 0 && (blockBytes & (blockBytes - 1)) == 0, "夹具逻辑块大小必须是 2 的幂");
        }
        // 逻辑块大小
        uint32_t BlockBytes() const override { return m_blockBytes; }
        // 设备容量
        uint64_t CapacityBytes() const override { return m_bytes.size(); }
        // 是否可写
        bool Writable() const override { return m_writable; }
        // 读一段介质内容：未按块对齐的请求按契约拒绝
        Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override {
            if (!InRange(offset, bytes)) return Storage::Error::InvalidArgument;
            if (!Aligned(offset, bytes)) return Storage::Error::InvalidArgument;
            m_events.push_back('R');
            ++m_reads;
            if (m_failRead) return Storage::Error::Io;
            std::memcpy(buffer, m_bytes.data() + offset, bytes);
            return Storage::Error::Ok;
        }
        // 写一段介质内容：未按块对齐的请求按契约拒绝；m_dropWrite 模拟“写返回成功但没有落盘”
        Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override {
            if (!InRange(offset, bytes)) return Storage::Error::InvalidArgument;
            if (!Aligned(offset, bytes)) return Storage::Error::InvalidArgument;
            m_events.push_back('W');
            ++m_writes;
            m_lastWriteBytes = bytes;
            m_lastWriteOffset = offset;
            if (!m_writable) return Storage::Error::ReadOnly;
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
        // 设置是否有刷新原语：默认 true（内存介质提交即持久），false 模拟 BIOS 侧 USB / NVMe
        void SetFlushPrimitive(bool value) { m_flushPrimitive = value; }
        bool HasFlushPrimitive() const override { return m_flushPrimitive; }
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
        // 最近一次写的长度与落点：用于核对"一次提交只写一个扇区、且按扇区对齐"
        uint32_t LastWriteBytes() const { return m_lastWriteBytes; }
        uint64_t LastWriteOffset() const { return m_lastWriteOffset; }
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
        // 偏移与长度是否都按逻辑块对齐：块设备契约的硬要求
        bool Aligned(uint64_t offset, uint32_t bytes) const { return (offset % m_blockBytes) == 0 && (bytes % m_blockBytes) == 0; }
        Bytes m_bytes;              // 介质内容
        uint32_t m_blockBytes;      // 逻辑块大小
        bool m_writable;            // 是否可写
        bool m_failRead = false;    // 读失败注入
        bool m_failWrite = false;   // 写失败注入
        bool m_failFlush = false;   // 刷盘失败注入
        bool m_dropWrite = false;   // “写成功但不落盘”注入
        bool m_flushPrimitive = true;   // 是否有刷新原语
        unsigned m_reads = 0;       // 读调用次数
        unsigned m_writes = 0;      // 写尝试次数
        uint32_t m_lastWriteBytes = 0;    // 最近一次写的长度
        uint64_t m_lastWriteOffset = 0;   // 最近一次写的落点
        unsigned m_flushes = 0;     // 刷新调用次数
        std::vector<char> m_events; // 操作序列
    };

    // 空文件系统：选版只把 FileSystem 转交给清单验证与吊销记录加载，替身不使用它
    // 默认所有路径都不存在（NotFound）：模拟空卷没有吊销记录；错误注入用于“吊销记录打不开”用例
    class NullFileSystem final : public Storage::FileSystem {
    public:
        // 设置 Open 的返回：NotFound 是空卷的正常形态，其它取值模拟存储层读不了
        void SetOpenError(Storage::Error error) { m_openError = error; }
        // 打开：按注入的结论返回，默认 NotFound
        Storage::Error Open(const char* path, Storage::FileId& id) override {
            (void)path;
            (void)id;
            return m_openError;
        }
    private:
        Storage::Error m_openError = Storage::Error::NotFound;   // Open 的返回结论
    };

    // 夹具登记的一条集：目录、标识与清单事实
    struct SetEntry {
        std::string directory;      // 集目录路径
        uint8_t buildId[32] = {};   // 集标识
        uint64_t securityVersion = 0;   // 清单的安全版本
        uint32_t artifacts = 1;     // 产物条目数
        uint8_t signerKeyId = 1;    // 清单的签名密钥编号
        bool verified = true;       // 清单验证是否通过
        std::string kernelPath = kKernelName;   // 清单 .os 条目的相对路径（可在子目录）
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
        // 构造：可写性与逻辑块大小，并把自己登记为替身的当前夹具
        explicit Fixture(bool writable = true, uint32_t blockBytes = 512) : m_medium(kMediumBytes, writable, blockBytes) { s_current = this; }
        // 析构：撤掉当前夹具
        ~Fixture() { s_current = nullptr; }
        // 不允许拷贝：介质与记录是独占的
        Fixture(const Fixture&) = delete;
        Fixture& operator=(const Fixture&) = delete;
        // 当前夹具：替身经它取结果
        static Fixture* Current() { return s_current; }
        // 夹具介质
        MemoryMedium& Medium() { return m_medium; }
        // 注入吊销记录打开失败：模拟系统卷读不了签名吊销记录
        void SetRevocationOpenError(Storage::Error error) { m_fileSystem.SetOpenError(error); }
        // 登记一条集：验证默认通过；artifactPath 是清单 .os 条目的相对路径
        void AddSet(const uint8_t* buildId, uint64_t securityVersion, const char* artifactPath = kKernelName) {
            SetEntry entry;
            entry.directory = SetDirectory(buildId);
            std::memcpy(entry.buildId, buildId, sizeof(entry.buildId));
            entry.securityVersion = securityVersion;
            entry.kernelPath = artifactPath;
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
                // 生产读取方把清单 .os 条目的相对路径带回；这里按登记值填，选版必须原样使用
                Require(entry.kernelPath.size() <= KernelSet::ManifestLayout::kEntryPathMaxBytes, "夹具产物路径超过上限");
                std::memcpy(facts.kernelPath, entry.kernelPath.data(), entry.kernelPath.size());
                facts.kernelPath[entry.kernelPath.size()] = 0;
                facts.kernelPathBytes = static_cast<uint16_t>(entry.kernelPath.size());
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
        // 跑一次选版：layoutFlags 默认 0（缺省不降级），要测介质策略位时显式传位
        Baleen::Diagnostic Select(const char* layoutKernelPath, Baleen::Core::SelectedSet& selected, uint32_t layoutFlags = 0, uint64_t stateOffset = kStateOffset, uint32_t stateBytes = kStateBytes) {
            return Baleen::Core::SelectKernelSet(m_fileSystem, m_medium, stateOffset, stateBytes, layoutKernelPath, layoutFlags, selected);
        }
        // 按生产写入方的同一套接口写一份记录：开发期对 Updater / Init 的替身
        bool WriteRecord(const Record& record, uint64_t& generation) {
            return BootState::Write(m_medium, kStateOffset, kStateBytes, record, generation);
        }
        // 读一份记录
        BootState::ReadResult ReadRecord(Record& record) {
            return BootState::Read(m_medium, kStateOffset, kStateBytes, record);
        }
        // 槽位级检查：每个槽位各自的结论与选中记录
        BootState::Inspection InspectRecords() { return BootState::Inspect(m_medium, kStateOffset, kStateBytes); }
        // 受控恢复：清零全部槽位
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

    // 夹具侧的槽位构造：按规格第二节的字节表拼接一个 512 字节槽位，校验和用夹具自己的 CRC-32
    // 生产代码不会产生字段违规的记录，构造这类副本是为了核对读取方的逐项校验
    Bytes RawCopy(const Record& record, uint8_t stateValue, uint8_t flags, uint32_t reserved, bool checksumCorrect) {
        Bytes copy(BootState::kCopyBytes, 0);
        Put32(copy, 0x000, 0x54534C42u);                    // Magic "BLST"
        Put16(copy, 0x004, BootState::kVersion);            // Version
        Put16(copy, 0x006, BootState::kRecordBytes);        // Bytes
        Put64(copy, 0x008, record.generation);              // Generation
        copy[0x010] = stateValue;                           // State
        copy[0x011] = record.triesRemaining;                // TriesRemaining
        copy[0x012] = record.triesDone;                     // TriesDone（版本 3 起）
        copy[0x013] = flags;                                // Flags（版本 3 起是 8 位）
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

    // 把一份原始槽位写进指定的槽位偏移
    void PutCopy(Fixture& fixture, uint64_t copyOffset, const Bytes& copy) {
        Require(copy.size() == BootState::kCopyBytes, "原始副本长度不符");
        fixture.Medium().Patch(kStateOffset + copyOffset, copy.data(), static_cast<uint32_t>(copy.size()));
    }

    // 核对选中集：目录、指定内核路径、标识、已验证与试启动身份
    void RequireSelectedPath(const Baleen::Core::SelectedSet& selected, const uint8_t* buildId, const std::string& kernelPath, uint64_t attemptId) {
        Require(std::string(selected.setDirectory) == SetDirectory(buildId), "选中的集目录不符：" + std::string(selected.setDirectory));
        Require(std::string(selected.kernelPath) == kernelPath, "选中的内核路径不符：" + std::string(selected.kernelPath));
        Require(std::memcmp(selected.buildId, buildId, 32) == 0, "选中的集标识不符");
        Require(selected.verified, "选中的集未经清单验证");
        Require(selected.attemptId == attemptId, "试启动身份不符：" + std::to_string(selected.attemptId));
    }

    // 核对选中集：清单 .os 在集目录根上的默认形态
    void RequireSelected(const Baleen::Core::SelectedSet& selected, const uint8_t* buildId, uint64_t attemptId) {
        RequireSelectedPath(selected, buildId, KernelPath(buildId), attemptId);
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

    // 空区域按“无记录”处理：缺省集直启；清单版本为 0、没有可记事实时不写盘、无诊断
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

    // 空区域首次启动持久化缺省集事实：缺省集一旦验证通过，把已见版本与吊销并集钉进一条新记录
    void CaseEmptyAreaPersistsFacts() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 5);
        Baleen::Core::SelectedSet first;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), first);
        Require(reason.code == nullptr, "空区域启动失败：" + Text(reason));
        RequireSelected(first, defaultId.data(), 0);
        Require(fixture.Medium().Writes() == 1, "有非零事实时空区域必须以一次提交持久化");
        Record record;
        Require(fixture.ReadRecord(record) == BootState::ReadResult::Ok, "持久化后必须能读出记录");
        Require(record.state == State::Confirmed, "持久化记录必须是 Confirmed");
        Require(record.generation == 1, "空区域首次持久化的代次必须是 1");
        Require(record.seenSecurityVersion == 5, "持久化记录必须保存已见最高版本");
        Require(std::memcmp(record.confirmedBuildId, defaultId.data(), 32) == 0, "持久化记录必须把验证过的缺省集钉为已知良好集");
        Require(std::memcmp(record.confirmedManifestDigest, Id(0x00).data(), 32) != 0, "持久化记录必须带上缺省集的清单摘要");
        Require(std::memcmp(record.candidateBuildId, Id(0x00).data(), 32) == 0 && record.attemptId == 0, "持久化记录的候选与尝试身份必须为零");
        // 第二次选版走 Confirmed 路径：不再多写，也不再报“没有已知良好集”
        Baleen::Core::SelectedSet second;
        Require(fixture.Select(KernelPath(defaultId.data()).c_str(), second).code == nullptr, "第二次选版失败");
        RequireSelected(second, defaultId.data(), 0);
        Require(fixture.Medium().Writes() == 1, "第二次选版不该再写记录");
        for (const auto& line : fixture.Diagnostics()) Require(line.code != "E-CORE-59", "第二次选版不该再报没有已知良好集");
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

    // 读状态区域失败按致命原因停机：设备事实未知，不能当成“无记录”按零事实启动
    void CaseStateReadFailure() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 0);
        fixture.Medium().FailRead(true);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-89", "读失败必须以 E-CORE-89 停机：" + Text(reason));
        Require(selected.setDirectory[0] == '\0', "读失败时不得定下目标集");
        Require(fixture.Medium().Writes() == 0, "读失败时不得写盘");
        Require(fixture.Calls().empty(), "读失败时不得进入清单验证");
    }

    // 状态区域参数非法按致命原因停机：长度不足或起点未按块对齐都在读写介质之前拒绝
    void CaseInvalidStateAreaStops() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 0);
        Baleen::Core::SelectedSet selected;
        const auto shortReason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected, 0, kStateOffset, BootState::kAreaBytes - 1);
        Require(shortReason.code != nullptr && std::string(shortReason.code) == "E-CORE-90", "长度不足的状态区域必须以 E-CORE-90 停机：" + Text(shortReason));
        Require(selected.setDirectory[0] == '\0', "参数非法时不得定下目标集");
        const auto unalignedReason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected, 0, kStateOffset + BootState::kCopyBytes / 2);
        Require(unalignedReason.code != nullptr && std::string(unalignedReason.code) == "E-CORE-90", "起点未按块对齐的状态区域必须以 E-CORE-90 停机：" + Text(unalignedReason));
        Require(selected.setDirectory[0] == '\0', "参数非法时不得定下目标集");
        Require(fixture.Medium().Writes() == 0 && fixture.Medium().Reads() == 0, "参数检查必须发生在读写介质之前");
    }

    // 吊销记录打开失败报持久化边界：不能当成“没有记录”静默略过，WARN 后继续选版
    void CaseRevocationOpenFailureWarns() {
        Fixture fixture;
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 0);
        fixture.SetRevocationOpenError(Storage::Error::Io);
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "吊销记录读不了不应阻止启动：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-75");
        bool detailGiven = false;
        for (const auto& line : fixture.Diagnostics()) {
            if (line.code == "E-CORE-75") detailGiven = !line.detail.empty();
        }
        Require(detailGiven, "E-CORE-75 必须带上存储层给出的原因");
        Require(fixture.Medium().Writes() == 0, "没有可记事实时不该写状态记录");
    }

    // 首次写入落槽位 A（第一个未写过的槽位）、代次为 1，写入前区域必须是空的
    void CaseFirstWriteLandsInCopyA() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        Record record = ConfirmedRecord(confirmed.data());
        uint64_t generation = 0;
        Require(fixture.WriteRecord(record, generation), "空区域首次写入必须成立");
        Require(generation == 1, "空区域首次写入的代次必须是 1，实际 " + std::to_string(generation));
        Require(fixture.Medium().Writes() == 1, "首次写入只允许一次写调用");
        const Bytes slotA = fixture.Medium().Slice(kStateOffset, 4);
        const Bytes slotB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes, 4);
        Require(std::memcmp(slotA.data(), "BLST", 4) == 0, "首次写入必须落在槽位 A");
        Require(std::memcmp(slotB.data(), "\0\0\0\0", 4) == 0, "首次写入不该碰槽位 B");
    }

    // 第二次写入落槽位 B（下一个未写过的槽位）、代次为 2；读取取代次最高的槽位
    void CaseSecondWriteRotatesToCopyB() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
        Record pending = CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries);
        Require(fixture.WriteRecord(pending, generation), "第二份记录写入失败");
        Require(generation == 2, "第二份记录的代次必须是 2，实际 " + std::to_string(generation));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "槽位里必须能读出记录");
        Require(read.generation == 2 && read.state == State::Pending, "读取必须取代次最高的槽位");
        const Bytes slotB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes, 4);
        Require(std::memcmp(slotB.data(), "BLST", 4) == 0, "第二次写入必须落在槽位 B");
    }

    // 槽位环形推进：未写过的槽位优先，全部写满之后回到代次最低的那个（最旧的）
    // 这条是掉电安全的依据：任何一次提交都不覆盖当前最新的那一份
    void CaseWriteRotatesThroughSlots() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        for (uint32_t i = 0; i < BootState::kSlots; ++i) {
            Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "填槽写入失败");
            Require(generation == i + 1, "第 " + std::to_string(i + 1) + " 次写入的代次必须是 " + std::to_string(i + 1));
        }
        // 第 9 次：所有槽位都有效，必须覆盖代次最低的那个（槽位 A）
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第九次写入失败");
        Require(generation == BootState::kSlots + 1, "第九次写入的代次必须是 " + std::to_string(BootState::kSlots + 1));
        const auto readGeneration = [&fixture](uint32_t slot) {
            const Bytes head = fixture.Medium().Slice(kStateOffset + slot * BootState::kCopyBytes + 0x008, 8);
            uint64_t value = 0;
            for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(head[i]) << (8 * i);
            return value;
        };
        Require(readGeneration(0) == BootState::kSlots + 1, "第九次必须覆盖最旧的槽位 A");
        Require(readGeneration(BootState::kSlots - 1) == BootState::kSlots, "写入永不覆盖代次最高的槽位");
    }

    // 一个槽位写坏：回落到其它完整有效的槽位（掉电语义：正在写的那一个可能不完整）
    void CaseOneCopyCorrupt() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        const auto candidate = Id(0x22);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "第一份记录写入失败");
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), confirmed.data(), BootState::kDefaultTries), generation), "第二份记录写入失败");
        // 第二份落在槽位 B：把它改坏，读取必须回落到槽位 A 的 Confirmed 记录
        Bytes slotB = fixture.Medium().Slice(kStateOffset + BootState::kCopyBytes, BootState::kCopyBytes);
        slotB[0x060] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, slotB.data(), static_cast<uint32_t>(slotB.size()));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "一个槽位写坏时必须取另一个");
        Require(read.generation == 1 && read.state == State::Confirmed, "回落到的记录必须是被写坏那个槽位之外的另一份");
    }

    // 只剩一个槽位有效仍能读出记录：这是把副本从 2 扩到 8 的收益——连续多次断电各毁一个也要毁到第 8 个
    void CaseSevenSlotsCorrupt() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        for (uint32_t i = 0; i < BootState::kSlots; ++i) Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "填槽写入失败");
        // 留下最后一个槽位，其余每一个都写成"有痕迹但没有可用记录"
        for (uint32_t slot = 0; slot + 1 < BootState::kSlots; ++slot) {
            Bytes damaged(BootState::kCopyBytes, 0);
            damaged[0] = 0x42;
            fixture.Medium().Patch(kStateOffset + slot * BootState::kCopyBytes, damaged.data(), static_cast<uint32_t>(damaged.size()));
        }
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "只剩一个槽位有效时仍必须读出记录");
        Require(read.generation == BootState::kSlots, "读出的必须是那个唯一有效槽位的记录");
    }

    // 全部槽位都写坏：按记录损坏处理，不猜测目标集
    void CaseBothCopiesCorrupt() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Bytes slotA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        slotA[0x030] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, slotA.data(), static_cast<uint32_t>(slotA.size()));
        for (uint32_t slot = 1; slot < BootState::kSlots; ++slot) {
            Bytes damaged(BootState::kCopyBytes, 0);
            damaged[0] = 0x42;           // 只有写入痕迹，没有可用记录
            fixture.Medium().Patch(kStateOffset + slot * BootState::kCopyBytes, damaged.data(), static_cast<uint32_t>(damaged.size()));
        }
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Corrupt, "全部槽位都不可用必须判为损坏");
    }

    // 最高代次出现两次：视为写方违规，哪一份都不采用
    void CaseEqualGenerations() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        // 把槽位 A 的记录原样复制到槽位 B：两份代次相同且都有效
        const Bytes slotA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, slotA.data(), static_cast<uint32_t>(slotA.size()));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Corrupt, "最高代次出现两次必须判为损坏");
    }

    // 写中途断电：只落了头部的槽位一律不得被选中——哪怕它声称的代次最高
    // 构造方式是"新一代记录的前 N 字节覆盖上去、尾部与校验和仍是旧内容"，
    // 校验和因此对不上；这正是新记录的头落盘、其余没落的现场
    void CaseTornCommitNeverChosen() {
        // 边界取到 0xEB：记录有效长度 0xF0、校验和在 0xEC，越过它就不是撕裂而是完整写入
        const uint32_t boundaries[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 192, 235};
        for (const uint32_t torn : boundaries) {
            Fixture fixture;
            const auto confirmed = Id(0x11);
            uint64_t generation = 0;
            Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "干净记录写入失败");
            // 新一代记录：代次 2、状态 Pending，校验和按完整记录算好（头部含代次字段）
            Record newer = CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries);
            newer.generation = 2;
            const Bytes full = RawCopy(newer, static_cast<uint8_t>(State::Pending), 0, 0, true);
            Bytes tornBytes(BootState::kCopyBytes, 0);
            for (uint32_t i = 0; i < torn; ++i) tornBytes[i] = full[i];
            fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, tornBytes.data(), static_cast<uint32_t>(tornBytes.size()));

            Record read;
            const auto result = fixture.ReadRecord(read);
            Require(result == BootState::ReadResult::Ok, "撕裂长度 " + std::to_string(torn) + "：必须仍能读出干净的那一份");
            Require(read.generation == 1 && read.state == State::Confirmed, "撕裂长度 " + std::to_string(torn) + "：绝不能按撕裂槽位声称的代次选中它");
            const auto inspection = fixture.InspectRecords();
            Require(inspection.slots[1].status != BootState::CopyStatus::Valid, "撕裂长度 " + std::to_string(torn) + "：撕裂槽位不得报有效");
        }
        // 反面边界：写满整条记录（含校验和）就不是撕裂，必须按完整记录接受
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "干净记录写入失败");
        Record newer = CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries);
        newer.generation = 2;
        const Bytes full = RawCopy(newer, static_cast<uint8_t>(State::Pending), 0, 0, true);
        fixture.Medium().Patch(kStateOffset + BootState::kCopyBytes, full.data(), static_cast<uint32_t>(full.size()));
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "写满整条记录时必须可读");
        Require(read.generation == 2 && read.state == State::Pending, "写满整条记录时必须按新一代记录接受");
    }

    // 只有最后一个槽位完好、其余七个都只落了半条记录：仍要从那一个读出记录
    void CaseTornHeadsElsewhereStillReads() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        for (uint32_t i = 0; i < BootState::kSlots; ++i) Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "填槽写入失败");
        const Bytes last = fixture.Medium().Slice(kStateOffset + (BootState::kSlots - 1) * BootState::kCopyBytes, BootState::kCopyBytes);
        for (uint32_t slot = 0; slot + 1 < BootState::kSlots; ++slot) {
            Bytes torn(BootState::kCopyBytes, 0);
            for (uint32_t i = 0; i < 32; ++i) torn[i] = last[i];          // 头 32 字节落盘，尾部与校验和是旧的
            fixture.Medium().Patch(kStateOffset + slot * BootState::kCopyBytes, torn.data(), static_cast<uint32_t>(torn.size()));
        }
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "七个槽位半条记录时仍必须读出记录");
        Require(read.generation == BootState::kSlots, "读出的必须是唯一完好那个槽位的记录");
    }

    // 介质没有刷新原语时，持久化边界必须报到现场：提交可能只落在设备缓存里，掉电会回退那次转换
    // 有原语的介质上不得报这条——否则每台机器每次启动都在喊狼来了
    void CaseNoFlushPrimitiveReported() {
        {
            Fixture fixture;
            fixture.Medium().SetFlushPrimitive(false);
            const auto confirmed = Id(0x11);
            const auto candidate = Id(0x22);
            fixture.AddSet(confirmed.data(), 0);
            fixture.AddSet(candidate.data(), 4);
            uint64_t generation = 0;
            Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), nullptr, BootState::kDefaultTries), generation), "Pending 写入失败");
            Baleen::Core::SelectedSet selected;
            Require(fixture.Select(KernelPath(candidate.data()).c_str(), selected).code == nullptr, "无原语介质上选版仍必须成立");
            RequireDiagnostic(fixture, "E-CORE-71");
            // 写入照常落盘：这条 WARN 说的是持久化边界，不是写失败
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Trying, "无原语介质上状态转换照常写盘");
        }
        {
            Fixture fixture;   // 默认有刷新原语
            const auto confirmed = Id(0x11);
            const auto candidate = Id(0x22);
            fixture.AddSet(confirmed.data(), 0);
            fixture.AddSet(candidate.data(), 4);
            uint64_t generation = 0;
            Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), nullptr, BootState::kDefaultTries), generation), "Pending 写入失败");
            Baleen::Core::SelectedSet selected;
            Require(fixture.Select(KernelPath(candidate.data()).c_str(), selected).code == nullptr, "选版必须成立");
            for (const auto& line : fixture.Diagnostics()) Require(std::string(line.code) != "E-CORE-71", "有原语的介质不该报持久化边界");
        }
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
        const auto makeFlags = +[](Bytes& copy) { copy[0x013] = 1; FixChecksum(copy); };
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

    // 写失败：不落盘、已有有效槽位字节不变
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
        Require(std::memcmp(copyB.data(), "\0\0\0\0", 4) == 0, "写失败不该在别的槽位留下内容");
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

    // 槽位级检查：报告每个槽位各自的结论与选中记录，能指出坏的是哪一个
    void CaseInspectCopies() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data(), 5), generation), "记录写入失败");
        auto inspection = fixture.InspectRecords();
        Require(inspection.result == BootState::ReadResult::Ok, "一份有效时区域结论必须有效");
        Require(inspection.slots[0].status == BootState::CopyStatus::Valid && inspection.slots[0].generation == 1, "槽位 A 必须报告有效与代次 1");
        Require(inspection.slots[1].status == BootState::CopyStatus::NotWritten && inspection.slots[1].generation == 0, "槽位 B 必须报告未写过");
        // 再写一份到槽位 B，然后把槽位 A 改坏：报告要指出坏的是哪一个槽位
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, Id(0x22).data(), confirmed.data(), BootState::kDefaultTries), generation), "第二份记录写入失败");
        Bytes slotA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        slotA[0x030] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, slotA.data(), static_cast<uint32_t>(slotA.size()));
        inspection = fixture.InspectRecords();
        Require(inspection.slots[0].status == BootState::CopyStatus::Invalid, "写坏的那个槽位必须报告不可用");
        Require(inspection.slots[1].status == BootState::CopyStatus::Valid && inspection.slots[1].generation == 2, "另一个槽位必须报告有效与自身代次");
        Require(inspection.result == BootState::ReadResult::Ok && inspection.record.generation == 2, "区域结论必须取有效的那一份");
    }

    // 受控清零：全部槽位清零、区域回到出厂形态；只读介质上拒绝
    void CaseClearArea() {
        Fixture fixture;
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "记录写入失败");
        Require(fixture.ClearArea(), "可写介质上的清零必须成立");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Empty, "清零后区域必须按空处理");
        const auto inspection = fixture.InspectRecords();
        bool allClear = true;
        for (uint32_t slot = 0; slot < BootState::kSlots; ++slot) allClear = allClear && inspection.slots[slot].status == BootState::CopyStatus::NotWritten;
        Require(allClear, "清零后全部槽位都必须未写过");
        Fixture readOnly(false);
        Require(!readOnly.ClearArea(), "只读介质上的清零必须失败");
    }

    // 按有效记录修复：把每个不可用槽位都重建一遍，内容不变；没有可用记录时拒绝
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
        // 写坏代次较小的那份（槽位 A，代次 1）：槽位 B 完好，其余槽位未写过
        Bytes slotA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        slotA[0x008] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, slotA.data(), static_cast<uint32_t>(slotA.size()));
        Require(fixture.InspectRecords().slots[0].status == BootState::CopyStatus::Invalid, "夹具必须先把槽位 A 写坏");
        Require(fixture.RepairArea(generation), "有有效记录时必须能修复");
        const auto inspection = fixture.InspectRecords();
        bool allValid = true;
        for (uint32_t slot = 0; slot < BootState::kSlots; ++slot) allValid = allValid && inspection.slots[slot].status == BootState::CopyStatus::Valid;
        Require(allValid, "修复后全部槽位都必须有效（冗余恢复到满）");
        // 修完的槽位代次互不相同：每次写抬一代，不会与源槽位撞代次（撞了就会判为损坏）
        bool distinct = true;
        for (uint32_t a = 0; a < BootState::kSlots; ++a) {
            for (uint32_t b = a + 1; b < BootState::kSlots; ++b) distinct = distinct && inspection.slots[a].generation != inspection.slots[b].generation;
        }
        Require(distinct, "修复后各槽位代次必须互不相同");
        Require(inspection.record.state == before.state && inspection.record.seenSecurityVersion == before.seenSecurityVersion, "修复不得改变状态与已见版本");
        Require(inspection.record.triesRemaining == before.triesRemaining, "修复不得改变剩余尝试次数");
        Require(std::memcmp(inspection.record.candidateBuildId, candidate.data(), 32) == 0, "修复必须保留候选集");
        uint64_t ignored = 0;
        Fixture empty;
        Require(!empty.RepairArea(ignored), "空区域不得修复");
        Fixture broken;
        for (uint32_t slot = 0; slot < BootState::kSlots; ++slot) {
            Bytes garbage(BootState::kCopyBytes, 0);
            garbage[0] = 0x42;
            broken.Medium().Patch(kStateOffset + slot * BootState::kCopyBytes, garbage.data(), static_cast<uint32_t>(garbage.size()));
        }
        Require(!broken.RepairArea(ignored), "全部槽位都不可用时不得修复");
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

    // 确认提交保留已试次数：triesDone 是“这一版试过几次”的审计账，转正时不清零
    void CaseConfirmKeepsTriesDone() {
        Fixture fixture;
        const auto candidate = Id(0x22);
        Record trying = CandidateRecord(State::Trying, candidate.data(), nullptr, BootState::kDefaultTries);
        trying.triesDone = 2;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(trying, generation), "Trying 记录写入失败");
        Require(generation == 1, "Trying 首次写入的代次必须是 1");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.state == State::Trying, "Trying 记录必须读得回来");
        Require(read.attemptId == generation && read.triesDone == 2, "Trying 的尝试身份应取写入代次、已试次数保持 2");
        uint64_t confirmedGeneration = 0;
        Require(BootState::ConfirmAttempt(fixture.Medium(), kStateOffset, kStateBytes, read.attemptId, confirmedGeneration), "身份相符的确认应成功");
        Require(confirmedGeneration == generation + 1, "确认写入的代次必须加一");
        Record after;
        Require(fixture.ReadRecord(after) == BootState::ReadResult::Ok && after.state == State::Confirmed, "确认后记录必须是 Confirmed");
        Require(after.triesDone == 2, "确认提交不得清零已试次数");
        Require(after.attemptId == 0 && after.triesRemaining == 0, "确认后尝试身份与剩余次数应清零");
        Require(std::memcmp(after.confirmedBuildId, candidate.data(), 32) == 0, "确认应把候选转成已知良好集");
    }

    // 掉电演练：整份副本只落下一部分时，读取方必须仍然拿到一份完整有效副本，不判损坏
    // 逐字节截断两个方向的写入，核对"任何时刻至少有一份完整有效副本"这条不变量；
    // 截断到记录有效长度（0xF0，含校验和）时新副本自身已完整，读取取代次更大的那一份
    void CasePowerLossDrill() {
        // 方向一：槽位 A 已有效（代次 1），向槽位 B 写代次 2 的槽位，逐字节截断
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

    // 代次耗尽按致命原因停机：写不进新记录时不能只报 WARN 继续，否则状态区域永远无法推进
    void CaseGenerationExhaustedStops() {
        // 已确认集合：清单安全版本更高，选版要抬升已见版本——提交写不成立，代次耗尽必须 FATAL
        {
            Fixture fixture;
            const auto confirmed = Id(0x11);
            fixture.AddSet(confirmed.data(), 5);
            Record record = ConfirmedRecord(confirmed.data());
            record.generation = BootState::kGenerationMax;
            PutCopy(fixture, 0, RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true));
            Baleen::Core::SelectedSet selected;
            const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
            Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-92", "代次耗尽必须以 E-CORE-92 停机：" + Text(reason));
            RequireDiagnostic(fixture, "E-CORE-70", 0);
            Require(fixture.Medium().Writes() == 0, "代次耗尽时不该发生写调用");
        }
        // 只读介质上同样按耗尽停机：耗尽判定先于写能力，不能退化成普通写失败 WARN
        {
            Fixture fixture(false);
            const auto confirmed = Id(0x11);
            fixture.AddSet(confirmed.data(), 5);
            Record record = ConfirmedRecord(confirmed.data());
            record.generation = BootState::kGenerationMax;
            PutCopy(fixture, 0, RawCopy(record, static_cast<uint8_t>(State::Confirmed), 0, 0, true));
            Baleen::Core::SelectedSet selected;
            const auto reason = fixture.Select(KernelPath(confirmed.data()).c_str(), selected);
            Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-92", "只读介质上的代次耗尽也必须以 E-CORE-92 停机：" + Text(reason));
            Require(fixture.Medium().Writes() == 0, "只读介质不该发生写调用");
        }
        // 回退缺省集的写回同样适用：Failed 记录到代次上限时也必须 FATAL
        {
            Fixture fixture;
            const auto candidate = Id(0x22);
            const auto defaultId = Id(0x0D);
            fixture.AddSet(defaultId.data(), 5);
            Record failed = CandidateRecord(State::Failed, candidate.data(), nullptr, 0);
            failed.generation = BootState::kGenerationMax;
            PutCopy(fixture, 0, RawCopy(failed, static_cast<uint8_t>(State::Failed), 0, 0, true));
            Baleen::Core::SelectedSet selected;
            const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
            Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-92", "回退缺省集的写回耗尽必须以 E-CORE-92 停机：" + Text(reason));
            RequireDiagnostic(fixture, "E-CORE-70", 0);
            Require(fixture.Medium().Writes() == 0, "代次耗尽时不该发生写调用");
        }
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

    // Failed 记录且没有已知良好集：不能去开 /KernelSets/000…/，必须回退到布局描述指定的缺省集
    // 出厂介质上第一次更新就失败三次时会落到这个组合：那时从来没有集被确认过，
    // 若按“已知良好集”去选，整台机器会永久起不来（真实介质上复现过，见 CheckAll 的失败注入与状态机多轮）
    void CaseFailedWithoutConfirmed() {
        Fixture fixture;
        const auto candidate = Id(0x22);
        const auto defaultId = Id(0x0D);
        fixture.AddSet(candidate.data(), 4);
        fixture.AddSet(defaultId.data(), 0);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Failed, candidate.data(), nullptr, 0), generation), "Failed 记录写入失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "Failed 且无良好集时应回退缺省集：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-59");
    }

    // 回退缺省集时保留原状态，把缺省集带来的已见版本抬升进记录
    void CaseFallbackLiftsSeenVersion() {
        Fixture fixture;
        const auto candidate = Id(0x22);
        const auto defaultId = Id(0x0D);
        fixture.AddSet(defaultId.data(), 5);
        Record failed = CandidateRecord(State::Failed, candidate.data(), nullptr, 0);
        failed.seenSecurityVersion = 2;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(failed, generation), "Failed 记录写入失败");
        Require(generation == 1 && fixture.Medium().Writes() == 1, "铺底记录必须只写一次、代次为 1");
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "回退缺省集失败：" + Text(reason));
        RequireSelected(selected, defaultId.data(), 0);
        RequireDiagnostic(fixture, "E-CORE-59");
        Require(fixture.Medium().Writes() == 2, "抬升已见版本必须再写一份记录");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "抬升后的记录必须可读");
        Require(read.state == State::Failed, "回退写回必须保持原状态");
        Require(read.generation == 2, "写回记录的代次必须是 2");
        Require(read.seenSecurityVersion == 5, "写回记录必须抬升已见最高版本");
        Require(std::memcmp(read.confirmedBuildId, Id(0x00).data(), 32) == 0, "没有已知良好集时必须保持全零");
    }

    // 512 字节逻辑块：一次提交只写一份槽位（512 字节），多扇区写被打断时不会波及另一份
    void CaseCommitWritesOneSlot() {
        static_assert(BootState::kCopyBytes == 512, "一份槽位固定 512 字节");
        static_assert(BootState::kRecordBytes <= BootState::kCopyBytes, "记录必须落在一份槽位内");
        Fixture fixture;
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(Id(0x11).data()), generation), "写入失败");
        Require(fixture.Medium().Writes() == 1, "一次提交只允许一次写调用");
        Require(fixture.Medium().LastWriteBytes() == BootState::kCopyBytes, "512 字节块上的提交长度必须正好是一份槽位");
        Require(fixture.Medium().LastWriteOffset() % BootState::kCopyBytes == 0, "槽位落点必须按槽位长度对齐");
    }

    // 4Kn 介质（4096 字节逻辑块）：提交必须整块落盘，槽位布局、环形与掉电语义不变
    // 旧实现固定只写 512 字节：块设备契约判 InvalidArgument，4Kn 上所有状态转换都失败（E-CORE-70）
    void CaseFourKnCommit() {
        static_assert(BootState::kAreaBytes == 4096, "4Kn 用例要求状态区域恰好等于一个逻辑块");
        Fixture fixture(true, 4096);
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "4Kn 上首次提交失败");
        Require(fixture.Medium().Writes() == 1, "4Kn 上一次提交只允许一次写调用");
        Require(fixture.Medium().LastWriteBytes() == 4096, "4Kn 上的提交长度必须正好是一个 4096 字节逻辑块");
        Require(fixture.Medium().LastWriteOffset() == kStateOffset, "4Kn 上首次提交的落点必须是状态区域起点");
        // 填满八槽环形：每次整块读—改—写都只改目标槽位，其余槽位必须保持有效
        for (uint32_t slot = 1; slot < BootState::kSlots; ++slot) {
            Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "4Kn 上第 " + std::to_string(slot + 1) + " 次提交失败");
            Require(generation == slot + 1, "4Kn 上代次必须逐次加一");
            Require(fixture.Medium().LastWriteBytes() == 4096, "4Kn 上每次提交都必须写一个逻辑块");
            Require(fixture.Medium().LastWriteOffset() == kStateOffset, "4Kn 上读—改—写的落点必须是包含目标槽位的整块起点");
            const auto inspection = fixture.InspectRecords();
            for (uint32_t other = 0; other <= slot; ++other) {
                Require(inspection.slots[other].status == BootState::CopyStatus::Valid, "4Kn 整块提交不得破坏已有的其它槽位");
            }
            Require(inspection.result == BootState::ReadResult::Ok && inspection.record.generation == generation, "4Kn 上读取必须取最高代次");
        }
        // 真正的状态机写入：Pending 的候选验证失败 → 写 Failed 并回退缺省集
        // 旧实现在这一步因 512 字节写被拒而只报 E-CORE-70，状态停在 Pending
        const auto candidate = Id(0x22);
        const auto defaultId = Id(0x0D);
        Fixture selectFixture(true, 4096);
        selectFixture.AddSet(defaultId.data(), 0);
        uint64_t selectGeneration = 0;
        Record pending = CandidateRecord(State::Pending, candidate.data(), nullptr, BootState::kDefaultTries);
        Require(selectFixture.WriteRecord(pending, selectGeneration), "4Kn 上写入 Pending 失败");
        Baleen::Core::SelectedSet selected;
        const auto reason = selectFixture.Select(KernelPath(defaultId.data()).c_str(), selected);
        Require(reason.code == nullptr, "4Kn 上候选失败后应回退缺省集：" + Text(reason));
        RequireDiagnostic(selectFixture, "E-CORE-72");
        Require(selectFixture.Medium().LastWriteBytes() == 4096, "4Kn 上的状态转换也必须整块提交");
        Record after;
        Require(selectFixture.ReadRecord(after) == BootState::ReadResult::Ok, "4Kn 上 Failed 记录应可读");
        Require(after.state == State::Failed && after.generation == selectGeneration + 1, "4Kn 上候选失败必须真的写出 Failed");
        RequireSelected(selected, defaultId.data(), 0);
    }

    // 4Kn 介质的维护动作：按有效记录修复、整片清零同样按整块落盘
    void CaseFourKnMaintenance() {
        Fixture fixture(true, 4096);
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "4Kn 上首次写入失败");
        Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "4Kn 上第二次写入失败");
        // 写坏代次较小的槽位 A，槽位 B 完好：修复必须把 A 重建出来
        Bytes slotA = fixture.Medium().Slice(kStateOffset, BootState::kCopyBytes);
        slotA[0x008] ^= 0xFF;
        fixture.Medium().Patch(kStateOffset, slotA.data(), static_cast<uint32_t>(slotA.size()));
        uint64_t repaired = 0;
        Require(fixture.RepairArea(repaired), "4Kn 上按有效记录修复失败");
        const auto inspection = fixture.InspectRecords();
        for (uint32_t slot = 0; slot < BootState::kSlots; ++slot) {
            Require(inspection.slots[slot].status == BootState::CopyStatus::Valid, "4Kn 修复后全部槽位必须有效");
        }
        Require(fixture.Medium().LastWriteBytes() == 4096, "4Kn 上的修复必须整块落盘");
        // 清零：整片区域一次写零，回到出厂形态
        Require(fixture.ClearArea(), "4Kn 上清零失败");
        Record empty;
        Require(fixture.ReadRecord(empty) == BootState::ReadResult::Empty, "4Kn 清零后必须按空区域处理");
        Require(fixture.Medium().LastWriteBytes() == 4096, "4Kn 上的清零必须整块落盘");
        Require(fixture.Medium().LastWriteOffset() == kStateOffset, "4Kn 上清零的落点必须是状态区域起点");
    }

    // 2048 字节逻辑块：一次提交是包含目标槽位的 2048 字节块，块内其它槽位字节原样保留
    void CaseWideBlockRmw2048() {
        Fixture fixture(true, 2048);
        const auto confirmed = Id(0x11);
        uint64_t generation = 0;
        for (uint32_t slot = 0; slot < BootState::kSlots; ++slot) {
            Require(fixture.WriteRecord(ConfirmedRecord(confirmed.data()), generation), "2048 字节块上第 " + std::to_string(slot + 1) + " 次提交失败");
            Require(generation == slot + 1, "2048 字节块上代次必须逐次加一");
            Require(fixture.Medium().LastWriteBytes() == 2048, "2048 字节块上提交长度必须是一个逻辑块");
            Require(fixture.Medium().LastWriteOffset() % 2048 == 0, "2048 字节块上提交落点必须按块对齐");
            const auto inspection = fixture.InspectRecords();
            for (uint32_t other = 0; other <= slot; ++other) {
                Require(inspection.slots[other].status == BootState::CopyStatus::Valid, "整块读—改—写不得破坏块内其它槽位");
            }
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok && read.generation == generation, "2048 字节块上读取必须取最高代次");
        }
    }

    // 状态区域起点未按逻辑块对齐、长度不足一个区域：参数非法，必须在碰介质之前拒绝
    void CaseUnalignedStateArea() {
        Fixture fixture(true, 4096);
        const uint64_t unaligned = kStateOffset + BootState::kCopyBytes;
        Record read;
        Require(BootState::Read(fixture.Medium(), unaligned, kStateBytes, read) == BootState::ReadResult::InvalidArea, "未按块对齐的状态区域必须判参数非法");
        uint64_t generation = 0;
        Require(!BootState::Write(fixture.Medium(), unaligned, kStateBytes, ConfirmedRecord(Id(0x11).data()), generation), "未按块对齐的状态区域必须拒绝写入");
        Require(!BootState::Clear(fixture.Medium(), unaligned, kStateBytes), "未按块对齐的状态区域必须拒绝清零");
        Require(fixture.Medium().Reads() == 0 && fixture.Medium().Writes() == 0, "对齐检查必须发生在读写介质之前");
        Require(BootState::Read(fixture.Medium(), kStateOffset, BootState::kAreaBytes - 1, read) == BootState::ReadResult::InvalidArea, "长度不足的状态区域必须判参数非法");
    }

    // 已试次数只增：每次试启动前进一次，次数用尽转 Failed 时保留；候选换新时清零
    void CaseTriesDoneAdvances() {
        Fixture fixture;
        const auto candidate = Id(0x22);
        fixture.AddSet(candidate.data(), 4);
        uint64_t generation = 0;
        Require(fixture.WriteRecord(CandidateRecord(State::Pending, candidate.data(), nullptr, BootState::kDefaultTries), generation), "Pending 写入失败");
        for (uint8_t expected = 1; expected <= BootState::kDefaultTries; ++expected) {
            Baleen::Core::SelectedSet selected;
            Require(fixture.Select(KernelPath(candidate.data()).c_str(), selected).code == nullptr, "试启动选版应成功");
            Record read;
            Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "记录应可读");
            Require(read.triesDone == expected, "已试次数应随每次试启动前进");
            Require(read.state == State::Trying, "未确认前应停在 Trying");
        }
        // 次数用尽：转 Failed 并保留已试次数
        Baleen::Core::SelectedSet selected;
        Require(fixture.Select(KernelPath(candidate.data()).c_str(), selected).code == nullptr, "次数用尽后应回退");
        Record read;
        Require(fixture.ReadRecord(read) == BootState::ReadResult::Ok, "记录应可读");
        Require(read.state == State::Failed && read.triesDone == BootState::kDefaultTries, "Failed 应保留已试次数");
    }

    // 记录损坏时的两种收场：默认停机不猜目标集；介质在布局描述里声明了策略位才退到缺省集
    void CaseDegradeWithoutRecordPolicy() {
        const auto defaultId = Id(0x0D);
        // 造一份"没有一个槽位可用"的区域：先写一份有效记录，再把整片区域写成不可验证的字节
        const auto damage = [](Fixture& fixture) {
            uint64_t generation = 0;
            Require(fixture.WriteRecord(ConfirmedRecord(Id(0x11).data()), generation), "铺底记录写入失败");
            Bytes area = fixture.Medium().Slice(kStateOffset, BootState::kAreaBytes);
            for (auto& byte : area) byte = 0xFF;
            fixture.Medium().Patch(kStateOffset, area.data(), static_cast<uint32_t>(area.size()));
        };
        {
            Fixture fixture;
            fixture.AddSet(defaultId.data(), 0);
            damage(fixture);
            Baleen::Core::SelectedSet selected;
            const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected);
            Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-69", "默认必须停机且不猜目标集");
        }
        {
            Fixture fixture;
            fixture.AddSet(defaultId.data(), 0);
            damage(fixture);
            Baleen::Core::SelectedSet selected;
            const auto reason = fixture.Select(KernelPath(defaultId.data()).c_str(), selected, Baleen::LayoutDescriptor::kFlagDegradeWithoutRecord);
            Require(reason.code == nullptr, "声明策略位后应退到布局描述的缺省集：" + Text(reason));
            RequireSelected(selected, defaultId.data(), 0);
            RequireDiagnostic(fixture, "E-CORE-56");
        }
    }

    // 扁平落点（没有 KernelSets/ 标记）是选版启用前的形态：选择器必须拒绝，绝不直启未验签的路径
    void CaseFlatPathRejected() {
        Fixture fixture;
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(kFlatKernelPath, selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-84", "扁平落点必须以 E-CORE-84 拒绝：" + Text(reason));
        Require(!selected.verified, "拒绝时不能声称集已验证");
        Require(selected.kernelPath[0] == '\0', "拒绝时不该定下装载路径");
        Require(fixture.Medium().Reads() == 0 && fixture.Medium().Writes() == 0, "拒绝前不该碰状态区域");
        Require(fixture.Calls().empty(), "扁平落点不得进入清单验证（也不得跳过它直启）");
    }

    // 清单把 .os 放在子目录：装载路径必须取清单登记的路径，不能固定拼 <集>/LikesProgram.os
    void CaseManifestKernelSubdirectory() {
        // 缺省集：布局路径指向子目录里的 .os，选择结果必须用同一条路径
        const auto defaultId = Id(0x0D);
        Fixture fixture;
        fixture.AddSet(defaultId.data(), 0, "Boot/LikesProgram.os");
        Baleen::Core::SelectedSet selected;
        const std::string layoutPath = ArtifactPath(defaultId.data(), "Boot/LikesProgram.os");
        Require(fixture.Select(layoutPath.c_str(), selected).code == nullptr, "子目录缺省集选版失败");
        RequireSelectedPath(selected, defaultId.data(), layoutPath, 0);

        // 已知良好集：Confirmed 记录的集把 .os 放在子目录，选择结果同样取清单路径
        const auto confirmed = Id(0x11);
        Fixture confirmedFixture;
        confirmedFixture.AddSet(defaultId.data(), 0);
        confirmedFixture.AddSet(confirmed.data(), 2, "Boot/Kernel.os");
        uint64_t generation = 0;
        Record record = ConfirmedRecord(confirmed.data());
        Require(confirmedFixture.WriteRecord(record, generation), "Confirmed 记录写入失败");
        Baleen::Core::SelectedSet confirmedSelected;
        const std::string defaultPath = KernelPath(defaultId.data());
        Require(confirmedFixture.Select(defaultPath.c_str(), confirmedSelected).code == nullptr, "子目录良好集选版失败");
        RequireSelectedPath(confirmedSelected, confirmed.data(), ArtifactPath(confirmed.data(), "Boot/Kernel.os"), 0);

        // 候选集：Pending 的集把 .os 放在子目录，写 Trying 后装载路径同样取清单路径
        const auto candidate = Id(0x22);
        Fixture candidateFixture;
        candidateFixture.AddSet(defaultId.data(), 0);
        candidateFixture.AddSet(candidate.data(), 4, "Boot/Candidate.os");
        uint64_t pendingGeneration = 0;
        Record pending = CandidateRecord(State::Pending, candidate.data(), nullptr, BootState::kDefaultTries);
        Require(candidateFixture.WriteRecord(pending, pendingGeneration), "Pending 记录写入失败");
        Baleen::Core::SelectedSet candidateSelected;
        const std::string candidateDefaultPath = KernelPath(defaultId.data());
        Require(candidateFixture.Select(candidateDefaultPath.c_str(), candidateSelected).code == nullptr, "子目录候选集选版失败");
        Require(candidateSelected.attemptId != 0, "候选试启动身份必须非零");
        RequireSelectedPath(candidateSelected, candidate.data(), ArtifactPath(candidate.data(), "Boot/Candidate.os"), candidateSelected.attemptId);
    }

    // 布局路径指向集内的另一个文件：清单登记的 .os 才是唯一装载路径，不一致必须拒绝（不能“验 A 装 B”）
    void CaseLayoutArtifactMismatch() {
        const auto defaultId = Id(0x0D);
        Fixture fixture;
        fixture.AddSet(defaultId.data(), 0);
        Baleen::Core::SelectedSet selected;
        const std::string wrongPath = ArtifactPath(defaultId.data(), "Other.os");
        const auto reason = fixture.Select(wrongPath.c_str(), selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-85", "布局路径与清单产物不符必须以 E-CORE-85 拒绝：" + Text(reason));
        Require(std::string(selected.kernelPath).empty(), "拒绝时不该定下装载路径");
        // 清单本身仍然通过验证：拒绝点在“布局路径与已验证产物不符”，不是验签失败
        Require(fixture.Calls().size() == 1, "布局路径不符时应先完成清单验证再判定");
    }

    // 缺省路径过长：以 E-CORE-84 停机，不碰介质
    void CaseDefaultPathTooLong() {
        Fixture fixture;
        const std::string path(600, 'a');
        Baleen::Core::SelectedSet selected;
        const auto reason = fixture.Select(path.c_str(), selected);
        Require(reason.code != nullptr && std::string(reason.code) == "E-CORE-84", "过长路径必须以 E-CORE-84 停机：" + Text(reason));
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
        Case("空区域首次启动持久化缺省集事实", CaseEmptyAreaPersistsFacts);
        Case("没有状态区域按无记录处理", CaseNoStateArea);
        Case("读状态区域失败按致命原因停机", CaseStateReadFailure);
        Case("状态区域参数非法按致命原因停机", CaseInvalidStateAreaStops);
        Case("吊销记录打开失败报持久化边界", CaseRevocationOpenFailureWarns);
        Case("首次写入落槽位 A", CaseFirstWriteLandsInCopyA);
        Case("第二次写入落槽位 B", CaseSecondWriteRotatesToCopyB);
        Case("槽位环形推进与最旧槽位复用", CaseWriteRotatesThroughSlots);
        Case("一个槽位写坏仍取其它槽位", CaseOneCopyCorrupt);
        Case("只剩一个槽位有效仍可读", CaseSevenSlotsCorrupt);
        Case("全部槽位都写坏判为损坏", CaseBothCopiesCorrupt);
        Case("最高代次出现两次判为损坏", CaseEqualGenerations);
        Case("夹具按字节表构造的记录被接受", CaseRawCopyAccepted);
        Case("字段违规的副本一律拒绝", CaseMalformedCopiesRejected);
        Case("代次全 FF 拒绝写入", CaseGenerationOverflow);
        Case("只读介质不进入候选", CaseReadOnlyMedium);
        Case("写失败不改动当前有效副本", CaseWriteFailureKeepsOtherCopy);
        Case("读回不符按写失败处理", CaseReadBackMismatch);
        Case("槽位级检查逐个报告结论", CaseInspectCopies);
        Case("受控清零回到出厂形态", CaseClearArea);
        Case("按有效记录修复全部坏槽位", CaseRepairCopies);
        Case("人工接受只允许下调已见版本", CaseAcceptSeenVersion);
        Case("发布候选：首次写入与保留既有状态", CasePublishCandidate);
        Case("提交确认：比较再提交", CaseConfirmAttempt);
        Case("确认提交保留已试次数", CaseConfirmKeepsTriesDone);
        Case("掉电演练：逐字节截断不破坏有效记录", CasePowerLossDrill);
        Case("写入顺序为写-刷盘-读回", CaseWriteFlushReadOrder);
        Case("刷盘失败按写失败降级", CaseFlushFailure);
        Case("Confirmed 选已知良好集", CaseConfirmedSelects);
        Case("Confirmed 抬升已见最高版本", CaseConfirmedLiftsSeenVersion);
        Case("Confirmed 验证失败返回致命原因", CaseConfirmedVerifyFailure);
        Case("代次耗尽按致命原因停机", CaseGenerationExhaustedStops);
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
        Case("Failed 且无良好集回退缺省集", CaseFailedWithoutConfirmed);
        Case("回退缺省集抬升已见版本", CaseFallbackLiftsSeenVersion);
        Case("512 字节块上提交是单槽位写", CaseCommitWritesOneSlot);
        Case("4Kn 介质按整块提交状态", CaseFourKnCommit);
        Case("4Kn 介质维护动作按整块落盘", CaseFourKnMaintenance);
        Case("2048 字节块介质读改写槽位", CaseWideBlockRmw2048);
        Case("状态区域未按块对齐即拒绝", CaseUnalignedStateArea);
        Case("无刷新原语时报告持久化边界", CaseNoFlushPrimitiveReported);
        Case("写中途断电：撕裂槽位永不选中", CaseTornCommitNeverChosen);
        Case("七个槽位半条记录仍可读", CaseTornHeadsElsewhereStillReads);
        Case("已试次数只增", CaseTriesDoneAdvances);
        Case("记录损坏的收场与介质策略位", CaseDegradeWithoutRecordPolicy);
        Case("扁平落点被拒绝", CaseFlatPathRejected);
        Case("子目录 .os 按清单路径装载", CaseManifestKernelSubdirectory);
        Case("布局路径与清单产物不符", CaseLayoutArtifactMismatch);
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
