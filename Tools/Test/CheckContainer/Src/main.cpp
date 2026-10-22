/* main.cpp
    CheckContainer：内核侧容器解析（Core/Src/ModuleContainer.cpp）的宿主侧检查，手工执行

    检查对象是 Packages/LikesProgramOS/Core/Src/ModuleContainer.cpp：用 vpath 按相对路径直接
    编进本工具，不复制实现；正例解析真实模块产物（Packages/KernelModules/Out/Modules 下的 .osm），
    负例在真实容器与合成夹具的字节副本上逐项改坏，按 ContainerError 分类断言（不按文本断言）
    覆盖容器解析的下列检查：头 / 表区越界与重叠 / 对齐 / 段类型与权限 /
    BSS / 内存与文件区间 / 入口落在 TEXT / 导出导入依赖表项 / 重定位目标 / 字符串表 /
    未知版本 / ModuleId 与版本 / 模块名；合成夹具补齐当前玩具模块里为空的导出 / 导入 / 依赖表

    每个用例打印 [ OK ] 或 [ FAIL ]；用例数固定，失败返回 1；真实模块产物缺失返回 2
*/

#include <ModuleContainer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace fs = std::filesystem;
    // 容器字节序列
    using Bytes = std::vector<std::uint8_t>;
    // 用例总数：与用例表长度核对，防止用例被误删
    constexpr unsigned kCaseCount = 89;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;
    // 默认模块产物目录：从仓库根执行时的相对路径，可用 --modules 覆盖
    constexpr const char* kDefaultModulesDir = "Packages/KernelModules/Out/Modules";
    // 容器文件头里的固定字段偏移与长度
    constexpr std::size_t kFieldTotalBytes = 0x10;
    constexpr std::size_t kFieldImageBytes = 0x14;
    constexpr std::size_t kFieldEntryInit = 0x18;
    constexpr std::size_t kFieldEntryActivate = 0x1C;
    constexpr std::size_t kFieldSectionTableOffset = 0x24;
    constexpr std::size_t kFieldSectionCount = 0x26;
    constexpr std::size_t kFieldSectionEntryBytes = 0x28;
    constexpr std::size_t kFieldExportCount = 0x2A;
    constexpr std::size_t kFieldExportTableOffset = 0x2C;
    constexpr std::size_t kFieldImportCount = 0x30;
    constexpr std::size_t kFieldImportEntryBytes = 0x32;
    constexpr std::size_t kFieldImportTableOffset = 0x34;
    constexpr std::size_t kFieldDependencyCount = 0x38;
    constexpr std::size_t kFieldDependencyEntryBytes = 0x3A;
    constexpr std::size_t kFieldDependencyTableOffset = 0x3C;
    constexpr std::size_t kFieldRelocationCount = 0x40;
    constexpr std::size_t kFieldRelocationTableOffset = 0x44;
    constexpr std::size_t kFieldStringTableOffset = 0x48;
    constexpr std::size_t kFieldStringTableBytes = 0x4C;
    // 段表条目内偏移
    constexpr std::size_t kSectionNameOffset = 0x00;
    constexpr std::size_t kSectionType = 0x04;
    constexpr std::size_t kSectionFlags = 0x08;
    constexpr std::size_t kSectionFileOffset = 0x0C;
    constexpr std::size_t kSectionFileBytes = 0x10;
    constexpr std::size_t kSectionMemoryOffset = 0x14;
    constexpr std::size_t kSectionMemoryBytes = 0x18;
    constexpr std::size_t kSectionAlign = 0x1C;
    // 导出条目内偏移
    constexpr std::size_t kExportVersion = 0x10;
    constexpr std::size_t kExportSlotCount = 0x12;
    constexpr std::size_t kExportSlotTableOffset = 0x14;
    constexpr std::size_t kExportReserved = 0x18;
    // 导入条目内偏移
    constexpr std::size_t kImportMinVersion = 0x10;
    constexpr std::size_t kImportMaxVersion = 0x12;
    constexpr std::size_t kImportSlotPatchOffset = 0x14;
    constexpr std::size_t kImportSlotCount = 0x18;
    constexpr std::size_t kImportReserved = 0x1A;
    // 依赖条目内偏移
    constexpr std::size_t kDependencyMinMajor = 0x10;
    constexpr std::size_t kDependencyMaxMajor = 0x16;
    constexpr std::size_t kDependencyFlags = 0x1C;
    // 重定位条目内偏移
    constexpr std::size_t kRelocationTarget = 0x04;

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

    // 小端写入 16 位：越界即中止当前用例
    void Put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
        if (offset > bytes.size() || bytes.size() - offset < 2) Fail("write beyond the container copy");
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }

    // 小端写入 32 位：越界即中止当前用例
    void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
        if (offset > bytes.size() || bytes.size() - offset < 4) Fail("write beyond the container copy");
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 小端写入 64 位：越界即中止当前用例
    void Put64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
        if (offset > bytes.size() || bytes.size() - offset < 8) Fail("write beyond the container copy");
        for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 读整个文件
    Bytes ReadFile(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open " + path);
        return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 解析失败分类的诊断名：成功显示 Success
    std::string ErrorName(Kernel::ContainerError error) {
        if (error == Kernel::ContainerError::None) return "Success";
        return Kernel::ContainerErrorText(error);
    }

    // 用解析器检查一份容器字节
    Kernel::ContainerError Parse(const Bytes& bytes) {
        Kernel::ContainerView view{};
        return Kernel::ParseModuleContainer(bytes.data(), static_cast<std::uint32_t>(bytes.size()), view);
    }

    // 期待容器被拒绝且分类匹配
    void ExpectError(const Bytes& bytes, Kernel::ContainerError expected) {
        const Kernel::ContainerError actual = Parse(bytes);
        if (actual == expected) return;
        Fail("expected '" + ErrorName(expected) + "', got '" + ErrorName(actual) + "'");
    }

    // 期待容器通过解析并交出视图
    Kernel::ContainerView ExpectSuccess(const Bytes& bytes) {
        Kernel::ContainerView view{};
        const Kernel::ContainerError actual = Kernel::ParseModuleContainer(bytes.data(), static_cast<std::uint32_t>(bytes.size()), view);
        if (actual != Kernel::ContainerError::None) Fail("expected success, got '" + ErrorName(actual) + "'");
        return view;
    }

    // 第 index 个段表条目的文件偏移
    std::size_t SectionEntry(const Bytes& bytes, unsigned index) {
        return static_cast<std::size_t>(Get16(bytes, kFieldSectionTableOffset)) + index * Kernel::kContainerSectionEntryBytes;
    }

    // 导出表第一个条目的文件偏移
    std::size_t ExportEntry(const Bytes& bytes) { return Get32(bytes, kFieldExportTableOffset); }

    // 导入表第一个条目的文件偏移
    std::size_t ImportEntry(const Bytes& bytes) { return Get32(bytes, kFieldImportTableOffset); }

    // 依赖表第一个条目的文件偏移
    std::size_t DependencyEntry(const Bytes& bytes) { return Get32(bytes, kFieldDependencyTableOffset); }

    // 重定位表第一个条目的文件偏移
    std::size_t RelocationEntry(const Bytes& bytes) { return Get32(bytes, kFieldRelocationTableOffset); }

    // —— 合成夹具：当前玩具模块的导出 / 导入 / 依赖表都是空的，负例用合成容器补齐这些表 ——

    // 合成夹具的一条导出条目
    struct FixtureExport {
        std::uint8_t id[16] = {};                 // 接口标识
        std::uint16_t version = 0;                // 接口版本
        std::vector<std::uint32_t> slots;         // 各槽偏移
    };

    // 合成夹具的一条导入条目
    struct FixtureImport {
        std::uint8_t id[16] = {};                 // 接口标识
        std::uint16_t minVersion = 0;             // 最低版本
        std::uint16_t maxVersion = 0;             // 最高版本
        std::vector<std::uint32_t> patchSlots;    // 各回填槽内存偏移
    };

    // 合成夹具的一条依赖条目
    struct FixtureDependency {
        std::uint8_t id[16] = {};                 // 被依赖模块标识
        std::uint16_t minMajor = 0;               // 最低 major
        std::uint16_t minMinor = 0;               // 最低 minor
        std::uint16_t minPatch = 0;               // 最低 patch
        std::uint16_t maxMajor = 0;               // 最高 major；0 表示不设上限
        std::uint16_t maxMinor = 0;               // 最高 minor
        std::uint16_t maxPatch = 0;               // 最高 patch
    };

    // 合成夹具的一条重定位条目
    struct FixtureRelocation {
        std::uint8_t type = 0;                    // 重定位类型
        std::uint32_t target = 0;                 // 内存偏移
        std::uint64_t addend = 0;                 // 加数
    };

    // 合成夹具的输入：四张表的内容
    struct FixtureTables {
        std::vector<FixtureExport> exports;           // 导出条目
        std::vector<FixtureImport> imports;           // 导入条目
        std::vector<FixtureDependency> dependencies;  // 依赖条目
        std::vector<FixtureRelocation> relocations;   // 重定位条目
    };

    // 生成一段标识字节：从 base 起连续 16 个值
    void FillId(std::uint8_t (&id)[16], std::uint8_t base) {
        for (std::uint32_t i = 0; i < 16; ++i) id[i] = static_cast<std::uint8_t>(base + i);
    }

    // 默认表：一条导出、一条导入、一条依赖、两条重定位
    FixtureTables DefaultTables() {
        FixtureTables tables;
        FixtureExport exportEntry;
        FillId(exportEntry.id, 0x31);
        exportEntry.version = 1;
        exportEntry.slots = { 0x1010, 0x1018 };
        tables.exports.push_back(exportEntry);
        FixtureImport importEntry;
        FillId(importEntry.id, 0x41);
        importEntry.minVersion = 1;
        importEntry.maxVersion = 1;
        importEntry.patchSlots = { 0x4000, 0x4008 };
        tables.imports.push_back(importEntry);
        FixtureDependency dependency;
        FillId(dependency.id, 0x51);
        dependency.minMajor = 1;
        tables.dependencies.push_back(dependency);
        tables.relocations.push_back({ 1, 0x3000, 0x10 });
        tables.relocations.push_back({ 2, 0x3008, 0x2000 });
        return tables;
    }

    // 把值向上取整到 align 的整数倍
    std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) { return (value + align - 1) / align * align; }

    // 构造一个规范的合成容器：四段各一页，表区按固定顺序排布
    Bytes BuildFixture(const FixtureTables& tables) {
        // 字符串表：偏移 0 空串，随后段名与模块名
        std::string strings(1, '\0');
        auto appendName = [&strings](const std::string& name) -> std::uint32_t {
            const auto offset = static_cast<std::uint32_t>(strings.size());
            strings += name;
            strings.push_back('\0');
            return offset;
        };
        const std::uint32_t textNameOffset = appendName(".text");
        const std::uint32_t rodataNameOffset = appendName(".rodata");
        const std::uint32_t dataNameOffset = appendName(".data");
        const std::uint32_t bssNameOffset = appendName(".bss");
        const std::uint32_t moduleNameOffset = appendName("FixtureModule");
        const std::uint16_t moduleNameBytes = 13;

        // 布局：头、段表、导出表与槽位表、导入表与回填表、依赖表、重定位表、字符串表、段文件数据
        std::uint64_t cursor = Kernel::kContainerHeaderBytes;
        const std::uint32_t sectionTableOffset = static_cast<std::uint32_t>(cursor);
        cursor = AlignUp(cursor, 4) + 4 * Kernel::kContainerSectionEntryBytes;
        const std::uint32_t exportTableOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = exportTableOffset + tables.exports.size() * Kernel::kContainerExportEntryBytes;
        std::vector<std::uint32_t> exportSlotOffsets;
        for (const FixtureExport& entry : tables.exports) {
            exportSlotOffsets.push_back(static_cast<std::uint32_t>(AlignUp(cursor, 4)));
            cursor = AlignUp(cursor, 4) + entry.slots.size() * 4;
        }
        const std::uint32_t importTableOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = importTableOffset + tables.imports.size() * Kernel::kContainerImportEntryBytes;
        std::vector<std::uint32_t> importPatchOffsets;
        for (const FixtureImport& entry : tables.imports) {
            importPatchOffsets.push_back(static_cast<std::uint32_t>(AlignUp(cursor, 4)));
            cursor = AlignUp(cursor, 4) + entry.patchSlots.size() * 4;
        }
        const std::uint32_t dependencyTableOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = dependencyTableOffset + tables.dependencies.size() * Kernel::kContainerDependencyEntryBytes;
        const std::uint32_t relocationTableOffset = static_cast<std::uint32_t>(AlignUp(cursor, 8));
        cursor = relocationTableOffset + tables.relocations.size() * Kernel::kContainerRelocationEntryBytes;
        const std::uint32_t stringTableOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = stringTableOffset + strings.size();
        const std::uint32_t textFileOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = AlignUp(cursor, 4) + 0x20;
        const std::uint32_t rodataFileOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = AlignUp(cursor, 4) + 8;
        const std::uint32_t dataFileOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = AlignUp(cursor, 4) + 0x40;
        const auto totalBytes = static_cast<std::uint32_t>(cursor);

        Bytes bytes(totalBytes, 0);
        // 文件头
        static constexpr std::uint8_t kMagic[8] = { 'L', 'P', 'O', 'S', 'M', 'O', 'D', 0 };
        std::memcpy(bytes.data(), kMagic, sizeof(kMagic));
        Put16(bytes, 0x08, 1);
        Put16(bytes, 0x0A, 0x80);
        Put32(bytes, 0x0C, 0);
        Put32(bytes, kFieldTotalBytes, totalBytes);
        Put32(bytes, kFieldImageBytes, 0x5000);
        Put32(bytes, kFieldEntryInit, 0x1010);
        Put32(bytes, kFieldEntryActivate, 0x1018);
        Put32(bytes, 0x20, 0);
        Put16(bytes, kFieldSectionTableOffset, static_cast<std::uint16_t>(sectionTableOffset));
        Put16(bytes, kFieldSectionCount, 4);
        Put16(bytes, kFieldSectionEntryBytes, 32);
        Put16(bytes, kFieldExportCount, static_cast<std::uint16_t>(tables.exports.size()));
        Put32(bytes, kFieldExportTableOffset, exportTableOffset);
        Put16(bytes, kFieldImportCount, static_cast<std::uint16_t>(tables.imports.size()));
        Put16(bytes, kFieldImportEntryBytes, 28);
        Put32(bytes, kFieldImportTableOffset, importTableOffset);
        Put16(bytes, kFieldDependencyCount, static_cast<std::uint16_t>(tables.dependencies.size()));
        Put16(bytes, kFieldDependencyEntryBytes, 32);
        Put32(bytes, kFieldDependencyTableOffset, dependencyTableOffset);
        Put32(bytes, kFieldRelocationCount, static_cast<std::uint32_t>(tables.relocations.size()));
        Put32(bytes, kFieldRelocationTableOffset, relocationTableOffset);
        Put32(bytes, kFieldStringTableOffset, stringTableOffset);
        Put32(bytes, kFieldStringTableBytes, static_cast<std::uint32_t>(strings.size()));
        // 模块标识：0x11 起的 16 个字节
        for (std::uint32_t i = 0; i < 16; ++i) bytes[0x50 + i] = static_cast<std::uint8_t>(0x11 + i);
        Put32(bytes, 0x60, moduleNameOffset);
        Put16(bytes, 0x64, moduleNameBytes);
        Put16(bytes, 0x66, 1);
        Put16(bytes, 0x68, 2);
        Put16(bytes, 0x6A, 3);
        Put16(bytes, 0x6C, 1);
        Put16(bytes, 0x6E, 0xFFFF);

        // 段表：TEXT / RODATA / DATA / BSS 各一页
        struct FixtureSection {
            std::uint32_t nameOffset;    // 段名偏移
            std::uint32_t type;          // 段类型
            std::uint32_t flags;         // 权限
            std::uint32_t fileOffset;    // 文件偏移
            std::uint32_t fileBytes;     // 文件内字节数
            std::uint32_t memoryOffset;  // 内存偏移
            std::uint32_t memoryBytes;   // 内存跨度
            std::uint32_t align;         // 对齐
        };
        const FixtureSection sections[4] = {
            { textNameOffset, 1, 5, textFileOffset, 0x20, 0x1000, 0x1000, 0x1000 },
            { rodataNameOffset, 2, 1, rodataFileOffset, 8, 0x2000, 0x1000, 0x1000 },
            { dataNameOffset, 3, 3, dataFileOffset, 0x40, 0x3000, 0x1000, 0x1000 },
            { bssNameOffset, 4, 3, 0, 0, 0x4000, 0x1000, 0x1000 },
        };
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::size_t entry = static_cast<std::size_t>(sectionTableOffset) + i * Kernel::kContainerSectionEntryBytes;
            Put32(bytes, entry + kSectionNameOffset, sections[i].nameOffset);
            Put32(bytes, entry + kSectionType, sections[i].type);
            Put32(bytes, entry + kSectionFlags, sections[i].flags);
            Put32(bytes, entry + kSectionFileOffset, sections[i].fileOffset);
            Put32(bytes, entry + kSectionFileBytes, sections[i].fileBytes);
            Put32(bytes, entry + kSectionMemoryOffset, sections[i].memoryOffset);
            Put32(bytes, entry + kSectionMemoryBytes, sections[i].memoryBytes);
            Put32(bytes, entry + kSectionAlign, sections[i].align);
        }
        // 导出表与槽位表
        for (std::size_t i = 0; i < tables.exports.size(); ++i) {
            const std::size_t entry = static_cast<std::size_t>(exportTableOffset) + i * Kernel::kContainerExportEntryBytes;
            std::memcpy(bytes.data() + entry, tables.exports[i].id, 16);
            Put16(bytes, entry + kExportVersion, tables.exports[i].version);
            Put16(bytes, entry + kExportSlotCount, static_cast<std::uint16_t>(tables.exports[i].slots.size()));
            Put32(bytes, entry + kExportSlotTableOffset, exportSlotOffsets[i]);
            Put32(bytes, entry + kExportReserved, 0);
            for (std::size_t slot = 0; slot < tables.exports[i].slots.size(); ++slot) {
                Put32(bytes, static_cast<std::size_t>(exportSlotOffsets[i]) + slot * 4, tables.exports[i].slots[slot]);
            }
        }
        // 导入表与回填表
        for (std::size_t i = 0; i < tables.imports.size(); ++i) {
            const std::size_t entry = static_cast<std::size_t>(importTableOffset) + i * Kernel::kContainerImportEntryBytes;
            std::memcpy(bytes.data() + entry, tables.imports[i].id, 16);
            Put16(bytes, entry + kImportMinVersion, tables.imports[i].minVersion);
            Put16(bytes, entry + kImportMaxVersion, tables.imports[i].maxVersion);
            Put32(bytes, entry + kImportSlotPatchOffset, importPatchOffsets[i]);
            Put16(bytes, entry + kImportSlotCount, static_cast<std::uint16_t>(tables.imports[i].patchSlots.size()));
            Put16(bytes, entry + kImportReserved, 0);
            for (std::size_t slot = 0; slot < tables.imports[i].patchSlots.size(); ++slot) {
                Put32(bytes, static_cast<std::size_t>(importPatchOffsets[i]) + slot * 4, tables.imports[i].patchSlots[slot]);
            }
        }
        // 依赖表
        for (std::size_t i = 0; i < tables.dependencies.size(); ++i) {
            const std::size_t entry = static_cast<std::size_t>(dependencyTableOffset) + i * Kernel::kContainerDependencyEntryBytes;
            std::memcpy(bytes.data() + entry, tables.dependencies[i].id, 16);
            Put16(bytes, entry + 0x10, tables.dependencies[i].minMajor);
            Put16(bytes, entry + 0x12, tables.dependencies[i].minMinor);
            Put16(bytes, entry + 0x14, tables.dependencies[i].minPatch);
            Put16(bytes, entry + kDependencyMaxMajor, tables.dependencies[i].maxMajor);
            Put16(bytes, entry + 0x18, tables.dependencies[i].maxMinor);
            Put16(bytes, entry + 0x1A, tables.dependencies[i].maxPatch);
            Put32(bytes, entry + kDependencyFlags, 0);
        }
        // 重定位表
        for (std::size_t i = 0; i < tables.relocations.size(); ++i) {
            const std::size_t entry = static_cast<std::size_t>(relocationTableOffset) + i * Kernel::kContainerRelocationEntryBytes;
            bytes[entry] = tables.relocations[i].type;
            Put32(bytes, entry + kRelocationTarget, tables.relocations[i].target);
            Put64(bytes, entry + 0x08, tables.relocations[i].addend);
        }
        // 字符串表与段文件数据
        std::memcpy(bytes.data() + stringTableOffset, strings.data(), strings.size());
        std::memset(bytes.data() + textFileOffset, 0x90, 0x20);
        std::memset(bytes.data() + rodataFileOffset, 0x91, 8);
        std::memset(bytes.data() + dataFileOffset, 0x92, 0x40);
        return bytes;
    }

    // 真实模块容器文件路径（按文件名排序）
    std::vector<std::string> g_moduleFiles;
    // 第一个真实容器的字节：头部 / 段 / 入口 / 字符串 / 重定位负例的基底
    Bytes g_real;
    // 合成容器的字节：导出 / 导入 / 依赖负例的基底
    Bytes g_synth;

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // ---- 正例 ----
        cases.push_back({ "all real module containers parse", [] {
            Require(!g_moduleFiles.empty(), "no real module container is available");
            for (const std::string& path : g_moduleFiles) {
                const Kernel::ContainerView view = ExpectSuccess(ReadFile(path));
                Require(!IsZero(view.moduleId), "real container ModuleId is zero");
                // 不写死尺寸：只断言结构性事实（页对齐、段落在映像内、入口落在 TEXT）
                Require(view.imageBytes > 0 && (view.imageBytes % Kernel::kContainerPageBytes) == 0,
                    "real container ImageBytes isn't a nonzero page multiple");
                Require(view.sectionCount >= 1 && view.sectionCount <= Kernel::kContainerMaxSections,
                    "real container section count is out of range");
                Require(view.versionMajor == 1 && view.abiMin == 1, "real container version/ABI is wrong");
                Require(view.moduleName != nullptr && view.moduleNameBytes > 0, "real container has no module name");
                Require(view.entryInit != 0, "real container EntryInit is zero");
                bool hasText = false;
                bool entryInText = false;
                for (std::uint32_t index = 0; index < view.sectionCount; ++index) {
                    const Kernel::ContainerSection& section = view.sections[index];
                    Require(section.type >= 1 && section.type <= 4, "real container section type is invalid");
                    Require((section.memoryOffset % Kernel::kContainerPageBytes) == 0,
                        "real container section memory offset isn't page aligned");
                    Require((section.memoryBytes % Kernel::kContainerPageBytes) == 0 && section.memoryBytes > 0,
                        "real container section memory span isn't a page multiple");
                    Require(static_cast<std::uint64_t>(section.memoryOffset) + section.memoryBytes <= view.imageBytes,
                        "real container section memory range exceeds ImageBytes");
                    if (section.type != 1) continue;
                    hasText = true;
                    if (view.entryInit >= section.memoryOffset
                        && static_cast<std::uint64_t>(view.entryInit) - section.memoryOffset < section.memoryBytes) {
                        entryInText = true;
                    }
                }
                Require(hasText, "real container has no TEXT section");
                Require(entryInText, "real container EntryInit is outside TEXT");
                if (view.relocationCount == 0) continue;
                Kernel::ContainerRelocation relocation{};
                Require(Kernel::ReadContainerRelocation(view, 0, relocation) == Kernel::ContainerError::None,
                    "cannot read the first real relocation");
                Require(relocation.type == 1 || relocation.type == 2, "real relocation type is unknown");
            }
        } });
        cases.push_back({ "synthetic container parses", [] {
            const Kernel::ContainerView view = ExpectSuccess(g_synth);
            Require(view.sectionCount == 4 && view.exportCount == 1 && view.importCount == 1 && view.dependencyCount == 1, "synthetic table counts are wrong");
            Require(view.relocationCount == 2 && view.relocationTableOffset != 0, "synthetic relocation count is wrong");
            Require(view.imageBytes == 0x5000 && view.entryInit == 0x1010 && view.entryActivate == 0x1018, "synthetic header fields are wrong");
            Require(view.versionMajor == 1 && view.versionMinor == 2 && view.versionPatch == 3, "synthetic module version is wrong");
            Require(view.abiMin == 1 && view.abiMax == 0xFFFF, "synthetic ABI range is wrong");
            Require(view.moduleNameBytes == 13 && std::memcmp(view.moduleName, "FixtureModule", 13) == 0, "synthetic module name is wrong");
            Require(view.exports[0].version == 1 && view.exports[0].slotCount == 2, "synthetic export entry is wrong");
            Require(view.imports[0].minVersion == 1 && view.imports[0].maxVersion == 1 && view.imports[0].slotCount == 2, "synthetic import entry is wrong");
            Require(view.dependencies[0].minMajor == 1 && view.dependencies[0].maxMajor == 0, "synthetic dependency entry is wrong");
        } });
        cases.push_back({ "relocation iterator reads entries", [] {
            const Kernel::ContainerView view = ExpectSuccess(g_synth);
            Kernel::ContainerRelocation first{};
            Kernel::ContainerRelocation second{};
            Require(Kernel::ReadContainerRelocation(view, 0, first) == Kernel::ContainerError::None, "cannot read relocation 0");
            Require(Kernel::ReadContainerRelocation(view, 1, second) == Kernel::ContainerError::None, "cannot read relocation 1");
            Require(first.type == 1 && first.target == 0x3000 && first.addend == 0x10, "relocation 0 is wrong");
            Require(second.type == 2 && second.target == 0x3008 && second.addend == 0x2000, "relocation 1 is wrong");
            Kernel::ContainerRelocation outOfRange{};
            Require(Kernel::ReadContainerRelocation(view, 2, outOfRange) == Kernel::ContainerError::RelocationEntry, "out-of-range relocation index was accepted");
        } });
        cases.push_back({ "slot accessors read entries", [] {
            const Kernel::ContainerView view = ExpectSuccess(g_synth);
            std::uint32_t value = 0;
            Require(Kernel::ContainerExportSlot(view.exports[0], 1, value) && value == 0x1018, "export slot accessor is wrong");
            Require(!Kernel::ContainerExportSlot(view.exports[0], 2, value), "out-of-range export slot was accepted");
            Require(Kernel::ContainerImportSlot(view.imports[0], 0, value) && value == 0x4000, "import slot accessor is wrong");
            Require(!Kernel::ContainerImportSlot(view.imports[0], 2, value), "out-of-range import slot was accepted");
        } });
        cases.push_back({ "container without optional tables parses", [] {
            FixtureTables tables = DefaultTables();
            tables.exports.clear();
            tables.imports.clear();
            tables.dependencies.clear();
            const Kernel::ContainerView view = ExpectSuccess(BuildFixture(tables));
            Require(view.exportCount == 0 && view.importCount == 0 && view.dependencyCount == 0, "optional tables aren't empty");
            Require(view.relocationCount == 2, "relocations were lost");
        } });
        // ---- 文件头负例 ----
        cases.push_back({ "null pointer is rejected", [] {
            Kernel::ContainerView view{};
            Require(Kernel::ParseModuleContainer(nullptr, 0, view) == Kernel::ContainerError::NullPointer, "null pointer wasn't rejected");
        } });
        cases.push_back({ "short container is rejected", [] {
            Bytes bytes = g_real;
            bytes.resize(0x7F);
            ExpectError(bytes, Kernel::ContainerError::TooShort);
        } });
        cases.push_back({ "magic mismatch is rejected", [] {
            Bytes bytes = g_real;
            bytes[0] = 0;
            ExpectError(bytes, Kernel::ContainerError::HeaderMagic);
        } });
        cases.push_back({ "unknown version is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, 0x08, 2);
            ExpectError(bytes, Kernel::ContainerError::HeaderVersion);
        } });
        cases.push_back({ "HeaderBytes mismatch is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, 0x0A, 0x7F);
            ExpectError(bytes, Kernel::ContainerError::HeaderBytes);
        } });
        cases.push_back({ "nonzero header Flags is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, 0x0C, 1);
            ExpectError(bytes, Kernel::ContainerError::HeaderFlags);
        } });
        cases.push_back({ "TotalBytes mismatch is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldTotalBytes, static_cast<std::uint32_t>(bytes.size()) + 4);
            ExpectError(bytes, Kernel::ContainerError::HeaderTotalBytes);
        } });
        cases.push_back({ "TotalBytes above the limit is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldTotalBytes, 0x4000001);
            ExpectError(bytes, Kernel::ContainerError::HeaderTotalBytes);
        } });
        cases.push_back({ "ImageBytes not a page multiple is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldImageBytes, 0x1234);
            ExpectError(bytes, Kernel::ContainerError::HeaderImageBytes);
        } });
        cases.push_back({ "nonzero header Reserved is rejected", [] {
            Bytes bytes = g_real;
            bytes[0x70] = 1;
            ExpectError(bytes, Kernel::ContainerError::HeaderReserved);
        } });
        cases.push_back({ "zero section count is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldSectionCount, 0);
            ExpectError(bytes, Kernel::ContainerError::SectionCount);
        } });
        cases.push_back({ "section count above the limit is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldSectionCount, 17);
            ExpectError(bytes, Kernel::ContainerError::SectionCount);
        } });
        cases.push_back({ "SectionEntryBytes mismatch is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldSectionEntryBytes, 31);
            ExpectError(bytes, Kernel::ContainerError::SectionEntryBytes);
        } });
        cases.push_back({ "export count above the limit is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldExportCount, 129);
            ExpectError(bytes, Kernel::ContainerError::ExportCount);
        } });
        cases.push_back({ "ImportEntryBytes mismatch is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldImportEntryBytes, 27);
            ExpectError(bytes, Kernel::ContainerError::ImportEntryBytes);
        } });
        cases.push_back({ "DependencyEntryBytes mismatch is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldDependencyEntryBytes, 31);
            ExpectError(bytes, Kernel::ContainerError::DependencyEntryBytes);
        } });
        cases.push_back({ "relocation count above the limit is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldRelocationCount, 0x10001);
            ExpectError(bytes, Kernel::ContainerError::RelocationCount);
        } });
        cases.push_back({ "section table offset misalignment is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldSectionTableOffset, static_cast<std::uint16_t>(Get16(bytes, kFieldSectionTableOffset) + 1));
            ExpectError(bytes, Kernel::ContainerError::SectionTableOffset);
        } });
        cases.push_back({ "section table offset out of range is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, kFieldSectionTableOffset, 0xFFFF);
            ExpectError(bytes, Kernel::ContainerError::SectionTableOffset);
        } });
        cases.push_back({ "export table offset out of range is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldExportTableOffset, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::ExportTableOffset);
        } });
        cases.push_back({ "export table offset misalignment is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldExportTableOffset, Get32(bytes, kFieldExportTableOffset) + 2);
            ExpectError(bytes, Kernel::ContainerError::ExportTableOffset);
        } });
        cases.push_back({ "import table offset out of range is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldImportTableOffset, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::ImportTableOffset);
        } });
        cases.push_back({ "dependency table offset out of range is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldDependencyTableOffset, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::DependencyTableOffset);
        } });
        cases.push_back({ "relocation table offset misalignment is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldRelocationTableOffset, Get32(bytes, kFieldRelocationTableOffset) + 4);
            ExpectError(bytes, Kernel::ContainerError::RelocationTableOffset);
        } });
        cases.push_back({ "relocation table offset out of range is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldRelocationTableOffset, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::RelocationTableOffset);
        } });
        cases.push_back({ "string table offset out of range is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldStringTableOffset, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::StringTableOffset);
        } });
        cases.push_back({ "string table bounds is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldStringTableBytes, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::StringTableBounds);
        } });
        cases.push_back({ "string table without NUL is rejected", [] {
            Bytes bytes = g_real;
            const std::size_t offset = Get32(bytes, kFieldStringTableOffset);
            const std::size_t count = Get32(bytes, kFieldStringTableBytes);
            bytes[offset + count - 1] = 'X';
            ExpectError(bytes, Kernel::ContainerError::StringTableNul);
        } });
        // ---- 模块身份与模块名 ----
        cases.push_back({ "zero ModuleId is rejected", [] {
            Bytes bytes = g_real;
            for (std::size_t i = 0; i < 16; ++i) bytes[0x50 + i] = 0;
            ExpectError(bytes, Kernel::ContainerError::ModuleId);
        } });
        cases.push_back({ "zero module major is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, 0x66, 0);
            ExpectError(bytes, Kernel::ContainerError::ModuleVersion);
        } });
        cases.push_back({ "zero kernel ABI minimum is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, 0x6C, 0);
            ExpectError(bytes, Kernel::ContainerError::KernelAbi);
        } });
        cases.push_back({ "module name inconsistency is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, 0x64, 0);
            ExpectError(bytes, Kernel::ContainerError::ModuleName);
        } });
        cases.push_back({ "module name outside the string table is rejected", [] {
            Bytes bytes = g_real;
            Put16(bytes, 0x64, 0xFFFF);
            ExpectError(bytes, Kernel::ContainerError::ModuleName);
        } });
        cases.push_back({ "section name offset outside the string table is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 0) + kSectionNameOffset, 0xFFFF);
            ExpectError(bytes, Kernel::ContainerError::SectionName);
        } });
        // ---- 段表负例 ----
        cases.push_back({ "unknown section type is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 0) + kSectionType, 5);
            ExpectError(bytes, Kernel::ContainerError::SectionType);
        } });
        cases.push_back({ "section W+X flags is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 0) + kSectionFlags, 7);
            ExpectError(bytes, Kernel::ContainerError::SectionFlags);
        } });
        cases.push_back({ "section type/flags mismatch is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 1) + kSectionFlags, 3);
            ExpectError(bytes, Kernel::ContainerError::SectionFlags);
        } });
        cases.push_back({ "section align is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 0) + kSectionAlign, 3);
            ExpectError(bytes, Kernel::ContainerError::SectionAlign);
        } });
        cases.push_back({ "section memory offset misalignment is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 0) + kSectionMemoryOffset, 0x1800);
            ExpectError(bytes, Kernel::ContainerError::SectionMemoryOffset);
        } });
        cases.push_back({ "section memory span misalignment is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 1) + kSectionMemoryBytes, 0x800);
            ExpectError(bytes, Kernel::ContainerError::SectionMemoryBytes);
        } });
        cases.push_back({ "section memory range above ImageBytes is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 3) + kSectionMemoryBytes, 0x2000);
            ExpectError(bytes, Kernel::ContainerError::SectionMemoryRange);
        } });
        cases.push_back({ "section memory overlap is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 1) + kSectionMemoryOffset, 0x1000);
            ExpectError(bytes, Kernel::ContainerError::SectionMemoryOverlap);
        } });
        cases.push_back({ "BSS with file bytes is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 3) + kSectionFileBytes, 8);
            ExpectError(bytes, Kernel::ContainerError::SectionFileBytes);
        } });
        cases.push_back({ "non-BSS without file bytes is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 1) + kSectionFileBytes, 0);
            ExpectError(bytes, Kernel::ContainerError::SectionFileBytes);
        } });
        cases.push_back({ "FileBytes above MemoryBytes is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 2) + kSectionFileBytes, 0x2000);
            ExpectError(bytes, Kernel::ContainerError::SectionFileBytes);
        } });
        cases.push_back({ "section file offset misalignment is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 0) + kSectionFileOffset, Get32(bytes, SectionEntry(bytes, 0) + kSectionFileOffset) + 1);
            ExpectError(bytes, Kernel::ContainerError::SectionFileOffset);
        } });
        cases.push_back({ "section file range out of bounds is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, SectionEntry(bytes, 0) + kSectionFileOffset, static_cast<std::uint32_t>(bytes.size()) - 4);
            ExpectError(bytes, Kernel::ContainerError::SectionFileRange);
        } });
        // ---- 入口 ----
        cases.push_back({ "zero EntryInit is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, kFieldEntryInit, 0);
            ExpectError(bytes, Kernel::ContainerError::EntryInit);
        } });
        cases.push_back({ "EntryInit outside TEXT is rejected", [] {
            // 从实际容器推导映像末尾：一定落在所有段之外，不写死旧容器的偏移
            const Kernel::ContainerView view = ExpectSuccess(g_real);
            Bytes bytes = g_real;
            Put32(bytes, kFieldEntryInit, view.imageBytes);
            ExpectError(bytes, Kernel::ContainerError::EntryInit);
        } });
        cases.push_back({ "EntryActivate outside TEXT is rejected", [] {
            const Kernel::ContainerView view = ExpectSuccess(g_real);
            Bytes bytes = g_real;
            Put32(bytes, kFieldEntryActivate, view.imageBytes);
            ExpectError(bytes, Kernel::ContainerError::EntryActivate);
        } });
        // ---- 表区重叠（合成容器，真实模块的可选表为空） ----
        cases.push_back({ "overlapping table regions are rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, kFieldDependencyTableOffset, Get32(bytes, kFieldRelocationTableOffset));
            ExpectError(bytes, Kernel::ContainerError::RegionOverlap);
        } });
        cases.push_back({ "export slot table overlapping the import table is rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, ExportEntry(bytes) + kExportSlotTableOffset, Get32(bytes, kFieldImportTableOffset));
            ExpectError(bytes, Kernel::ContainerError::RegionOverlap);
        } });
        // ---- 导出表负例（合成容器） ----
        cases.push_back({ "zero export version is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ExportEntry(bytes) + kExportVersion, 0);
            ExpectError(bytes, Kernel::ContainerError::ExportEntry);
        } });
        cases.push_back({ "zero export identity is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t entry = ExportEntry(bytes);
            for (std::size_t i = 0; i < 16; ++i) bytes[entry + i] = 0;
            ExpectError(bytes, Kernel::ContainerError::ExportEntry);
        } });
        cases.push_back({ "nonzero export reserved field is rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, ExportEntry(bytes) + kExportReserved, 1);
            ExpectError(bytes, Kernel::ContainerError::ExportEntry);
        } });
        cases.push_back({ "zero export slot count is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ExportEntry(bytes) + kExportSlotCount, 0);
            ExpectError(bytes, Kernel::ContainerError::ExportSlotCount);
        } });
        cases.push_back({ "export slot count above the limit is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ExportEntry(bytes) + kExportSlotCount, 65);
            ExpectError(bytes, Kernel::ContainerError::ExportSlotCount);
        } });
        cases.push_back({ "export slot table offset misalignment is rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, ExportEntry(bytes) + kExportSlotTableOffset, Get32(bytes, ExportEntry(bytes) + kExportSlotTableOffset) + 2);
            ExpectError(bytes, Kernel::ContainerError::ExportSlotTableOffset);
        } });
        cases.push_back({ "export slot table offset out of range is rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, ExportEntry(bytes) + kExportSlotTableOffset, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::ExportSlotTableOffset);
        } });
        cases.push_back({ "export slot outside TEXT is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t slotTable = Get32(bytes, ExportEntry(bytes) + kExportSlotTableOffset);
            Put32(bytes, slotTable, 0x3000);
            ExpectError(bytes, Kernel::ContainerError::ExportSlot);
        } });
        cases.push_back({ "duplicate export is rejected", [] {
            FixtureTables tables = DefaultTables();
            tables.exports.push_back(tables.exports[0]);
            ExpectError(BuildFixture(tables), Kernel::ContainerError::ExportDuplicate);
        } });
        // ---- 导入表负例（合成容器） ----
        cases.push_back({ "zero import identity is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t entry = ImportEntry(bytes);
            for (std::size_t i = 0; i < 16; ++i) bytes[entry + i] = 0;
            ExpectError(bytes, Kernel::ContainerError::ImportEntry);
        } });
        cases.push_back({ "zero import min version is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ImportEntry(bytes) + kImportMinVersion, 0);
            ExpectError(bytes, Kernel::ContainerError::ImportEntry);
        } });
        cases.push_back({ "reversed import version range is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ImportEntry(bytes) + kImportMinVersion, 2);
            Put16(bytes, ImportEntry(bytes) + kImportMaxVersion, 1);
            ExpectError(bytes, Kernel::ContainerError::ImportEntry);
        } });
        cases.push_back({ "nonzero import reserved field is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ImportEntry(bytes) + kImportReserved, 1);
            ExpectError(bytes, Kernel::ContainerError::ImportEntry);
        } });
        cases.push_back({ "zero import slot count is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ImportEntry(bytes) + kImportSlotCount, 0);
            ExpectError(bytes, Kernel::ContainerError::ImportSlotCount);
        } });
        cases.push_back({ "import slot count above the limit is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, ImportEntry(bytes) + kImportSlotCount, 65);
            ExpectError(bytes, Kernel::ContainerError::ImportSlotCount);
        } });
        cases.push_back({ "import patch table offset misalignment is rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, ImportEntry(bytes) + kImportSlotPatchOffset, Get32(bytes, ImportEntry(bytes) + kImportSlotPatchOffset) + 2);
            ExpectError(bytes, Kernel::ContainerError::ImportSlotTableOffset);
        } });
        cases.push_back({ "import patch table offset out of range is rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, ImportEntry(bytes) + kImportSlotPatchOffset, static_cast<std::uint32_t>(bytes.size()));
            ExpectError(bytes, Kernel::ContainerError::ImportSlotTableOffset);
        } });
        cases.push_back({ "import patch slot outside writable sections is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t patchTable = Get32(bytes, ImportEntry(bytes) + kImportSlotPatchOffset);
            Put32(bytes, patchTable, 0x1000);
            ExpectError(bytes, Kernel::ContainerError::ImportSlot);
        } });
        cases.push_back({ "import patch slot misalignment is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t patchTable = Get32(bytes, ImportEntry(bytes) + kImportSlotPatchOffset);
            Put32(bytes, patchTable, 0x4004);
            ExpectError(bytes, Kernel::ContainerError::ImportSlot);
        } });
        cases.push_back({ "import patch slot out of bounds is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t patchTable = Get32(bytes, ImportEntry(bytes) + kImportSlotPatchOffset);
            Put32(bytes, patchTable, 0x5000);
            ExpectError(bytes, Kernel::ContainerError::ImportSlot);
        } });
        cases.push_back({ "duplicate import is rejected", [] {
            FixtureTables tables = DefaultTables();
            tables.imports.push_back(tables.imports[0]);
            ExpectError(BuildFixture(tables), Kernel::ContainerError::ImportDuplicate);
        } });
        // ---- 依赖表负例（合成容器） ----
        cases.push_back({ "zero dependency identity is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t entry = DependencyEntry(bytes);
            for (std::size_t i = 0; i < 16; ++i) bytes[entry + i] = 0;
            ExpectError(bytes, Kernel::ContainerError::DependencyEntry);
        } });
        cases.push_back({ "self dependency is rejected", [] {
            Bytes bytes = g_synth;
            const std::size_t entry = DependencyEntry(bytes);
            for (std::size_t i = 0; i < 16; ++i) bytes[entry + i] = bytes[0x50 + i];
            ExpectError(bytes, Kernel::ContainerError::DependencyEntry);
        } });
        cases.push_back({ "zero dependency min major is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, DependencyEntry(bytes) + kDependencyMinMajor, 0);
            ExpectError(bytes, Kernel::ContainerError::DependencyEntry);
        } });
        cases.push_back({ "nonzero dependency flags is rejected", [] {
            Bytes bytes = g_synth;
            Put32(bytes, DependencyEntry(bytes) + kDependencyFlags, 1);
            ExpectError(bytes, Kernel::ContainerError::DependencyEntry);
        } });
        cases.push_back({ "reversed dependency version range is rejected", [] {
            Bytes bytes = g_synth;
            Put16(bytes, DependencyEntry(bytes) + kDependencyMinMajor, 2);
            Put16(bytes, DependencyEntry(bytes) + kDependencyMaxMajor, 1);
            ExpectError(bytes, Kernel::ContainerError::DependencyEntry);
        } });
        cases.push_back({ "duplicate dependency is rejected", [] {
            FixtureTables tables = DefaultTables();
            tables.dependencies.push_back(tables.dependencies[0]);
            ExpectError(BuildFixture(tables), Kernel::ContainerError::DependencyDuplicate);
        } });
        // ---- 重定位表负例（真实容器） ----
        cases.push_back({ "unknown relocation type is rejected", [] {
            Bytes bytes = g_real;
            bytes[RelocationEntry(bytes)] = 3;
            ExpectError(bytes, Kernel::ContainerError::RelocationEntry);
        } });
        cases.push_back({ "nonzero relocation reserved byte is rejected", [] {
            Bytes bytes = g_real;
            bytes[RelocationEntry(bytes) + 1] = 1;
            ExpectError(bytes, Kernel::ContainerError::RelocationEntry);
        } });
        cases.push_back({ "relocation target in TEXT is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, RelocationEntry(bytes) + kRelocationTarget, 0x1000);
            ExpectError(bytes, Kernel::ContainerError::RelocationEntry);
        } });
        cases.push_back({ "misaligned relocation target is rejected", [] {
            Bytes bytes = g_real;
            Put32(bytes, RelocationEntry(bytes) + kRelocationTarget, 0x3004);
            ExpectError(bytes, Kernel::ContainerError::RelocationEntry);
        } });
        cases.push_back({ "relocation target out of bounds is rejected", [] {
            // 目标取实际 ImageBytes：页对齐、且一定落在所有段之外，不写死旧容器的偏移
            const Kernel::ContainerView view = ExpectSuccess(g_real);
            Require(view.relocationCount > 0, "the base container has no relocation to abuse");
            Bytes bytes = g_real;
            Put32(bytes, RelocationEntry(bytes) + kRelocationTarget, view.imageBytes);
            ExpectError(bytes, Kernel::ContainerError::RelocationEntry);
        } });
        return cases;
    }
}

