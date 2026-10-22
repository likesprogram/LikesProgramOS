/* main.cpp
    CheckHandoff：内核交接包 9 步核对的宿主侧检查，手工执行，不构成默认构建依赖

    检查对象是 Packages/LikesProgramOS/Core/Src/Handoff.cpp 与 ManifestView.cpp 的目标侧源码：
    用 vpath 按相对路径直接编进本工具，不复制实现；宿主 C 运行库提供 memcmp / memcpy，
    Libc.cpp 不链接
    正例用真实集产物构造：Packages/LikesProgramOS/Out/KernelSetStage 的 LikesProgram.os、
    Manifest.mft、Modules.list 与 Modules/ 下 13 个预装 .osm，按 16 字节记录布局
    拼出交接包；清单副本、列表副本、记录数组与模块字节的"物理地址"由本工具
    的宿主替身映射到真实缓冲，物理地址换算函数注入 HostMap
    负例逐项改坏交接包（缺缓冲、越界与回绕、清单/列表结构、记录与条目不符、主版本门、
    BuildId、期望集缺重未知、容器过短），按 E-KRN-50..57 编号断言，不按文本子串断言

    每个用例打印 [ OK ] 或 [ FAIL ]；用例数固定，失败返回 1；真实产物缺失返回 2
*/

