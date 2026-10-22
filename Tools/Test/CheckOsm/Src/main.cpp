/* main.cpp
    CheckOsm：内核模块容器（.osm）的宿主侧检查，复用 PackModule 的写出器与解析校验器

    用例分四类：
    1. 正例：写出器现场构造规范容器，解析器独立复算通过，并核对字段与规范化布局
    2. 逐字段负例：头部、表区边界与重叠、段权限与区间、入口、导出/导入/依赖、重定位、
       字符串表、依赖图（含 A→B→A 环）逐项改坏，必须被拒绝且原因命中关键字
    3. 确定性：同一构造输入两次写出的字节一致
    4. 依赖图：正例通过，自依赖、缺失与环被拒绝

    每个用例打印 [ OK ] 或 [ FAIL ]；失败用例计数，任一失败返回 1
    用例数固定为 kCaseCount，运行时与实跑数核对，防止用例被误删
*/

#include <Osm.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace L = osm::Layout;
    // 容器字节序列
    using Bytes = std::vector<std::uint8_t>;
    // 用例总数：与用例表长度核对
    constexpr unsigned kCaseCount = 78;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) {
        if (!value) Fail(message);
    }

    // 小端读取 16 位
    std::uint16_t Get16(const Bytes& bytes, std::size_t offset) {
        return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
    }

    // 小端读取 32 位
    std::uint32_t Get32(const Bytes& bytes, std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
        return value;
    }

    // 小端写入 16 位
    void Put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
        for (unsigned i = 0; i < 2; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 小端写入 32 位
    void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 构造一个段：整页内存跨度，fileBytes 决定落盘前缀长度
    osm::SectionSpec MakeSection(const char* name, std::uint32_t type, std::uint32_t flags,
        std::uint32_t memoryOffset, std::uint32_t fileBytes) {
        osm::SectionSpec section;
        section.name = name;
        section.type = type;
        section.flags = flags;
        section.memoryOffset = memoryOffset;
        section.memoryBytes = L::kPageBytes;
        section.align = L::kPageBytes;
        section.fileBytes.assign(fileBytes, 0x90);
        return section;
    }

    // 规范输入：TEXT/RODATA/DATA/BSS 各一页，一条导出、一条导入、一条依赖、两条重定位
    osm::Spec CanonicalSpec() {
        osm::Spec spec;
        spec.moduleName = "FixtureModule";
        spec.versionMajor = 1;
        spec.versionMinor = 2;
        spec.versionPatch = 3;
        spec.abiMin = 1;
        spec.abiMax = L::kAbiMaxUnlimited;
        spec.entryInit = 0x10;
        spec.entryActivate = 0x18;
        spec.entryAbort = 0;
        spec.sections.push_back(MakeSection(".text", L::kTypeText, L::kFlagRead | L::kFlagExecute, 0, 0x20));
        spec.sections.push_back(MakeSection(".rodata", L::kTypeRodata, L::kFlagRead, 0x1000, 8));
        spec.sections.push_back(MakeSection(".data", L::kTypeData, L::kFlagRead | L::kFlagWrite, 0x2000, 0x40));
        spec.sections.push_back(MakeSection(".bss", L::kTypeBss, L::kFlagRead | L::kFlagWrite, 0x3000, 0));
        spec.exports.push_back({ "Demo.Iface", 1, { 0x10, 0x18 } });
        spec.imports.push_back({ "Core.Page", 1, 1, { 0x3000, 0x3008, 0x3010, 0x3018 } });
        spec.dependencies.push_back({ "DevModel", 1, 0, 0, 0, 0, 0 });
        spec.relocations.push_back({ L::kRelocationRelative64, 0x2000, 0x10 });
        spec.relocations.push_back({ L::kRelocationRelative32, 0x2008, 0x1000 });
        return spec;
    }

    // 规范容器的字节
    Bytes Canonical() { return osm::Build(CanonicalSpec()); }

    // 第 index 个段表条目的文件偏移
    std::size_t SectionEntry(const Bytes& image, unsigned index) {
        return static_cast<std::size_t>(Get16(image, L::kFieldSectionTableOffset)) + index * L::kSectionEntryBytes;
    }

    // 期待容器被拒绝，且原因包含给定片段
    void ExpectReject(const Bytes& image, const std::string& token) {
        try {
            osm::Parse(image);
        } catch (const std::exception& error) {
            const std::string reason = error.what();
            if (reason.find(token) == std::string::npos) {
                Fail("rejected for an unexpected reason: " + reason + " (expected '" + token + "')");
            }
            return;
        }
        Fail("container was accepted but should have been rejected (expected '" + token + "')");
    }

    // 由改坏的输入构造容器并期待拒绝
    void ExpectRejectSpec(osm::Spec spec, const std::string& token) { ExpectReject(osm::Build(spec), token); }

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // ---- 正例 ----
        cases.push_back({ "canonical container is accepted", [] {
            const osm::Image image = osm::Parse(Canonical());
            Require(image.totalBytes == osm::Build(CanonicalSpec()).size(), "TotalBytes does not equal the built length");
            Require(image.imageBytes == 0x4000, "ImageBytes is not 0x4000");
            Require(image.sections.size() == 4, "section count is not 4");
            Require(image.sections[0].type == L::kTypeText && image.sections[0].memoryOffset == 0, "section 0 is not TEXT at 0");
            Require(image.sections[1].type == L::kTypeRodata && image.sections[1].memoryOffset == 0x1000, "section 1 is not RODATA at 0x1000");
            Require(image.sections[2].type == L::kTypeData && image.sections[2].memoryOffset == 0x2000, "section 2 is not DATA at 0x2000");
            Require(image.sections[3].type == L::kTypeBss && image.sections[3].fileBytes == 0, "section 3 is not fileless BSS");
            Require(image.sections[0].name == ".text" && image.sections[3].name == ".bss", "section names are wrong");
            Require(image.moduleName == "FixtureModule", "module name is wrong");
            Require(image.moduleId == osm::DeriveModuleId("FixtureModule"), "ModuleId is not derived from the logical name");
            Require(image.versionMajor == 1 && image.versionMinor == 2 && image.versionPatch == 3, "module version is wrong");
            Require(image.abiMin == 1 && image.abiMax == 0xFFFF, "kernel ABI range is wrong");
            Require(image.entryInit == 0x10 && image.entryActivate == 0x18 && image.entryAbort == 0, "entry offsets are wrong");
            Require(image.exports.size() == 1 && image.exports[0].interfaceId == osm::DeriveInterfaceId("Demo.Iface"), "export identity is wrong");
            Require(image.exports[0].version == 1 && image.exports[0].slots == std::vector<std::uint32_t>({ 0x10, 0x18 }), "export slots are wrong");
            Require(image.imports.size() == 1 && image.imports[0].interfaceId == osm::DeriveInterfaceId("Core.Page"), "import identity is wrong");
            Require(image.imports[0].minVersion == 1 && image.imports[0].maxVersion == 1, "import version range is wrong");
            Require(image.imports[0].patchSlots == std::vector<std::uint32_t>({ 0x3000, 0x3008, 0x3010, 0x3018 }), "import patch slots are wrong");
            Require(image.dependencies.size() == 1 && image.dependencies[0].moduleId == osm::DeriveModuleId("DevModel"), "dependency identity is wrong");
            Require(image.dependencies[0].minMajor == 1 && image.dependencies[0].maxMajor == 0, "dependency range is wrong");
            Require(image.relocations.size() == 2, "relocation count is not 2");
            Require(image.relocations[0].target == 0x2000 && image.relocations[1].target == 0x2008, "relocations are not sorted by target");
        } });
        cases.push_back({ "empty optional tables are accepted", [] {
            osm::Spec spec = CanonicalSpec();
            spec.exports.clear();
            spec.imports.clear();
            spec.dependencies.clear();
            const osm::Image image = osm::Parse(osm::Build(spec));
            Require(image.exports.empty() && image.imports.empty() && image.dependencies.empty(), "optional tables are not empty");
            Require(image.relocations.size() == 2, "relocations were lost");
        } });
        cases.push_back({ "full page file bytes are accepted", [] {
            osm::Spec spec = CanonicalSpec();
            for (osm::SectionSpec& section : spec.sections) {
                if (section.type != L::kTypeBss) section.fileBytes.assign(section.memoryBytes, 0);
            }
            const osm::Image image = osm::Parse(osm::Build(spec));
            for (const osm::SectionInfo& section : image.sections) {
                if (section.type != L::kTypeBss) Require(section.fileBytes == section.memoryBytes, "file-backed section is not full-page");
            }
        } });
        cases.push_back({ "same input builds identical bytes", [] {
            const Bytes first = osm::Build(CanonicalSpec());
            const Bytes second = osm::Build(CanonicalSpec());
            Require(first == second, "two builds of the same input differ");
            Require(!first.empty(), "build produced no bytes");
        } });
        cases.push_back({ "valid dependency graph is accepted", [] {
            const osm::ModuleId alpha = osm::DeriveModuleId("Alpha");
            const osm::ModuleId beta = osm::DeriveModuleId("Beta");
            std::vector<osm::DependencyNode> nodes(2);
            nodes[0].moduleId = alpha;
            nodes[0].name = "Alpha";
            nodes[0].dependencies.push_back(beta);
            nodes[1].moduleId = beta;
            nodes[1].name = "Beta";
            osm::VerifyDependencyGraph(nodes);
        } });
        cases.push_back({ "RELATIVE32 relocation is accepted", [] {
            const osm::Image image = osm::Parse(Canonical());
            Require(image.relocations.size() == 2, "relocation count is not 2");
            Require(image.relocations[1].type == L::kRelocationRelative32, "second relocation is not RELATIVE32");
            Require(image.relocations[1].addend == 0x1000, "RELATIVE32 addend is wrong");
        } });
        cases.push_back({ "same interface with two versions is accepted", [] {
            // 同一接口允许多版本导出，唯一性口径是 InterfaceId + Version
            osm::Spec spec = CanonicalSpec();
            spec.exports.push_back({ "Demo.Iface", 2, { 0x10 } });
            const osm::Image image = osm::Parse(osm::Build(spec));
            Require(image.exports.size() == 2, "two versions of one interface weren't accepted");
            Require(image.exports[0].version == 1 && image.exports[1].version == 2, "export version order is wrong");
        } });
        cases.push_back({ "container without module name is accepted", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldModuleNameOffset, 0);
            Put16(image, L::kFieldModuleNameBytes, 0);
            const osm::Image parsed = osm::Parse(image);
            Require(parsed.moduleName.empty(), "module name is not empty");
        } });
        // ---- 头部负例 ----
        cases.push_back({ "magic mismatch is rejected", [] {
            Bytes image = Canonical();
            image[0] = 0;
            ExpectReject(image, "magic");
        } });
        cases.push_back({ "unknown container version is rejected", [] {
            Bytes image = Canonical();
            image[L::kFieldVersion] = 2;
            image[L::kFieldVersion + 1] = 0;
            ExpectReject(image, "unsupported container version");
        } });
        cases.push_back({ "HeaderBytes mismatch is rejected", [] {
            Bytes image = Canonical();
            image[L::kFieldHeaderBytes] = 0x7F;
            ExpectReject(image, "HeaderBytes");
        } });
        cases.push_back({ "nonzero header Flags is rejected", [] {
            Bytes image = Canonical();
            image[L::kFieldFlags] = 1;
            ExpectReject(image, "Flags");
        } });
        cases.push_back({ "TotalBytes mismatch is rejected", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldTotalBytes, static_cast<std::uint32_t>(image.size()) + 4);
            ExpectReject(image, "TotalBytes");
        } });
        cases.push_back({ "ImageBytes not a page multiple is rejected", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldImageBytes, 0x1234);
            ExpectReject(image, "ImageBytes");
        } });
        cases.push_back({ "nonzero header Reserved is rejected", [] {
            Bytes image = Canonical();
            image[L::kFieldReserved] = 1;
            ExpectReject(image, "Reserved");
        } });
        cases.push_back({ "zero section count is rejected", [] {
            Bytes image = Canonical();
            Put16(image, L::kFieldSectionCount, 0);
            ExpectReject(image, "section count");
        } });
        cases.push_back({ "SectionEntryBytes mismatch is rejected", [] {
            Bytes image = Canonical();
            Put16(image, L::kFieldSectionEntryBytes, 31);
            ExpectReject(image, "SectionEntryBytes");
        } });
        cases.push_back({ "ImportEntryBytes mismatch is rejected", [] {
            Bytes image = Canonical();
            Put16(image, L::kFieldImportEntryBytes, 27);
            ExpectReject(image, "ImportEntryBytes");
        } });
        cases.push_back({ "DependencyEntryBytes mismatch is rejected", [] {
            Bytes image = Canonical();
            Put16(image, L::kFieldDependencyEntryBytes, 31);
            ExpectReject(image, "DependencyEntryBytes");
        } });
        cases.push_back({ "section table offset misalignment is rejected", [] {
            Bytes image = Canonical();
            Put16(image, L::kFieldSectionTableOffset, Get16(image, L::kFieldSectionTableOffset) + 1);
            ExpectReject(image, "4-byte aligned");
        } });
        cases.push_back({ "export table offset out of range is rejected", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldExportTableOffset, static_cast<std::uint32_t>(image.size()));
            ExpectReject(image, "out of range");
        } });
        cases.push_back({ "export table offset misalignment is rejected", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldExportTableOffset, Get32(image, L::kFieldExportTableOffset) + 2);
            ExpectReject(image, "4-byte aligned");
        } });
        cases.push_back({ "relocation table offset misalignment is rejected", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldRelocationTableOffset, Get32(image, L::kFieldRelocationTableOffset) + 4);
            ExpectReject(image, "8-byte aligned");
        } });
        cases.push_back({ "overlapping table regions are rejected", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldDependencyTableOffset, Get32(image, L::kFieldRelocationTableOffset));
            ExpectReject(image, "overlap");
        } });
        cases.push_back({ "string table out of range is rejected", [] {
            Bytes image = Canonical();
            Put32(image, L::kFieldStringTableOffset, static_cast<std::uint32_t>(image.size()) - 2);
            Put32(image, L::kFieldStringTableBytes, 16);
            ExpectReject(image, "string table");
        } });
        cases.push_back({ "string table not NUL-terminated is rejected", [] {
            Bytes image = Canonical();
            const std::size_t offset = Get32(image, L::kFieldStringTableOffset);
            const std::size_t bytes = Get32(image, L::kFieldStringTableBytes);
            image[offset + bytes - 1] = 'X';
            ExpectReject(image, "NUL");
        } });
        cases.push_back({ "section name offset outside the string table is rejected", [] {
            Bytes image = Canonical();
            Put32(image, SectionEntry(image, 0) + L::kSectionNameOffset, 0xFFFF);
            ExpectReject(image, "name offset");
        } });
        cases.push_back({ "export count above the limit is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.exports.clear();
            for (unsigned i = 0; i < 129; ++i) {
                spec.exports.push_back({ "Demo.Iface" + std::to_string(i), 1, { 0x10 } });
            }
            ExpectRejectSpec(spec, "export count");
        } });
        cases.push_back({ "relocation count above the limit is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.relocations.clear();
            for (unsigned i = 0; i < 65537; ++i) spec.relocations.push_back({ L::kRelocationRelative64, 0x2000, 0 });
            ExpectRejectSpec(spec, "relocation count");
        } });
        cases.push_back({ "dependency count above the limit is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.dependencies.clear();
            for (unsigned i = 0; i < 33; ++i) spec.dependencies.push_back({ "Dev" + std::to_string(i), 1, 0, 0, 0, 0, 0 });
            ExpectRejectSpec(spec, "dependency count");
        } });
        cases.push_back({ "zero ModuleId is rejected", [] {
            Bytes image = Canonical();
            for (std::size_t i = 0; i < L::kIdBytes; ++i) image[L::kFieldModuleId + i] = 0;
            ExpectReject(image, "ModuleId");
        } });
        cases.push_back({ "zero ModuleVersionMajor is rejected", [] {
            Bytes image = Canonical();
            image[L::kFieldModuleVersionMajor] = 0;
            image[L::kFieldModuleVersionMajor + 1] = 0;
            ExpectReject(image, "ModuleVersionMajor");
        } });
        cases.push_back({ "zero KernelAbiMin is rejected", [] {
            Bytes image = Canonical();
            image[L::kFieldKernelAbiMin] = 0;
            image[L::kFieldKernelAbiMin + 1] = 0;
            ExpectReject(image, "KernelAbiMin");
        } });
        cases.push_back({ "module name offset/byte count inconsistency is rejected", [] {
            Bytes image = Canonical();
            Put16(image, L::kFieldModuleNameBytes, 0);
            ExpectReject(image, "inconsistent");
        } });
        cases.push_back({ "module name outside the string table is rejected", [] {
            Bytes image = Canonical();
            Put16(image, L::kFieldModuleNameBytes, 0xFFFF);
            ExpectReject(image, "ModuleName");
        } });
        // ---- 段负例 ----
        cases.push_back({ "section memory offset not 4 KiB aligned is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[0].memoryOffset = 0x800;
            ExpectRejectSpec(spec, "4 KiB aligned");
        } });
        cases.push_back({ "section memory span not 4 KiB multiple is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[1].memoryBytes = 0x800;
            ExpectRejectSpec(spec, "4 KiB multiple");
        } });
        cases.push_back({ "section memory ranges overlap is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[1].memoryOffset = 0;
            ExpectRejectSpec(spec, "memory ranges overlap");
        } });
        cases.push_back({ "section flags W+X is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[0].flags = L::kFlagRead | L::kFlagWrite | L::kFlagExecute;
            ExpectRejectSpec(spec, "flags");
        } });
        cases.push_back({ "section Type/Flags mismatch is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[1].flags = L::kFlagRead | L::kFlagWrite;
            ExpectRejectSpec(spec, "flags");
        } });
        cases.push_back({ "unknown section type is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[2].type = 5;
            ExpectRejectSpec(spec, "unknown type");
        } });
        cases.push_back({ "BSS with file bytes is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[3].fileBytes.assign(8, 0);
            ExpectRejectSpec(spec, "BSS");
        } });
        cases.push_back({ "non-BSS section without file bytes is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[1].fileBytes.clear();
            ExpectRejectSpec(spec, "no file bytes");
        } });
        cases.push_back({ "FileBytes exceeding MemoryBytes is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.sections[2].fileBytes.assign(0x2000, 0);
            ExpectRejectSpec(spec, "FileBytes exceeds");
        } });
        cases.push_back({ "section file offset misalignment is rejected", [] {
            Bytes image = Canonical();
            const std::size_t offset = SectionEntry(image, 0) + L::kSectionFileOffset;
            Put32(image, offset, Get32(image, offset) + 1);
            ExpectReject(image, "4-byte aligned");
        } });
        cases.push_back({ "section file range out of bounds is rejected", [] {
            Bytes image = Canonical();
            Put32(image, SectionEntry(image, 0) + L::kSectionFileOffset, static_cast<std::uint32_t>(image.size()) - 4);
            ExpectReject(image, "out of range");
        } });
        cases.push_back({ "BSS with nonzero file offset is rejected", [] {
            Bytes image = Canonical();
            Put32(image, SectionEntry(image, 3) + L::kSectionFileOffset, 0x2000);
            ExpectReject(image, "BSS");
        } });
        // ---- 入口负例 ----
        cases.push_back({ "zero EntryInit is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.entryInit = 0;
            ExpectRejectSpec(spec, "EntryInit");
        } });
        cases.push_back({ "EntryInit outside TEXT is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.entryInit = 0x1000;
            ExpectRejectSpec(spec, "TEXT");
        } });
        cases.push_back({ "EntryActivate outside TEXT is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.entryActivate = 0x2000;
            ExpectRejectSpec(spec, "TEXT");
        } });
        // ---- 导出/导入负例 ----
        cases.push_back({ "zero export version is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.exports[0].version = 0;
            ExpectRejectSpec(spec, "version is zero");
        } });
        cases.push_back({ "zero export slot count is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.exports[0].slots.clear();
            ExpectRejectSpec(spec, "slot count");
        } });
        cases.push_back({ "export slot count above 64 is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.exports[0].slots.assign(65, 0x10);
            ExpectRejectSpec(spec, "slot count");
        } });
        cases.push_back({ "export slot outside TEXT is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.exports[0].slots = { 0x2000 };
            ExpectRejectSpec(spec, "TEXT");
        } });
        cases.push_back({ "duplicate export is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.exports.push_back(spec.exports[0]);
            ExpectRejectSpec(spec, "duplicates");
        } });
        cases.push_back({ "zero export InterfaceId is rejected", [] {
            Bytes image = Canonical();
            const std::size_t offset = Get32(image, L::kFieldExportTableOffset);
            for (std::size_t i = 0; i < L::kIdBytes; ++i) image[offset + L::kExportInterfaceId + i] = 0;
            ExpectReject(image, "InterfaceId");
        } });
        cases.push_back({ "nonzero export Reserved is rejected", [] {
            Bytes image = Canonical();
            Put32(image, Get32(image, L::kFieldExportTableOffset) + L::kExportReserved, 1);
            ExpectReject(image, "Reserved");
        } });
        cases.push_back({ "zero import MinVersion is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports[0].minVersion = 0;
            ExpectRejectSpec(spec, "MinVersion");
        } });
        cases.push_back({ "reversed import version range is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports[0].minVersion = 2;
            spec.imports[0].maxVersion = 1;
            ExpectRejectSpec(spec, "reversed");
        } });
        cases.push_back({ "zero import slot count is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports[0].patchSlots.clear();
            ExpectRejectSpec(spec, "slot count");
        } });
        cases.push_back({ "import slot count above 64 is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports[0].patchSlots.clear();
            for (unsigned i = 0; i < 65; ++i) spec.imports[0].patchSlots.push_back(0x3000 + i * 8);
            ExpectRejectSpec(spec, "slot count");
        } });
        cases.push_back({ "import patch slot not writable is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports[0].patchSlots = { 0x10 };
            ExpectRejectSpec(spec, "writable");
        } });
        cases.push_back({ "import patch slot not 8-byte aligned is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports[0].patchSlots = { 0x3004 };
            ExpectRejectSpec(spec, "8-byte aligned");
        } });
        cases.push_back({ "import patch slot out of bounds is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports[0].patchSlots = { 0x4000 };
            ExpectRejectSpec(spec, "writable");
        } });
        cases.push_back({ "duplicate import is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.imports.push_back(spec.imports[0]);
            ExpectRejectSpec(spec, "duplicates");
        } });
        cases.push_back({ "zero import InterfaceId is rejected", [] {
            Bytes image = Canonical();
            const std::size_t offset = Get32(image, L::kFieldImportTableOffset);
            for (std::size_t i = 0; i < L::kIdBytes; ++i) image[offset + L::kImportInterfaceId + i] = 0;
            ExpectReject(image, "InterfaceId");
        } });
        // ---- 依赖负例 ----
        cases.push_back({ "self dependency is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.dependencies.push_back({ "FixtureModule", 1, 0, 0, 0, 0, 0 });
            ExpectRejectSpec(spec, "self dependency");
        } });
        cases.push_back({ "zero dependency MinMajor is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.dependencies[0].minMajor = 0;
            ExpectRejectSpec(spec, "MinMajor");
        } });
        cases.push_back({ "reversed dependency version range is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.dependencies[0].minMajor = 2;
            spec.dependencies[0].maxMajor = 1;
            ExpectRejectSpec(spec, "reversed");
        } });
        cases.push_back({ "duplicate dependency is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.dependencies.push_back(spec.dependencies[0]);
            ExpectRejectSpec(spec, "duplicates");
        } });
        cases.push_back({ "nonzero dependency Flags is rejected", [] {
            Bytes image = Canonical();
            Put32(image, Get32(image, L::kFieldDependencyTableOffset) + L::kDependencyFlags, 1);
            ExpectReject(image, "Flags");
        } });
        cases.push_back({ "dependency graph cycle is rejected", [] {
            const osm::ModuleId alpha = osm::DeriveModuleId("Alpha");
            const osm::ModuleId beta = osm::DeriveModuleId("Beta");
            std::vector<osm::DependencyNode> nodes(2);
            nodes[0].moduleId = alpha;
            nodes[0].name = "Alpha";
            nodes[0].dependencies.push_back(beta);
            nodes[1].moduleId = beta;
            nodes[1].name = "Beta";
            nodes[1].dependencies.push_back(alpha);
            try {
                osm::VerifyDependencyGraph(nodes);
            } catch (const std::exception& error) {
                if (std::string(error.what()).find("cycle") == std::string::npos) Fail(std::string("unexpected reason: ") + error.what());
                return;
            }
            Fail("dependency cycle was accepted");
        } });
        cases.push_back({ "dependency graph missing module is rejected", [] {
            const osm::ModuleId alpha = osm::DeriveModuleId("Alpha");
            const osm::ModuleId gamma = osm::DeriveModuleId("Gamma");
            std::vector<osm::DependencyNode> nodes(1);
            nodes[0].moduleId = alpha;
            nodes[0].name = "Alpha";
            nodes[0].dependencies.push_back(gamma);
            try {
                osm::VerifyDependencyGraph(nodes);
            } catch (const std::exception& error) {
                if (std::string(error.what()).find("misses module") == std::string::npos) Fail(std::string("unexpected reason: ") + error.what());
                return;
            }
            Fail("missing dependency was accepted");
        } });
        // ---- 重定位负例 ----
        cases.push_back({ "unknown relocation type is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.relocations[0].type = 3;
            ExpectRejectSpec(spec, "unsupported type");
        } });
        cases.push_back({ "relocation target in TEXT is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.relocations[0].target = 0x10;
            ExpectRejectSpec(spec, "writable");
        } });
        cases.push_back({ "misaligned relocation target is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.relocations[0].target = 0x2004;
            ExpectRejectSpec(spec, "aligned");
        } });
        cases.push_back({ "relocation target out of bounds is rejected", [] {
            osm::Spec spec = CanonicalSpec();
            spec.relocations[0].target = 0x4008;
            ExpectRejectSpec(spec, "writable");
        } });
        cases.push_back({ "nonzero relocation Reserved is rejected", [] {
            Bytes image = Canonical();
            image[Get32(image, L::kFieldRelocationTableOffset) + L::kRelocationReserved1] = 1;
            ExpectReject(image, "Reserved");
        } });
        return cases;
    }
}

// 入口：跑全部用例并汇总；任一失败返回 1
int main() {
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckOsm: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckOsm: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckOsm: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