// 入口：加载真实模块产物，跑全部用例并汇总；产物缺失返回 2
int main(int argc, char** argv) {
    std::string modulesDir = kDefaultModulesDir;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            std::cout <<
                "CheckContainer - host-side check of the kernel module container parser\n"
                "\n"
                "Usage: CheckContainer [--modules <dir>]\n"
                "  --modules  directory with the real .osm module products\n"
                "             (default: Packages/KernelModules/Out/Modules)\n"
                "Exit codes: 0 all cases passed, 1 a case failed, 2 the real module products are missing\n";
            return 0;
        } else if (argument == "--modules") {
            if (i + 1 >= argc) {
                std::cerr << "CheckContainer: --modules expects a value\n";
                return 2;
            }
            modulesDir = argv[++i];
        } else {
            std::cerr << "CheckContainer: unknown option " << argument << "\n";
            return 2;
        }
    }
    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(modulesDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".osm") continue;
            g_moduleFiles.push_back(entry.path().string());
        }
        std::sort(g_moduleFiles.begin(), g_moduleFiles.end());
        if (g_moduleFiles.empty()) throw std::runtime_error("no .osm module product under " + modulesDir);
        // 改坏基底优先取带重定位的真实容器：部分用例要从实际重定位表取条目
        g_real = ReadFile(g_moduleFiles.front());
        for (const std::string& path : g_moduleFiles) {
            const Bytes candidate = ReadFile(path);
            Kernel::ContainerView view{};
            if (Kernel::ParseModuleContainer(candidate.data(), static_cast<std::uint32_t>(candidate.size()), view)
                != Kernel::ContainerError::None) {
                continue;
            }
            if (view.relocationCount == 0) continue;
            g_real = std::move(candidate);
            break;
        }
        g_synth = BuildFixture(DefaultTables());
    } catch (const std::exception& error) {
        std::cerr << "CheckContainer: infrastructure missing: " << error.what() << "\n";
        return 2;
    }
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckContainer: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckContainer: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckContainer: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