#include <Handoff.hpp>
#include <KernelImage.hpp>
#include <Manifest.hpp>
#include <ManifestView.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace ML = KernelSet::ManifestLayout;
    // 用例总数：与用例表长度核对，防止用例被误删
    constexpr unsigned kCaseCount = 55;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;
    // 默认集产物目录：从仓库根执行时的相对路径，可用 --stage 覆盖
    constexpr const char* kDefaultStage = "Packages/LikesProgramOS/Out/KernelSetStage";
    // 物理地址铺开的起点与模块间距：互不重叠且都在低 4GiB 内
    constexpr std::uint64_t kManifestPhys = 0x00100000;
    constexpr std::uint64_t kListPhys = 0x00200000;
    constexpr std::uint64_t kRecordsPhys = 0x00300000;
    constexpr std::uint64_t kModulePhysBase = 0x01000000;
    constexpr std::uint64_t kModulePhysStride = 0x00020000;
    // "使用实际长度"的哨兵
    constexpr std::uint32_t kUseActualBytes = 0xFFFFFFFF;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) {
        if (!value) Fail(message);
    }

    // 按小端写入 16 位
    void Put16(std::uint8_t* field, std::uint16_t value) {
        field[0] = static_cast<std::uint8_t>(value);
        field[1] = static_cast<std::uint8_t>(value >> 8);
    }

    // 按小端写入 32 位
    void Put32(std::uint8_t* field, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) field[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 按小端写入 64 位
    void Put64(std::uint8_t* field, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) field[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 读整个文件
    std::vector<std::uint8_t> ReadFile(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open " + path);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 一条预装记录：与交接包 16 字节布局一一对应，负例按字段改
    struct Record {
        std::uint64_t phys = 0;           // 模块字节物理地址
        std::uint32_t bytes = 0;          // 模块字节数
        std::uint16_t artifactIndex = 0;  // 清单产物条目下标
        std::uint16_t reserved = 0;       // 保留字段
    };

    // 交接包夹具：真实集产物的可改副本与宿主"物理内存"布局
    struct Fixture {
        std::vector<std::uint8_t> manifest;               // 清单副本字节
        std::vector<std::uint8_t> list;                   // Modules.list 副本字节
        std::vector<Record> records;                      // 记录数组内容
        std::vector<std::vector<std::uint8_t>> modules;   // 各记录指向的模块字节
        std::vector<std::uint8_t> osBuildId;              // .os 的 BuildId
        std::uint16_t osMajor = 0;                        // .os 的模块主版本
        std::vector<bool> moduleReadable;                 // 各模块区间是否登记为可读
        std::uint32_t recordCount = kUseActualBytes;      // 记录数覆盖值
        std::uint32_t recordsReserved = 0;                // 记录数组头保留字段
        bool manifestReadable = true;                     // 清单区间是否登记
        bool listReadable = true;                         // 列表区间是否登记
        bool recordsReadable = true;                      // 记录数组区间是否登记
        bool osBuildIdNull = false;                       // 传空 BuildId 指针
        std::uint64_t manifestPhys = kManifestPhys;       // 清单副本物理地址
        std::uint64_t listPhys = kListPhys;               // 列表副本物理地址
        std::uint64_t recordsPhys = kRecordsPhys;         // 记录数组物理地址
        std::uint32_t manifestBytes = kUseActualBytes;    // 清单长度覆盖值
        std::uint32_t listBytes = kUseActualBytes;        // 列表长度覆盖值
    };

    // 宿主替身的一段可读内存
    struct Region {
        std::uint64_t phys = 0;          // 物理起址
        const std::uint8_t* data = nullptr; // 宿主缓冲
        std::uint64_t bytes = 0;         // 字节数
    };

    // 当前用例登记的区间
    std::vector<Region> g_regions;

    // 物理地址换算：整段落在某个已登记区间内才返回指针，否则返回空
    const std::uint8_t* HostMap(std::uint64_t physical, std::uint64_t bytes) {
        for (const Region& region : g_regions) {
            if (physical < region.phys) continue;
            const std::uint64_t offset = physical - region.phys;
            if (offset > region.bytes) continue;
            if (bytes > region.bytes - offset) continue;
            return region.data + offset;
        }
        return nullptr;
    }

    // 登记一段区间；readable 为 false 时跳过，模拟不可读
    void AddRegion(std::uint64_t phys, const std::uint8_t* data, std::uint64_t bytes, bool readable) {
        if (readable) g_regions.push_back({ phys, data, bytes });
    }

    // 一次核对的结果：编号、原因与成功时的事实
    struct Outcome {
        std::string code;                  // 返回编号，空表示成功
        std::string reason;                // 原因文本
        Kernel::HandoffFacts facts;        // 成功时填充的事实
    };

    // 用夹具跑一次 CheckHandoff：按夹具登记宿主区间并注入 HostMap
    Outcome Run(const Fixture& fixture) {
        g_regions.clear();
        // 记录数组字节：8 字节头 + count 条 16 字节记录
        const std::uint32_t count = fixture.recordCount == kUseActualBytes
            ? static_cast<std::uint32_t>(fixture.records.size()) : fixture.recordCount;
        std::vector<std::uint8_t> recordBytes(static_cast<std::size_t>(Kernel::kHandoffHeaderBytes)
            + static_cast<std::size_t>(count) * Kernel::kHandoffRecordBytes, 0);
        Put32(recordBytes.data(), count);
        Put32(recordBytes.data() + 4, fixture.recordsReserved);
        for (std::size_t i = 0; i < fixture.records.size() && i < count; ++i) {
            std::uint8_t* const record = recordBytes.data() + Kernel::kHandoffHeaderBytes + i * Kernel::kHandoffRecordBytes;
            Put64(record, fixture.records[i].phys);
            Put32(record + 8, fixture.records[i].bytes);
            Put16(record + 12, fixture.records[i].artifactIndex);
            Put16(record + 14, fixture.records[i].reserved);
        }
        const std::uint32_t manifestBytes = fixture.manifestBytes == kUseActualBytes
            ? static_cast<std::uint32_t>(fixture.manifest.size()) : fixture.manifestBytes;
        const std::uint32_t listBytes = fixture.listBytes == kUseActualBytes
            ? static_cast<std::uint32_t>(fixture.list.size()) : fixture.listBytes;
        AddRegion(fixture.manifestPhys, fixture.manifest.data(), fixture.manifest.size(), fixture.manifestReadable);
        AddRegion(fixture.listPhys, fixture.list.data(), fixture.list.size(), fixture.listReadable);
        AddRegion(fixture.recordsPhys, recordBytes.data(), recordBytes.size(), fixture.recordsReadable);
        for (std::size_t i = 0; i < fixture.records.size(); ++i) {
            const bool readable = i < fixture.moduleReadable.size() ? fixture.moduleReadable[i] : true;
            AddRegion(fixture.records[i].phys, fixture.modules[i].data(), fixture.modules[i].size(), readable);
        }
        Boot::Info info{};
        info.kernelSetManifest = fixture.manifestPhys;
        info.kernelSetManifestBytes = manifestBytes;
        info.bootList = fixture.listPhys;
        info.bootListBytes = listBytes;
        info.bootModules = fixture.recordsPhys;
        Outcome outcome;
        const char* reason = nullptr;
        const Kernel::Diagnostics::Diagnostic diagnostic = Kernel::CheckHandoff(info,
            fixture.osBuildIdNull ? nullptr : fixture.osBuildId.data(), fixture.osMajor, outcome.facts, &reason, HostMap);
        if (diagnostic.code != nullptr) outcome.code = diagnostic.code;
        if (reason != nullptr) outcome.reason = reason;
        return outcome;
    }

    // 期待返回指定编号
    void ExpectCode(const Fixture& fixture, const Kernel::Diagnostics::Diagnostic& expected) {
        const Outcome outcome = Run(fixture);
        if (outcome.code == expected.code) return;
        Fail("expected " + std::string(expected.code) + ", got "
            + (outcome.code.empty() ? "success" : outcome.code)
            + (outcome.reason.empty() ? "" : " (" + outcome.reason + ")"));
    }

    // 期待成功并返回事实
    Kernel::HandoffFacts ExpectSuccess(const Fixture& fixture) {
        const Outcome outcome = Run(fixture);
        if (!outcome.code.empty()) Fail("expected success, got " + outcome.code + " (" + outcome.reason + ")");
        return outcome.facts;
    }

    // 解析清单视图；失败即中止
    Kernel::ManifestView ParseManifestOrFail(const std::vector<std::uint8_t>& manifest) {
        Kernel::ManifestView view{};
        const char* const error = Kernel::ParseManifest(manifest.data(), static_cast<std::uint32_t>(manifest.size()), view);
        if (error != nullptr) Fail(std::string("fixture manifest doesn't parse: ") + error);
        return view;
    }

    // 清单里第 index 个条目的文件偏移
    std::size_t ManifestEntryOffset(const std::vector<std::uint8_t>& manifest, unsigned index) {
        std::size_t offset = ML::kFieldArtifacts;
        for (unsigned i = 0;; ++i) {
            if (i == index) return offset;
            const std::uint16_t pathBytes = static_cast<std::uint16_t>(manifest[offset + ML::kEntryPathBytes]
                | (manifest[offset + ML::kEntryPathBytes + 1] << 8));
            offset += ML::kEntryFixedBytes + pathBytes;
            if (offset >= manifest.size()) Fail("manifest entry index is out of range");
        }
    }

    // 找第一个指定类型的清单条目下标，找不到返回 entryCount
    unsigned FindManifestEntry(const std::vector<std::uint8_t>& manifest, std::uint8_t type) {
        const Kernel::ManifestView view = ParseManifestOrFail(manifest);
        for (unsigned i = 0; i < view.entryCount; ++i) {
            if (view.entries[i].type == type) return i;
        }
        return view.entryCount;
    }

    // 改写清单条目的 DataBytes（负例用来制造长度不符与短容器）
    void SetManifestEntryBytes(Fixture& fixture, unsigned index, std::uint64_t bytes) {
        const std::size_t offset = ManifestEntryOffset(fixture.manifest, index);
        Put64(fixture.manifest.data() + offset + ML::kEntryDataBytes, bytes);
    }

    // 列表副本里的等长替换（替换后清单登记的列表长度仍成立）
    void ReplaceInList(Fixture& fixture, const std::string& from, const std::string& to) {
        if (from.size() != to.size()) Fail("in-place list replacement must keep the length");
        const std::string text(fixture.list.begin(), fixture.list.end());
        const std::size_t position = text.find(from);
        if (position == std::string::npos) Fail("fixture list doesn't contain " + from);
        std::copy(to.begin(), to.end(), fixture.list.begin() + static_cast<std::ptrdiff_t>(position));
    }

    // 删除列表里的一条 module 行，并把清单登记的列表长度改成新长度
    void RemoveListModule(Fixture& fixture, const std::string& path) {
        const std::string text(fixture.list.begin(), fixture.list.end());
        const std::string line = "module " + path + "\n";
        const std::size_t position = text.find(line);
        if (position == std::string::npos) Fail("fixture list doesn't contain " + path);
        const std::string updated = text.substr(0, position) + text.substr(position + line.size());
        fixture.list.assign(updated.begin(), updated.end());
        const unsigned listEntry = FindManifestEntry(fixture.manifest, ML::kTypeBootList);
        if (listEntry == ParseManifestOrFail(fixture.manifest).entryCount) Fail("fixture manifest has no Modules.list entry");
        SetManifestEntryBytes(fixture, listEntry, fixture.list.size());
    }

    // 复制模块字节里的 ModuleId：负例用来制造期望集重复成员
    void CopyModuleId(Fixture& fixture, unsigned from, unsigned to) {
        std::memcpy(fixture.modules[to].data() + Kernel::kModuleIdOffset,
            fixture.modules[from].data() + Kernel::kModuleIdOffset, Kernel::kModuleIdBytes);
    }

    // 在清单副本里做等长替换（负例用；清单长度与条目表不变）
    void ReplaceInManifest(Fixture& fixture, const std::string& from, const std::string& to) {
        if (from.size() != to.size()) Fail("in-place manifest replacement must keep the length");
        const std::string text(fixture.manifest.begin(), fixture.manifest.end());
        const std::size_t position = text.find(from);
        if (position == std::string::npos) Fail("fixture manifest doesn't contain " + from);
        std::copy(to.begin(), to.end(), fixture.manifest.begin() + static_cast<std::ptrdiff_t>(position));
    }

    // 往清单副本追加一条 .osm 条目：重写头部计数与长度，条目区之后保留原签名区
    void AppendManifestEntry(Fixture& fixture, const std::string& path, std::uint64_t dataBytes) {
        const Kernel::ManifestView view = ParseManifestOrFail(fixture.manifest);
        const std::size_t signatureBegin = fixture.manifest.size() - ML::kSignatureAreaBytes;
        std::vector<std::uint8_t> entry(ML::kEntryFixedBytes + path.size(), 0);
        entry[ML::kEntryType] = ML::kTypeOsm;
        Put16(entry.data() + ML::kEntryPathBytes, static_cast<std::uint16_t>(path.size()));
        Put64(entry.data() + ML::kEntryDataBytes, dataBytes);
        std::memset(entry.data() + ML::kEntryDigest, 0x5A, ML::kEntryDigestBytes);
        std::memcpy(entry.data() + ML::kEntryPath, path.data(), path.size());
        std::vector<std::uint8_t> updated;
        updated.insert(updated.end(), fixture.manifest.begin(), fixture.manifest.begin() + static_cast<std::ptrdiff_t>(signatureBegin));
        updated.insert(updated.end(), entry.begin(), entry.end());
        updated.insert(updated.end(), fixture.manifest.begin() + static_cast<std::ptrdiff_t>(signatureBegin), fixture.manifest.end());
        Put32(updated.data() + ML::kFieldBytes, static_cast<std::uint32_t>(updated.size()));
        Put16(updated.data() + ML::kFieldArtifactCount, static_cast<std::uint16_t>(view.entryCount + 1));
        fixture.manifest = std::move(updated);
    }

    // 在列表副本尾部追加一条 module 行，并把清单 Type=3 条目登记的长度改成新长度
    void AppendListModule(Fixture& fixture, const std::string& path) {
        std::string text(fixture.list.begin(), fixture.list.end());
        text += "module " + path + "\n";
        fixture.list.assign(text.begin(), text.end());
        const unsigned listEntry = FindManifestEntry(fixture.manifest, ML::kTypeBootList);
        if (listEntry == ParseManifestOrFail(fixture.manifest).entryCount) Fail("fixture manifest has no Modules.list entry");
        SetManifestEntryBytes(fixture, listEntry, fixture.list.size());
    }

    // 从真实集产物构造基础夹具：清单、列表、.os 身份与 13 个预装模块
    Fixture LoadBaseFixture(const std::string& stageDir) {
        Fixture fixture;
        // .os：用内核镜像数据校验取发布线主版本与 BuildId
        const std::vector<std::uint8_t> os = ReadFile(stageDir + "/LikesProgram.os");
        Kernel::ImageFacts osFacts{};
        const char* const osError = Kernel::CheckImageData(os.data(), static_cast<std::uint32_t>(os.size()), osFacts);
        if (osError != nullptr) throw std::runtime_error(std::string("LikesProgram.os: ") + osError);
        fixture.osMajor = osFacts.moduleMajor;
        fixture.osBuildId.assign(os.begin() + Kernel::ImageDataLayout::kFieldBuildId,
            os.begin() + Kernel::ImageDataLayout::kFieldBuildId + Boot::InfoLayout::kKernelSetBuildIdBytes);
        // 清单与列表副本
        fixture.manifest = ReadFile(stageDir + "/Manifest.mft");
        fixture.list = ReadFile(stageDir + "/Modules.list");
        const Kernel::ManifestView view = ParseManifestOrFail(fixture.manifest);
        // 每条 Type=2 条目一条记录：模块字节按记录序号铺开物理地址
        for (unsigned i = 0; i < view.entryCount; ++i) {
            const Kernel::ManifestEntry& entry = view.entries[i];
            if (entry.type != ML::kTypeOsm) continue;
            const std::string path(reinterpret_cast<const char*>(entry.path), entry.pathBytes);
            std::vector<std::uint8_t> module = ReadFile(stageDir + "/" + path);
            if (module.size() != entry.dataBytes) {
                throw std::runtime_error("staged module length doesn't match the manifest: " + path);
            }
            Record record;
            record.phys = kModulePhysBase + fixture.modules.size() * kModulePhysStride;
            record.bytes = static_cast<std::uint32_t>(module.size());
            record.artifactIndex = static_cast<std::uint16_t>(i);
            fixture.records.push_back(record);
            fixture.modules.push_back(std::move(module));
        }
        fixture.moduleReadable.assign(fixture.records.size(), true);
        return fixture;
    }

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // 全局基础夹具：用例都从它复制再改
    Fixture g_base;

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // ---- 正例 ----
        cases.push_back({ "real kernel set package passes", [] {
            const Kernel::HandoffFacts facts = ExpectSuccess(g_base);
            Require(facts.recordCount == g_base.records.size(), "record count is wrong");
            Require(facts.closureCount == 13, "closure count isn't 13");
            Require(facts.runtimeCount == 0, "formal set must have no runtime module");
            Require(facts.recordCount == facts.closureCount, "record count doesn't match the closure count");
            Require(facts.manifest.entryCount == 15, "manifest entry count isn't 15");
            Require(facts.list.count == 13, "list module count isn't 13");
            Require(facts.list.major == g_base.osMajor, "list major doesn't match the kernel");
            Require(facts.list.revision == 1, "list revision isn't 1");
            Require(facts.moduleMajor == g_base.osMajor, "module major isn't the kernel major");
            Require(facts.moduleRevision == 1, "module revision isn't 1");
            Require(facts.buildId[0] == g_base.osBuildId[0], "BuildId doesn't match the kernel image");
            std::uint64_t moduleBytes = 0;
            for (std::size_t i = 0; i < g_base.modules.size(); ++i) {
                moduleBytes += g_base.modules[i].size();
                Require(facts.moduleRanges[i].phys == g_base.records[i].phys, "module range address is wrong");
                Require(facts.moduleRanges[i].bytes == g_base.records[i].bytes, "module range length is wrong");
            }
            Require(facts.moduleBytes == moduleBytes, "module byte total is wrong");
            Require(facts.manifestRange.phys == g_base.manifestPhys && facts.manifestRange.bytes == g_base.manifest.size(), "manifest range is wrong");
            Require(facts.listRange.phys == g_base.listPhys && facts.listRange.bytes == g_base.list.size(), "list range is wrong");
            Require(facts.recordsRange.bytes == Kernel::kHandoffHeaderBytes + facts.recordCount * Kernel::kHandoffRecordBytes, "record range length is wrong");
        } });
        cases.push_back({ "MapPhysical rejects unusable ranges", [] {
            Require(Kernel::MapPhysical(0, 16) == nullptr, "zero address was mapped");
            Require(Kernel::MapPhysical(0x100000000ull, 16) == nullptr, "address above the window was mapped");
            Require(Kernel::MapPhysical(0xFFFFFFFFull, 2) == nullptr, "range crossing the window end was mapped");
            Require(Kernel::MapPhysical(0x1000, 16) != nullptr, "in-window address wasn't mapped");
        } });
        // ---- 第 2 步：存在性 ----
        cases.push_back({ "missing manifest copy is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.manifestPhys = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "zero manifest length is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.manifestBytes = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "missing list copy is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.listPhys = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "zero list length is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.listBytes = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "missing record array is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.recordsPhys = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        // ---- 第 3/4 步：清单副本 ----
        cases.push_back({ "manifest address above the window is E-KRN-51", [] {
            Fixture fixture = g_base;
            fixture.manifestPhys = 0x100000000ull;
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestCopy);
        } });
        cases.push_back({ "manifest range crossing the window end is E-KRN-51", [] {
            Fixture fixture = g_base;
            fixture.manifestPhys = 0xFFFFFF00ull;
            fixture.manifestBytes = 0x200;
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestCopy);
        } });
        cases.push_back({ "manifest length above the limit is E-KRN-51", [] {
            Fixture fixture = g_base;
            fixture.manifestBytes = ML::kManifestMaxBytes + 1;
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestCopy);
        } });
        cases.push_back({ "unreadable manifest copy is E-KRN-51", [] {
            Fixture fixture = g_base;
            fixture.manifestReadable = false;
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestCopy);
        } });
        cases.push_back({ "manifest magic mismatch is E-KRN-52", [] {
            Fixture fixture = g_base;
            fixture.manifest[0] = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestFormat);
        } });
        cases.push_back({ "manifest artifact count below the minimum is E-KRN-52", [] {
            Fixture fixture = g_base;
            Put16(fixture.manifest.data() + ML::kFieldArtifactCount, 1);
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestFormat);
        } });
        cases.push_back({ "manifest flags nonzero is E-KRN-52", [] {
            Fixture fixture = g_base;
            Put32(fixture.manifest.data() + ML::kFieldFlags, 1);
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestFormat);
        } });
        cases.push_back({ "manifest signature algorithm is E-KRN-52", [] {
            Fixture fixture = g_base;
            fixture.manifest[fixture.manifest.size() - ML::kSignatureAreaBytes + ML::kSignatureAlgorithm] = 2;
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestFormat);
        } });
        cases.push_back({ "truncated manifest copy is E-KRN-52", [] {
            Fixture fixture = g_base;
            fixture.manifest.resize(100);
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestFormat);
        } });
        cases.push_back({ "manifest entry running past the entry area is E-KRN-52", [] {
            Fixture fixture = g_base;
            Put16(fixture.manifest.data() + ML::kFieldArtifacts + ML::kEntryPathBytes, 0xFFFF);
            ExpectCode(fixture, Kernel::Diagnostics::kFailManifestFormat);
        } });
        // ---- 第 3/4 步：列表副本 ----
        cases.push_back({ "list address above the window is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.listPhys = 0x100000000ull;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "list length above the limit is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.listBytes = Kernel::kHandoffListMaxBytes + 1;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "unreadable list copy is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.listReadable = false;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "list magic mismatch is E-KRN-55", [] {
            Fixture fixture = g_base;
            fixture.list[0] = 'X';
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "truncated list copy is E-KRN-55", [] {
            Fixture fixture = g_base;
            fixture.list.pop_back();
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "list entry outside Modules/ is E-KRN-55", [] {
            Fixture fixture = g_base;
            ReplaceInList(fixture, "Modules/", "Modulex/");
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "duplicate list path is E-KRN-55", [] {
            Fixture fixture = g_base;
            ReplaceInList(fixture, "Modules/Nvme.1.0.0.osm", "Modules/Ahci.1.0.0.osm");
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "zero list revision is E-KRN-55", [] {
            Fixture fixture = g_base;
            ReplaceInList(fixture, "revision 1", "revision 0");
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "list entry prefix is E-KRN-55", [] {
            Fixture fixture = g_base;
            ReplaceInList(fixture, "module Modules/DevModel", "modulX Modules/DevModel");
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "list length mismatching the manifest is E-KRN-50", [] {
            Fixture fixture = g_base;
            const unsigned listEntry = FindManifestEntry(fixture.manifest, ML::kTypeBootList);
            SetManifestEntryBytes(fixture, listEntry, fixture.list.size() + 1);
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        // ---- 第 3/5 步：记录数组与记录 ----
        cases.push_back({ "unreadable record array is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.recordsReadable = false;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "zero record count is E-KRN-50", [] {
            Fixture fixture = g_base;
            fixture.recordCount = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailHandoffMissing);
        } });
        cases.push_back({ "record count above the limit is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.recordCount = Kernel::kHandoffMaxRecords + 1;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "record array reserved field is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.recordsReserved = 1;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "record array address above the window is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.recordsPhys = 0x100000000ull;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "record reserved field is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.records[0].reserved = 1;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "zero record length is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.records[0].bytes = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "record artifact index beyond the manifest is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.records[0].artifactIndex = 0xFFFF;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "record address above the window is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.records[0].phys = 0x100000000ull;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "record range crossing the window end is E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.records[0].phys = 0xFFFFFFFFull;
            fixture.records[0].bytes = 0x200;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "unreadable module bytes are E-KRN-53", [] {
            Fixture fixture = g_base;
            fixture.moduleReadable[0] = false;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadRecords);
        } });
        cases.push_back({ "record pointing at a non-.osm entry is E-KRN-54", [] {
            Fixture fixture = g_base;
            fixture.records[0].artifactIndex = static_cast<std::uint16_t>(FindManifestEntry(fixture.manifest, ML::kTypeOs));
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadEntry);
        } });
        cases.push_back({ "record length mismatching the manifest is E-KRN-54", [] {
            Fixture fixture = g_base;
            fixture.records[0].bytes += 4;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadEntry);
        } });
        cases.push_back({ "record path not in the list is E-KRN-54", [] {
            Fixture fixture = g_base;
            ReplaceInList(fixture, "Modules/DevModel.1.0.0.osm", "Modules/DevModex.1.0.0.osm");
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadEntry);
        } });
        cases.push_back({ "two records claiming one list entry is E-KRN-54", [] {
            Fixture fixture = g_base;
            fixture.records[1].artifactIndex = fixture.records[0].artifactIndex;
            fixture.records[1].bytes = fixture.records[0].bytes;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadEntry);
        } });
        cases.push_back({ "record count not matching the list is E-KRN-54", [] {
            Fixture fixture = g_base;
            fixture.records.pop_back();
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadEntry);
        } });
        cases.push_back({ "short module container is E-KRN-57", [] {
            Fixture fixture = g_base;
            SetManifestEntryBytes(fixture, fixture.records[0].artifactIndex, 0x40);
            fixture.records[0].bytes = 0x40;
            ExpectCode(fixture, Kernel::Diagnostics::kFailOsmContainer);
        } });
        // ---- 第 6/7 步：主版本门与 BuildId ----
        cases.push_back({ "zero kernel module major is E-KRN-56", [] {
            Fixture fixture = g_base;
            fixture.osMajor = 0;
            ExpectCode(fixture, Kernel::Diagnostics::kFailBuildId);
        } });
        cases.push_back({ "list major mismatch is E-KRN-56", [] {
            Fixture fixture = g_base;
            ReplaceInList(fixture, "major 1", "major 2");
            ExpectCode(fixture, Kernel::Diagnostics::kFailBuildId);
        } });
        cases.push_back({ "module major mismatch is E-KRN-56", [] {
            Fixture fixture = g_base;
            Put16(fixture.modules[0].data() + Kernel::kModuleMajorOffset, 2);
            ExpectCode(fixture, Kernel::Diagnostics::kFailBuildId);
        } });
        cases.push_back({ "BuildId mismatch is E-KRN-56", [] {
            Fixture fixture = g_base;
            fixture.osBuildId[0] ^= 0x01;
            ExpectCode(fixture, Kernel::Diagnostics::kFailBuildId);
        } });
        cases.push_back({ "null BuildId is E-KRN-56", [] {
            Fixture fixture = g_base;
            fixture.osBuildIdNull = true;
            ExpectCode(fixture, Kernel::Diagnostics::kFailBuildId);
        } });
        // ---- 第 8 步：期望集完整性 ----
        cases.push_back({ "unknown module identity is E-KRN-55", [] {
            Fixture fixture = g_base;
            fixture.modules[0][Kernel::kModuleIdOffset] ^= 0x01;
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "manifest entry missing from the list is E-KRN-54", [] {
            Fixture fixture = g_base;
            RemoveListModule(fixture, "Modules/DevModel.1.0.0.osm");
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadEntry);
        } });
        cases.push_back({ "boot closure member replaced by a runtime entry is E-KRN-55", [] {
            Fixture fixture = g_base;
            ReplaceInManifest(fixture, "Modules/DevModel.1.0.0.osm", "Modules/DevModex.1.0.0.osm");
            ReplaceInList(fixture, "Modules/DevModel.1.0.0.osm", "Modules/DevModex.1.0.0.osm");
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        cases.push_back({ "list with runtime entries is accepted", [] {
            Fixture fixture = g_base;
            const std::string runtimePath = "Modules/Runtime.1.0.0.osm";
            AppendManifestEntry(fixture, runtimePath, 0x180);
            AppendListModule(fixture, runtimePath);
            const Kernel::HandoffFacts facts = ExpectSuccess(fixture);
            Require(facts.closureCount == 13, "closure count must stay 13");
            Require(facts.recordCount == 13, "record count must stay 13");
            Require(facts.runtimeCount == 1, "runtime count must be 1");
            Require(facts.list.count == 14, "list count must be 14");
            Require(facts.runtimeListIndex[0] == 13, "runtime entry must be the appended list entry");
            const std::uint16_t artifactIndex = facts.list.artifactIndex[13];
            Require(artifactIndex < facts.manifest.entryCount, "runtime artifact index is out of range");
            Require(facts.manifest.entries[artifactIndex].type == ML::kTypeOsm, "runtime artifact must be .osm");
            Require(facts.list.closureMember[13] == Kernel::kNotClosureMember, "runtime entry must not be a closure member");
        } });
        cases.push_back({ "record claiming a runtime entry is E-KRN-54", [] {
            Fixture fixture = g_base;
            const std::string runtimePath = "Modules/Runtime.1.0.0.osm";
            AppendManifestEntry(fixture, runtimePath, 0x180);
            AppendListModule(fixture, runtimePath);
            const Kernel::ManifestView view = ParseManifestOrFail(fixture.manifest);
            fixture.records.back().artifactIndex = static_cast<std::uint16_t>(view.entryCount - 1);
            fixture.records.back().bytes = 0x180;
            ExpectCode(fixture, Kernel::Diagnostics::kFailPreloadEntry);
        } });
        cases.push_back({ "duplicate boot closure member is E-KRN-55", [] {
            Fixture fixture = g_base;
            CopyModuleId(fixture, 0, 1);
            ExpectCode(fixture, Kernel::Diagnostics::kFailBootClosure);
        } });
        return cases;
    }
}

// 入口：加载真实集产物，跑全部用例并汇总；产物缺失返回 2
int main(int argc, char** argv) {
    std::string stageDir = kDefaultStage;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            std::cout <<
                "CheckHandoff - host-side check of the kernel handoff package (9-step validation)\n"
                "\n"
                "Usage: CheckHandoff [--stage <dir>]\n"
                "  --stage  kernel set stage directory with LikesProgram.os, Manifest.mft,\n"
                "           Modules.list and Modules/ (default: Packages/LikesProgramOS/Out/KernelSetStage)\n"
                "Exit codes: 0 all cases passed, 1 a case failed, 2 the real kernel set artifacts are missing\n";
            return 0;
        } else if (argument == "--stage") {
            if (i + 1 >= argc) {
                std::cerr << "CheckHandoff: --stage expects a value\n";
                return 2;
            }
            stageDir = argv[++i];
        } else {
            std::cerr << "CheckHandoff: unknown option " << argument << "\n";
            return 2;
        }
    }
    try {
        g_base = LoadBaseFixture(stageDir);
    } catch (const std::exception& error) {
        std::cerr << "CheckHandoff: infrastructure missing: " << error.what() << "\n";
        return 2;
    }
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckHandoff: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckHandoff: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckHandoff: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
