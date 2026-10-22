/* Osm.cpp
    内核模块容器（.osm）的写出器与解析校验器：两者共用本文件的布局定义与字段编码

    写出器只做规范化布局：表顺序固定（头、段表、导出表与槽表、导入表与回填表、
    依赖表、重定位表、字符串表、段文件数据），不写时间戳、路径与用户名，保留字段写零
    解析器独立复算全部容器约束，任何不符抛出英文原因的 std::runtime_error；
    PackModule --verify 与 CheckOsm 都走这里
*/

#include <Osm.hpp>

#include <Sha256.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
    namespace L = osm::Layout;
    // 只读字节视图
    using ByteView = std::span<const std::uint8_t>;

    // 小端读取 8 位
    std::uint8_t Get8(ByteView bytes, std::uint64_t offset) { return bytes[static_cast<std::size_t>(offset)]; }

    // 小端读取 16 位
    std::uint16_t Get16(ByteView bytes, std::uint64_t offset) {
        return static_cast<std::uint16_t>(Get8(bytes, offset) | (static_cast<std::uint16_t>(Get8(bytes, offset + 1)) << 8));
    }

    // 小端读取 32 位
    std::uint32_t Get32(ByteView bytes, std::uint64_t offset) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(Get8(bytes, offset + i)) << (8 * i);
        return value;
    }

    // 小端读取 64 位
    std::uint64_t Get64(ByteView bytes, std::uint64_t offset) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(Get8(bytes, offset + i)) << (8 * i);
        return value;
    }

    // 小端写入 16 位
    void Put16(osm::Bytes& bytes, std::uint64_t offset, std::uint16_t value) {
        for (unsigned i = 0; i < 2; ++i) bytes[static_cast<std::size_t>(offset + i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 小端写入 32 位
    void Put32(osm::Bytes& bytes, std::uint64_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[static_cast<std::size_t>(offset + i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 小端写入 64 位
    void Put64(osm::Bytes& bytes, std::uint64_t offset, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes[static_cast<std::size_t>(offset + i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    // 把值向上取整到 align 的整数倍
    std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) { return (value + align - 1) / align * align; }

    // 一段带名字的文件区间，用于表区与段文件数据的重叠检查
    struct Region {
        std::string what;    // 诊断用的区间名字
        std::uint64_t begin; // 起始字节偏移
        std::uint64_t end;   // 结束字节偏移（不含）
    };

    // 检查一组文件区间两两不重叠，重叠时抛出带名字的异常
    void CheckRegions(std::vector<Region> regions) {
        std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.begin < b.begin; });
        for (std::size_t i = 0; i + 1 < regions.size(); ++i) {
            if (regions[i].end > regions[i + 1].begin) {
                throw std::runtime_error("file regions overlap: " + regions[i].what + " and " + regions[i + 1].what);
            }
        }
    }

    // 检查一段文件区间落在 [0x80, totalBytes) 内
    void CheckFileRange(const std::string& what, std::uint64_t offset, std::uint64_t bytes, std::uint64_t totalBytes) {
        if (offset < L::kMinTableOffset || offset >= totalBytes) throw std::runtime_error(what + " offset is out of range");
        if (bytes > 0 && offset + bytes > totalBytes) throw std::runtime_error(what + " is out of range");
    }

    // 判断一段字节是否全零
    bool IsZero(std::span<const std::uint8_t> bytes) {
        for (const std::uint8_t byte : bytes) if (byte != 0) return false;
        return true;
    }

    // 判断一个字节是否 ASCII 大写字母
    bool IsAsciiUpper(std::uint8_t byte) { return byte >= 'A' && byte <= 'Z'; }

    // 判断一个字节是否 ASCII 字母或数字
    bool IsAsciiAlnum(std::uint8_t byte) { return IsAsciiUpper(byte) || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9'); }

    // 判断稳定名的一个分量：首字符大写，其余字母或数字，非空
    bool IsValidNameComponent(std::string_view component) {
        if (component.empty()) return false;
        if (!IsAsciiUpper(static_cast<std::uint8_t>(component[0]))) return false;
        for (std::size_t i = 1; i < component.size(); ++i) {
            if (!IsAsciiAlnum(static_cast<std::uint8_t>(component[i]))) return false;
        }
        return true;
    }

    // 名字长度上限：非空且不超过 64 字节
    bool IsValidNameLength(std::string_view name) { return !name.empty() && name.size() <= 64; }

    // 按域标记派生标识的前 16 字节
    osm::IdBytes DeriveId(std::string_view domain, std::string_view name) {
        hostbuild::Sha256 sha;
        sha.Begin();
        sha.Update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(domain.data()), domain.size()));
        sha.Update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
        const std::array<std::uint8_t, hostbuild::kSha256Bytes> digest = sha.Finish();
        osm::IdBytes id{};
        std::copy(digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(id.size()), id.begin());
        return id;
    }

    // 深度优先遍历依赖图的一个节点，遇到环抛出异常
    void VisitDependency(std::size_t index, const std::vector<std::vector<std::size_t>>& edges,
        std::span<const osm::DependencyNode> nodes, std::vector<std::uint8_t>& colors) {
        if (colors[index] == 1) throw std::runtime_error("dependency graph contains a cycle at module '" + nodes[index].name + "'");
        if (colors[index] == 2) return;
        colors[index] = 1;
        for (const std::size_t next : edges[index]) VisitDependency(next, edges, nodes, colors);
        colors[index] = 2;
    }
}

namespace osm {
    namespace L = Layout;

    bool IsValidModuleName(std::string_view name) noexcept {
        if (!IsValidNameLength(name)) return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto byte = static_cast<std::uint8_t>(name[i]);
            if (i == 0 ? !IsAsciiUpper(byte) : !IsAsciiAlnum(byte)) return false;
        }
        return true;
    }

    bool IsValidInterfaceName(std::string_view name) noexcept {
        if (!IsValidNameLength(name)) return false;
        // 至少两个分量，按 . 分隔，每个分量首字符大写
        std::size_t components = 0;
        std::size_t begin = 0;
        while (true) {
            const std::size_t end = name.find('.', begin);
            const std::string_view component = name.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
            if (!IsValidNameComponent(component)) return false;
            ++components;
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        return components >= 2;
    }

    InterfaceId DeriveInterfaceId(std::string_view stableName) {
        if (!IsValidInterfaceName(stableName)) throw std::runtime_error("invalid interface stable name: " + std::string(stableName));
        return DeriveId("LPIF01", stableName);
    }

    ModuleId DeriveModuleId(std::string_view logicalName) {
        if (!IsValidModuleName(logicalName)) throw std::runtime_error("invalid module logical name: " + std::string(logicalName));
        return DeriveId("LPMOD01", logicalName);
    }

    std::string HexText(std::span<const std::uint8_t> bytes) {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const std::uint8_t byte : bytes) {
            text.push_back(kDigits[byte >> 4]);
            text.push_back(kDigits[byte & 0x0F]);
        }
        return text;
    }

    Bytes Build(const Spec& spec) {
        // 段表书写顺序必须与 MemoryOffset 升序一致
        std::vector<const SectionSpec*> sections;
        sections.reserve(spec.sections.size());
        for (const SectionSpec& section : spec.sections) sections.push_back(&section);
        std::stable_sort(sections.begin(), sections.end(),
            [](const SectionSpec* a, const SectionSpec* b) { return a->memoryOffset < b->memoryOffset; });

        // 字符串表：偏移 0 是空串（让“0 表示没有”与真实名字不冲突），随后按固定顺序拼接
        std::string strings(1, '\0');
        auto appendName = [&strings](const std::string& name) -> std::uint32_t {
            if (name.empty()) return 0;
            const auto offset = static_cast<std::uint32_t>(strings.size());
            strings += name;
            strings.push_back('\0');
            return offset;
        };
        std::vector<std::uint32_t> sectionNameOffsets;
        sectionNameOffsets.reserve(sections.size());
        for (const SectionSpec* section : sections) sectionNameOffsets.push_back(appendName(section->name));
        const std::uint32_t moduleNameOffset = appendName(spec.moduleName);
        const std::uint16_t moduleNameBytes = static_cast<std::uint16_t>(spec.moduleName.size());
        for (const ExportSpec& entry : spec.exports) appendName(entry.stableName);
        for (const ImportSpec& entry : spec.imports) appendName(entry.stableName);
        for (const DependencySpec& entry : spec.dependencies) appendName(entry.logicalName);

        // 数量必须能放进头部的 16 位/32 位字段
        if (sections.size() > 0xFFFF) throw std::runtime_error("too many sections to serialize");
        if (spec.exports.size() > 0xFFFF) throw std::runtime_error("too many exports to serialize");
        if (spec.imports.size() > 0xFFFF) throw std::runtime_error("too many imports to serialize");
        if (spec.dependencies.size() > 0xFFFF) throw std::runtime_error("too many dependencies to serialize");
        if (spec.relocations.size() > 0xFFFFFFFF) throw std::runtime_error("too many relocations to serialize");
        // 槽位数写进 16 位字段
        for (const ExportSpec& entry : spec.exports) {
            if (entry.slots.size() > 0xFFFF) throw std::runtime_error("too many export slots to serialize: " + entry.stableName);
        }
        for (const ImportSpec& entry : spec.imports) {
            if (entry.patchSlots.size() > 0xFFFF) throw std::runtime_error("too many import slots to serialize: " + entry.stableName);
        }
        if (strings.size() > 0xFFFFFFFF) throw std::runtime_error("string table exceeds 32-bit offset space");

        // 计算规范化布局：表顺序固定，表区起始按各自对齐要求取整，段文件数据最后
        // 所有偏移先按 64 位累加，超过 32 位或 64 MiB 即拒绝，避免截断后写出错位容器
        auto toU32 = [](std::uint64_t value, const char* what) -> std::uint32_t {
            if (value > 0xFFFFFFFF) throw std::runtime_error(std::string(what) + " exceeds 32-bit offset space");
            return static_cast<std::uint32_t>(value);
        };
        std::uint64_t cursor = L::kHeaderBytes;
        const std::uint32_t sectionTableOffset = toU32(AlignUp(cursor, 4), "section table offset");
        cursor = AlignUp(cursor, 4) + static_cast<std::uint64_t>(sections.size()) * L::kSectionEntryBytes;
        const std::uint32_t exportTableOffset = toU32(AlignUp(cursor, 4), "export table offset");
        cursor = AlignUp(cursor, 4) + static_cast<std::uint64_t>(spec.exports.size()) * L::kExportEntryBytes;
        std::vector<std::uint32_t> exportSlotOffsets;
        exportSlotOffsets.reserve(spec.exports.size());
        for (const ExportSpec& entry : spec.exports) {
            exportSlotOffsets.push_back(toU32(AlignUp(cursor, 4), "export slot table offset"));
            cursor = AlignUp(cursor, 4) + static_cast<std::uint64_t>(entry.slots.size()) * 4;
        }
        const std::uint32_t importTableOffset = toU32(AlignUp(cursor, 4), "import table offset");
        cursor = AlignUp(cursor, 4) + static_cast<std::uint64_t>(spec.imports.size()) * L::kImportEntryBytes;
        std::vector<std::uint32_t> importPatchOffsets;
        importPatchOffsets.reserve(spec.imports.size());
        for (const ImportSpec& entry : spec.imports) {
            importPatchOffsets.push_back(toU32(AlignUp(cursor, 4), "import slot patch table offset"));
            cursor = AlignUp(cursor, 4) + static_cast<std::uint64_t>(entry.patchSlots.size()) * 4;
        }
        const std::uint32_t dependencyTableOffset = toU32(AlignUp(cursor, 4), "dependency table offset");
        cursor = AlignUp(cursor, 4) + static_cast<std::uint64_t>(spec.dependencies.size()) * L::kDependencyEntryBytes;
        const std::uint32_t relocationTableOffset = toU32(AlignUp(cursor, 8), "relocation table offset");
        cursor = AlignUp(cursor, 8) + static_cast<std::uint64_t>(spec.relocations.size()) * L::kRelocationEntryBytes;
        const std::uint32_t stringTableOffset = toU32(AlignUp(cursor, 4), "string table offset");
        cursor = AlignUp(cursor, 4) + strings.size();
        std::vector<std::uint32_t> sectionFileOffsets;
        sectionFileOffsets.reserve(sections.size());
        for (const SectionSpec* section : sections) {
            if (section->fileBytes.empty()) {
                sectionFileOffsets.push_back(0);
                continue;
            }
            sectionFileOffsets.push_back(toU32(AlignUp(cursor, 4), "section file offset"));
            cursor = AlignUp(cursor, 4) + section->fileBytes.size();
        }
        if (cursor > L::kMaxTotalBytes) throw std::runtime_error("container exceeds the 64 MiB total-length limit");
        const auto totalBytes = static_cast<std::uint32_t>(cursor);
        // 装载映像跨度：覆盖全部段的内存区间，4 KiB 整数倍
        std::uint64_t imageEnd = 0;
        for (const SectionSpec* section : sections) {
            imageEnd = std::max(imageEnd, static_cast<std::uint64_t>(section->memoryOffset) + section->memoryBytes);
        }
        const std::uint64_t imageBytes64 = AlignUp(imageEnd, L::kPageBytes);
        if (imageBytes64 > 0xFFFFFFFF) throw std::runtime_error("image span exceeds 32-bit offset space");

        Bytes bytes(totalBytes, 0);
        // 文件头
        std::memcpy(bytes.data() + L::kFieldMagic, L::kMagic.data(), L::kMagic.size());
        Put16(bytes, L::kFieldVersion, L::kVersion);
        Put16(bytes, L::kFieldHeaderBytes, static_cast<std::uint16_t>(L::kHeaderBytes));
        Put32(bytes, L::kFieldFlags, 0);
        Put32(bytes, L::kFieldTotalBytes, totalBytes);
        Put32(bytes, L::kFieldImageBytes, static_cast<std::uint32_t>(imageBytes64));
        Put32(bytes, L::kFieldEntryInit, spec.entryInit);
        Put32(bytes, L::kFieldEntryActivate, spec.entryActivate);
        Put32(bytes, L::kFieldEntryAbort, spec.entryAbort);
        Put16(bytes, L::kFieldSectionTableOffset, static_cast<std::uint16_t>(sectionTableOffset));
        Put16(bytes, L::kFieldSectionCount, static_cast<std::uint16_t>(sections.size()));
        Put16(bytes, L::kFieldSectionEntryBytes, L::kSectionEntryBytes);
        Put16(bytes, L::kFieldExportCount, static_cast<std::uint16_t>(spec.exports.size()));
        Put32(bytes, L::kFieldExportTableOffset, exportTableOffset);
        Put16(bytes, L::kFieldImportCount, static_cast<std::uint16_t>(spec.imports.size()));
        Put16(bytes, L::kFieldImportEntryBytes, L::kImportEntryBytes);
        Put32(bytes, L::kFieldImportTableOffset, importTableOffset);
        Put16(bytes, L::kFieldDependencyCount, static_cast<std::uint16_t>(spec.dependencies.size()));
        Put16(bytes, L::kFieldDependencyEntryBytes, L::kDependencyEntryBytes);
        Put32(bytes, L::kFieldDependencyTableOffset, dependencyTableOffset);
        Put32(bytes, L::kFieldRelocationCount, static_cast<std::uint32_t>(spec.relocations.size()));
        Put32(bytes, L::kFieldRelocationTableOffset, relocationTableOffset);
        Put32(bytes, L::kFieldStringTableOffset, stringTableOffset);
        Put32(bytes, L::kFieldStringTableBytes, static_cast<std::uint32_t>(strings.size()));
        const ModuleId moduleId = DeriveModuleId(spec.moduleName);
        std::memcpy(bytes.data() + L::kFieldModuleId, moduleId.data(), moduleId.size());
        Put32(bytes, L::kFieldModuleNameOffset, moduleNameOffset);
        Put16(bytes, L::kFieldModuleNameBytes, moduleNameBytes);
        Put16(bytes, L::kFieldModuleVersionMajor, spec.versionMajor);
        Put16(bytes, L::kFieldModuleVersionMinor, spec.versionMinor);
        Put16(bytes, L::kFieldModuleVersionPatch, spec.versionPatch);
        Put16(bytes, L::kFieldKernelAbiMin, spec.abiMin);
        Put16(bytes, L::kFieldKernelAbiMax, spec.abiMax);

        // 段表
        for (std::size_t i = 0; i < sections.size(); ++i) {
            const SectionSpec* section = sections[i];
            const std::uint64_t entry = static_cast<std::uint64_t>(sectionTableOffset) + i * L::kSectionEntryBytes;
            Put32(bytes, entry + L::kSectionNameOffset, sectionNameOffsets[i]);
            Put32(bytes, entry + L::kSectionType, section->type);
            Put32(bytes, entry + L::kSectionFlags, section->flags);
            Put32(bytes, entry + L::kSectionFileOffset, sectionFileOffsets[i]);
            Put32(bytes, entry + L::kSectionFileBytes, static_cast<std::uint32_t>(section->fileBytes.size()));
            Put32(bytes, entry + L::kSectionMemoryOffset, section->memoryOffset);
            Put32(bytes, entry + L::kSectionMemoryBytes, section->memoryBytes);
            Put32(bytes, entry + L::kSectionAlign, section->align);
            if (!section->fileBytes.empty()) {
                std::memcpy(bytes.data() + sectionFileOffsets[i], section->fileBytes.data(), section->fileBytes.size());
            }
        }
        // 导出表与槽位表
        for (std::size_t i = 0; i < spec.exports.size(); ++i) {
            const ExportSpec& entry = spec.exports[i];
            const std::uint64_t offset = static_cast<std::uint64_t>(exportTableOffset) + i * L::kExportEntryBytes;
            const InterfaceId interfaceId = DeriveInterfaceId(entry.stableName);
            std::memcpy(bytes.data() + offset + L::kExportInterfaceId, interfaceId.data(), interfaceId.size());
            Put16(bytes, offset + L::kExportVersion, entry.version);
            Put16(bytes, offset + L::kExportSlotCount, static_cast<std::uint16_t>(entry.slots.size()));
            Put32(bytes, offset + L::kExportSlotTableOffset, exportSlotOffsets[i]);
            Put32(bytes, offset + L::kExportReserved, 0);
            for (std::size_t slot = 0; slot < entry.slots.size(); ++slot) {
                Put32(bytes, static_cast<std::uint64_t>(exportSlotOffsets[i]) + slot * 4, entry.slots[slot]);
            }
        }
        // 导入表与回填表
        for (std::size_t i = 0; i < spec.imports.size(); ++i) {
            const ImportSpec& entry = spec.imports[i];
            const std::uint64_t offset = static_cast<std::uint64_t>(importTableOffset) + i * L::kImportEntryBytes;
            const InterfaceId interfaceId = DeriveInterfaceId(entry.stableName);
            std::memcpy(bytes.data() + offset + L::kImportInterfaceId, interfaceId.data(), interfaceId.size());
            Put16(bytes, offset + L::kImportMinVersion, entry.minVersion);
            Put16(bytes, offset + L::kImportMaxVersion, entry.maxVersion);
            Put32(bytes, offset + L::kImportSlotPatchOffset, importPatchOffsets[i]);
            Put16(bytes, offset + L::kImportSlotCount, static_cast<std::uint16_t>(entry.patchSlots.size()));
            Put16(bytes, offset + L::kImportReserved, 0);
            for (std::size_t slot = 0; slot < entry.patchSlots.size(); ++slot) {
                Put32(bytes, static_cast<std::uint64_t>(importPatchOffsets[i]) + slot * 4, entry.patchSlots[slot]);
            }
        }
        // 依赖表
        for (std::size_t i = 0; i < spec.dependencies.size(); ++i) {
            const DependencySpec& entry = spec.dependencies[i];
            const std::uint64_t offset = static_cast<std::uint64_t>(dependencyTableOffset) + i * L::kDependencyEntryBytes;
            const ModuleId target = DeriveModuleId(entry.logicalName);
            std::memcpy(bytes.data() + offset + L::kDependencyModuleId, target.data(), target.size());
            Put16(bytes, offset + L::kDependencyMinMajor, entry.minMajor);
            Put16(bytes, offset + L::kDependencyMinMinor, entry.minMinor);
            Put16(bytes, offset + L::kDependencyMinPatch, entry.minPatch);
            Put16(bytes, offset + L::kDependencyMaxMajor, entry.maxMajor);
            Put16(bytes, offset + L::kDependencyMaxMinor, entry.maxMinor);
            Put16(bytes, offset + L::kDependencyMaxPatch, entry.maxPatch);
            Put32(bytes, offset + L::kDependencyFlags, 0);
        }
        // 重定位表：按 Target 升序书写，保证同一输入字节固定
        std::vector<const RelocationSpec*> relocations;
        relocations.reserve(spec.relocations.size());
        for (const RelocationSpec& entry : spec.relocations) relocations.push_back(&entry);
        std::stable_sort(relocations.begin(), relocations.end(),
            [](const RelocationSpec* a, const RelocationSpec* b) { return a->target < b->target; });
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            const RelocationSpec* entry = relocations[i];
            const std::uint64_t offset = static_cast<std::uint64_t>(relocationTableOffset) + i * L::kRelocationEntryBytes;
            bytes[static_cast<std::size_t>(offset + L::kRelocationType)] = entry->type;
            bytes[static_cast<std::size_t>(offset + L::kRelocationReserved1)] = 0;
            Put16(bytes, offset + L::kRelocationReserved16, 0);
            Put32(bytes, offset + L::kRelocationTarget, entry->target);
            Put64(bytes, offset + L::kRelocationAddend, entry->addend);
        }
        // 字符串表
        std::memcpy(bytes.data() + stringTableOffset, strings.data(), strings.size());
        return bytes;
    }

    Image Parse(ByteView bytes) {
        Image image;
        // 文件头与边界
        if (bytes.size() < L::kHeaderBytes) throw std::runtime_error("container is shorter than the 0x80-byte header");
        for (std::size_t i = 0; i < L::kMagic.size(); ++i) {
            if (bytes[i] != L::kMagic[i]) throw std::runtime_error("container magic mismatch");
        }
        image.version = Get16(bytes, L::kFieldVersion);
        if (image.version != L::kVersion) {
            throw std::runtime_error("unsupported container version " + std::to_string(image.version));
        }
        if (Get16(bytes, L::kFieldHeaderBytes) != L::kHeaderBytes) throw std::runtime_error("HeaderBytes is not 0x80");
        if (Get32(bytes, L::kFieldFlags) != 0) throw std::runtime_error("header Flags are not zero");
        image.totalBytes = Get32(bytes, L::kFieldTotalBytes);
        if (image.totalBytes != bytes.size()) {
            throw std::runtime_error("TotalBytes does not match the file length: header "
                + std::to_string(image.totalBytes) + ", actual " + std::to_string(bytes.size()));
        }
        if (image.totalBytes > L::kMaxTotalBytes) throw std::runtime_error("TotalBytes exceeds the 64 MiB limit");
        image.imageBytes = Get32(bytes, L::kFieldImageBytes);
        if (image.imageBytes % L::kPageBytes != 0) throw std::runtime_error("ImageBytes is not a 4 KiB multiple");
        if (!IsZero(std::span<const std::uint8_t>(bytes.data() + L::kFieldReserved, L::kIdBytes))) {
            throw std::runtime_error("header Reserved is not zero");
        }
        image.entryInit = Get32(bytes, L::kFieldEntryInit);
        image.entryActivate = Get32(bytes, L::kFieldEntryActivate);
        image.entryAbort = Get32(bytes, L::kFieldEntryAbort);

        // 数量与表区边界（先只核对偏移与长度，语义在后面按表检查）
        const std::uint32_t sectionTableOffset = Get16(bytes, L::kFieldSectionTableOffset);
        const std::uint16_t sectionCount = Get16(bytes, L::kFieldSectionCount);
        if (sectionCount < L::kMinSectionCount || sectionCount > L::kMaxSectionCount) {
            throw std::runtime_error("section count " + std::to_string(sectionCount) + " is outside 1..16");
        }
        if (Get16(bytes, L::kFieldSectionEntryBytes) != L::kSectionEntryBytes) throw std::runtime_error("SectionEntryBytes is not 32");
        if (sectionTableOffset % 4 != 0) throw std::runtime_error("section table offset is not 4-byte aligned");
        CheckFileRange("section table", sectionTableOffset, static_cast<std::uint64_t>(sectionCount) * L::kSectionEntryBytes, image.totalBytes);
        const std::uint16_t exportCount = Get16(bytes, L::kFieldExportCount);
        if (exportCount > L::kMaxExportCount) throw std::runtime_error("export count " + std::to_string(exportCount) + " exceeds 128");
        const std::uint32_t exportTableOffset = Get32(bytes, L::kFieldExportTableOffset);
        if (exportTableOffset % 4 != 0) throw std::runtime_error("export table offset is not 4-byte aligned");
        CheckFileRange("export table", exportTableOffset, static_cast<std::uint64_t>(exportCount) * L::kExportEntryBytes, image.totalBytes);
        const std::uint16_t importCount = Get16(bytes, L::kFieldImportCount);
        if (importCount > L::kMaxImportCount) throw std::runtime_error("import count " + std::to_string(importCount) + " exceeds 128");
        if (Get16(bytes, L::kFieldImportEntryBytes) != L::kImportEntryBytes) throw std::runtime_error("ImportEntryBytes is not 28");
        const std::uint32_t importTableOffset = Get32(bytes, L::kFieldImportTableOffset);
        if (importTableOffset % 4 != 0) throw std::runtime_error("import table offset is not 4-byte aligned");
        CheckFileRange("import table", importTableOffset, static_cast<std::uint64_t>(importCount) * L::kImportEntryBytes, image.totalBytes);
        const std::uint16_t dependencyCount = Get16(bytes, L::kFieldDependencyCount);
        if (dependencyCount > L::kMaxDependencyCount) throw std::runtime_error("dependency count " + std::to_string(dependencyCount) + " exceeds 32");
        if (Get16(bytes, L::kFieldDependencyEntryBytes) != L::kDependencyEntryBytes) throw std::runtime_error("DependencyEntryBytes is not 32");
        const std::uint32_t dependencyTableOffset = Get32(bytes, L::kFieldDependencyTableOffset);
        if (dependencyTableOffset % 4 != 0) throw std::runtime_error("dependency table offset is not 4-byte aligned");
        CheckFileRange("dependency table", dependencyTableOffset, static_cast<std::uint64_t>(dependencyCount) * L::kDependencyEntryBytes, image.totalBytes);
        const std::uint32_t relocationCount = Get32(bytes, L::kFieldRelocationCount);
        if (relocationCount > L::kMaxRelocationCount) throw std::runtime_error("relocation count " + std::to_string(relocationCount) + " exceeds 65536");
        const std::uint32_t relocationTableOffset = Get32(bytes, L::kFieldRelocationTableOffset);
        if (relocationTableOffset % 8 != 0) throw std::runtime_error("relocation table offset is not 8-byte aligned");
        CheckFileRange("relocation table", relocationTableOffset, static_cast<std::uint64_t>(relocationCount) * L::kRelocationEntryBytes, image.totalBytes);
        const std::uint32_t stringTableOffset = Get32(bytes, L::kFieldStringTableOffset);
        const std::uint32_t stringTableBytes = Get32(bytes, L::kFieldStringTableBytes);
        CheckFileRange("string table", stringTableOffset, stringTableBytes, image.totalBytes);
        if (stringTableBytes > 0 && Get8(bytes, std::uint64_t(stringTableOffset) + stringTableBytes - 1) != 0) {
            throw std::runtime_error("string table is not NUL-terminated");
        }

        // 文件区间集合：表区、槽位表、回填表、字符串表与段文件数据互不重叠
        std::vector<Region> regions;
        auto addRegion = [&regions](const std::string& what, std::uint64_t offset, std::uint64_t size) {
            if (size > 0) regions.push_back({ what, offset, offset + size });
        };
        addRegion("section table", sectionTableOffset, static_cast<std::uint64_t>(sectionCount) * L::kSectionEntryBytes);
        addRegion("export table", exportTableOffset, static_cast<std::uint64_t>(exportCount) * L::kExportEntryBytes);
        addRegion("import table", importTableOffset, static_cast<std::uint64_t>(importCount) * L::kImportEntryBytes);
        addRegion("dependency table", dependencyTableOffset, static_cast<std::uint64_t>(dependencyCount) * L::kDependencyEntryBytes);
        addRegion("relocation table", relocationTableOffset, static_cast<std::uint64_t>(relocationCount) * L::kRelocationEntryBytes);
        addRegion("string table", stringTableOffset, stringTableBytes);

        // 段表条目：先做文件区间与对齐检查，同时登记段文件数据的重叠区间
        struct RawSection {
            std::uint32_t nameOffset;    // 段名在字符串表里的偏移
            std::uint32_t type;          // 段类型
            std::uint32_t flags;         // 权限位
            std::uint32_t fileOffset;    // 文件偏移
            std::uint32_t fileBytes;     // 文件内字节数
            std::uint32_t memoryOffset;  // 内存偏移
            std::uint32_t memoryBytes;   // 内存跨度
            std::uint32_t align;         // 段对齐
        };
        std::vector<RawSection> rawSections;
        rawSections.reserve(sectionCount);
        for (std::uint16_t i = 0; i < sectionCount; ++i) {
            const std::uint64_t entry = static_cast<std::uint64_t>(sectionTableOffset) + i * L::kSectionEntryBytes;
            RawSection section{};
            section.nameOffset = Get32(bytes, entry + L::kSectionNameOffset);
            section.type = Get32(bytes, entry + L::kSectionType);
            section.flags = Get32(bytes, entry + L::kSectionFlags);
            section.fileOffset = Get32(bytes, entry + L::kSectionFileOffset);
            section.fileBytes = Get32(bytes, entry + L::kSectionFileBytes);
            section.memoryOffset = Get32(bytes, entry + L::kSectionMemoryOffset);
            section.memoryBytes = Get32(bytes, entry + L::kSectionMemoryBytes);
            section.align = Get32(bytes, entry + L::kSectionAlign);
            if (section.fileBytes > 0) {
                if (section.fileOffset % 4 != 0) {
                    throw std::runtime_error("section " + std::to_string(i) + " file offset is not 4-byte aligned");
                }
                CheckFileRange("section " + std::to_string(i) + " file data", section.fileOffset, section.fileBytes, image.totalBytes);
                addRegion("section " + std::to_string(i) + " file data", section.fileOffset, section.fileBytes);
            }
            rawSections.push_back(section);
        }

        // 导出表条目：先做条目与槽位表的文件区间检查
        struct RawExport {
            InterfaceId interfaceId{};   // 接口标识
            std::uint16_t version = 0;   // 接口版本
            std::uint16_t slotCount = 0; // 槽位数
            std::uint32_t slotTableOffset = 0; // 槽位表文件偏移
            std::uint32_t reserved = 0;  // 保留字段
        };
        std::vector<RawExport> rawExports;
        rawExports.reserve(exportCount);
        for (std::uint16_t i = 0; i < exportCount; ++i) {
            const std::uint64_t entry = static_cast<std::uint64_t>(exportTableOffset) + i * L::kExportEntryBytes;
            RawExport parsed{};
            std::memcpy(parsed.interfaceId.data(), bytes.data() + entry + L::kExportInterfaceId, parsed.interfaceId.size());
            parsed.version = Get16(bytes, entry + L::kExportVersion);
            parsed.slotCount = Get16(bytes, entry + L::kExportSlotCount);
            parsed.slotTableOffset = Get32(bytes, entry + L::kExportSlotTableOffset);
            parsed.reserved = Get32(bytes, entry + L::kExportReserved);
            if (parsed.slotTableOffset % 4 != 0) {
                throw std::runtime_error("export " + std::to_string(i) + " slot table offset is not 4-byte aligned");
            }
            CheckFileRange("export " + std::to_string(i) + " slot table", parsed.slotTableOffset,
                static_cast<std::uint64_t>(parsed.slotCount) * 4, image.totalBytes);
            addRegion("export " + std::to_string(i) + " slot table", parsed.slotTableOffset,
                static_cast<std::uint64_t>(parsed.slotCount) * 4);
            rawExports.push_back(parsed);
        }

        // 导入表条目：先做条目与回填表的文件区间检查
        struct RawImport {
            InterfaceId interfaceId{};         // 接口标识
            std::uint16_t minVersion = 0;      // 最低版本
            std::uint16_t maxVersion = 0;      // 最高版本
            std::uint32_t slotPatchOffset = 0; // 回填表文件偏移
            std::uint16_t slotCount = 0;       // 使用槽位数
            std::uint16_t reserved = 0;        // 保留字段
        };
        std::vector<RawImport> rawImports;
        rawImports.reserve(importCount);
        for (std::uint16_t i = 0; i < importCount; ++i) {
            const std::uint64_t entry = static_cast<std::uint64_t>(importTableOffset) + i * L::kImportEntryBytes;
            RawImport parsed{};
            std::memcpy(parsed.interfaceId.data(), bytes.data() + entry + L::kImportInterfaceId, parsed.interfaceId.size());
            parsed.minVersion = Get16(bytes, entry + L::kImportMinVersion);
            parsed.maxVersion = Get16(bytes, entry + L::kImportMaxVersion);
            parsed.slotPatchOffset = Get32(bytes, entry + L::kImportSlotPatchOffset);
            parsed.slotCount = Get16(bytes, entry + L::kImportSlotCount);
            parsed.reserved = Get16(bytes, entry + L::kImportReserved);
            if (parsed.slotPatchOffset % 4 != 0) {
                throw std::runtime_error("import " + std::to_string(i) + " slot patch table offset is not 4-byte aligned");
            }
            CheckFileRange("import " + std::to_string(i) + " slot patch table", parsed.slotPatchOffset,
                static_cast<std::uint64_t>(parsed.slotCount) * 4, image.totalBytes);
            addRegion("import " + std::to_string(i) + " slot patch table", parsed.slotPatchOffset,
                static_cast<std::uint64_t>(parsed.slotCount) * 4);
            rawImports.push_back(parsed);
        }

        // 依赖表条目（固定长度表，区间已登记）
        struct RawDependency {
            ModuleId moduleId{};           // 被依赖模块标识
            std::uint16_t minMajor = 0;    // 最低 major
            std::uint16_t minMinor = 0;    // 最低 minor
            std::uint16_t minPatch = 0;    // 最低 patch
            std::uint16_t maxMajor = 0;    // 最高 major
            std::uint16_t maxMinor = 0;    // 最高 minor
            std::uint16_t maxPatch = 0;    // 最高 patch
            std::uint32_t flags = 0;       // 保留字段
        };
        std::vector<RawDependency> rawDependencies;
        rawDependencies.reserve(dependencyCount);
        for (std::uint16_t i = 0; i < dependencyCount; ++i) {
            const std::uint64_t entry = static_cast<std::uint64_t>(dependencyTableOffset) + i * L::kDependencyEntryBytes;
            RawDependency parsed{};
            std::memcpy(parsed.moduleId.data(), bytes.data() + entry + L::kDependencyModuleId, parsed.moduleId.size());
            parsed.minMajor = Get16(bytes, entry + L::kDependencyMinMajor);
            parsed.minMinor = Get16(bytes, entry + L::kDependencyMinMinor);
            parsed.minPatch = Get16(bytes, entry + L::kDependencyMinPatch);
            parsed.maxMajor = Get16(bytes, entry + L::kDependencyMaxMajor);
            parsed.maxMinor = Get16(bytes, entry + L::kDependencyMaxMinor);
            parsed.maxPatch = Get16(bytes, entry + L::kDependencyMaxPatch);
            parsed.flags = Get32(bytes, entry + L::kDependencyFlags);
            rawDependencies.push_back(parsed);
        }

        // 重定位表条目
        struct RawRelocation {
            std::uint8_t type = 0;        // 重定位类型
            std::uint8_t reserved1 = 0;   // 保留字节
            std::uint16_t reserved16 = 0; // 保留字段
            std::uint32_t target = 0;     // 内存偏移
            std::uint64_t addend = 0;     // 加数
        };
        std::vector<RawRelocation> rawRelocations;
        rawRelocations.reserve(relocationCount);
        for (std::uint32_t i = 0; i < relocationCount; ++i) {
            const std::uint64_t entry = static_cast<std::uint64_t>(relocationTableOffset) + i * L::kRelocationEntryBytes;
            RawRelocation parsed{};
            parsed.type = Get8(bytes, entry + L::kRelocationType);
            parsed.reserved1 = Get8(bytes, entry + L::kRelocationReserved1);
            parsed.reserved16 = Get16(bytes, entry + L::kRelocationReserved16);
            parsed.target = Get32(bytes, entry + L::kRelocationTarget);
            parsed.addend = Get64(bytes, entry + L::kRelocationAddend);
            rawRelocations.push_back(parsed);
        }

        CheckRegions(std::move(regions));

        // 段语义：类型与权限组合、对齐、内存区间、文件区间与名字
        image.sections.reserve(sectionCount);
        for (std::uint16_t i = 0; i < sectionCount; ++i) {
            const RawSection& raw = rawSections[i];
            const std::string where = "section " + std::to_string(i);
            std::uint32_t expectedFlags = 0;
            switch (raw.type) {
                case L::kTypeText: expectedFlags = L::kFlagRead | L::kFlagExecute; break;
                case L::kTypeRodata: expectedFlags = L::kFlagRead; break;
                case L::kTypeData:
                case L::kTypeBss: expectedFlags = L::kFlagRead | L::kFlagWrite; break;
                default: throw std::runtime_error(where + " has unknown type " + std::to_string(raw.type));
            }
            if (raw.flags != expectedFlags) {
                throw std::runtime_error(where + " flags do not match its type (flags "
                    + std::to_string(raw.flags) + ", expected " + std::to_string(expectedFlags) + ")");
            }
            if (raw.align < L::kMinSectionAlign || raw.align > L::kMaxSectionAlign
                || (raw.align & (raw.align - 1)) != 0) {
                throw std::runtime_error(where + " align is not a power of two in 4..4096");
            }
            if (raw.memoryOffset % L::kPageBytes != 0) throw std::runtime_error(where + " memory offset is not 4 KiB aligned");
            if (raw.memoryBytes % L::kPageBytes != 0) throw std::runtime_error(where + " memory span is not a 4 KiB multiple");
            if (raw.memoryBytes == 0) throw std::runtime_error(where + " memory span is zero");
            if (static_cast<std::uint64_t>(raw.memoryOffset) + raw.memoryBytes > image.imageBytes) {
                throw std::runtime_error(where + " memory range exceeds ImageBytes");
            }
            if (raw.type == L::kTypeBss) {
                if (raw.fileOffset != 0 || raw.fileBytes != 0) throw std::runtime_error(where + " is BSS but has file bytes");
            } else {
                if (raw.fileBytes == 0) throw std::runtime_error(where + " is not BSS but has no file bytes");
                if (raw.fileBytes > raw.memoryBytes) throw std::runtime_error(where + " FileBytes exceeds MemoryBytes");
            }
            std::string name;
            if (raw.nameOffset != 0) {
                if (stringTableBytes == 0 || raw.nameOffset >= stringTableBytes) {
                    throw std::runtime_error(where + " name offset is outside the string table");
                }
                std::size_t length = 0;
                while (Get8(bytes, static_cast<std::uint64_t>(stringTableOffset) + raw.nameOffset + length) != 0) ++length;
                name.assign(reinterpret_cast<const char*>(bytes.data() + stringTableOffset + raw.nameOffset), length);
            }
            image.sections.push_back({ name, raw.type, raw.flags, raw.fileOffset, raw.fileBytes,
                raw.memoryOffset, raw.memoryBytes, raw.align });
        }
        // 段的内存区间两两不重叠（段表按内存偏移升序书写是写出器要求，读取方不依赖顺序）
        std::vector<std::size_t> memoryOrder(image.sections.size());
        for (std::size_t i = 0; i < memoryOrder.size(); ++i) memoryOrder[i] = i;
        std::sort(memoryOrder.begin(), memoryOrder.end(), [&image](std::size_t a, std::size_t b) {
            return image.sections[a].memoryOffset < image.sections[b].memoryOffset;
        });
        for (std::size_t i = 0; i + 1 < memoryOrder.size(); ++i) {
            const SectionInfo& first = image.sections[memoryOrder[i]];
            const SectionInfo& second = image.sections[memoryOrder[i + 1]];
            if (static_cast<std::uint64_t>(first.memoryOffset) + first.memoryBytes > second.memoryOffset) {
                throw std::runtime_error("sections " + std::to_string(memoryOrder[i]) + " and " + std::to_string(memoryOrder[i + 1])
                    + " memory ranges overlap");
            }
        }

        // 入口：Init 必须非零且落在 TEXT；Activate 与 Abort 允许为 0
        auto findContaining = [&image](std::uint64_t begin, std::uint64_t width, std::uint32_t typeA,
            std::uint32_t typeB) -> std::size_t {
            for (std::size_t i = 0; i < image.sections.size(); ++i) {
                const SectionInfo& section = image.sections[i];
                if (section.type != typeA && section.type != typeB) continue;
                if (begin < section.memoryOffset) continue;
                if (begin + width <= static_cast<std::uint64_t>(section.memoryOffset) + section.memoryBytes) return i;
            }
            return image.sections.size();
        };
        if (image.entryInit == 0) throw std::runtime_error("EntryInit is zero");
        if (findContaining(image.entryInit, 1, L::kTypeText, L::kTypeText) == image.sections.size()) {
            throw std::runtime_error("EntryInit is not inside a TEXT section");
        }
        if (image.entryActivate != 0 && findContaining(image.entryActivate, 1, L::kTypeText, L::kTypeText) == image.sections.size()) {
            throw std::runtime_error("EntryActivate is not inside a TEXT section");
        }
        if (image.entryAbort != 0 && findContaining(image.entryAbort, 1, L::kTypeText, L::kTypeText) == image.sections.size()) {
            throw std::runtime_error("EntryAbort is not inside a TEXT section");
        }

        // 导出：标识非零、版本非零、槽数、槽位落在 TEXT、同标识同版本唯一
        image.exports.reserve(exportCount);
        for (std::uint16_t i = 0; i < exportCount; ++i) {
            const RawExport& raw = rawExports[i];
            const std::string where = "export " + std::to_string(i);
            if (IsZero(raw.interfaceId)) throw std::runtime_error(where + " InterfaceId is zero");
            if (raw.version == 0) throw std::runtime_error(where + " version is zero");
            if (raw.slotCount < L::kMinSlotCount || raw.slotCount > L::kMaxSlotCount) {
                throw std::runtime_error(where + " slot count " + std::to_string(raw.slotCount) + " is outside 1..64");
            }
            if (raw.reserved != 0) throw std::runtime_error(where + " Reserved is not zero");
            for (std::size_t earlier = 0; earlier < image.exports.size(); ++earlier) {
                if (image.exports[earlier].interfaceId == raw.interfaceId && image.exports[earlier].version == raw.version) {
                    throw std::runtime_error(where + " duplicates an earlier export with the same InterfaceId and Version");
                }
            }
            ExportInfo info;
            info.interfaceId = raw.interfaceId;
            info.version = raw.version;
            info.slots.reserve(raw.slotCount);
            for (std::uint16_t slot = 0; slot < raw.slotCount; ++slot) {
                const std::uint32_t value = Get32(bytes, static_cast<std::uint64_t>(raw.slotTableOffset) + slot * 4);
                if (findContaining(value, 1, L::kTypeText, L::kTypeText) == image.sections.size()) {
                    throw std::runtime_error(where + " slot " + std::to_string(slot) + " is not inside a TEXT section");
                }
                info.slots.push_back(value);
            }
            image.exports.push_back(std::move(info));
        }

        // 导入：标识非零、版本区间、槽数、回填槽可写且 8 字节对齐、同标识唯一
        image.imports.reserve(importCount);
        for (std::uint16_t i = 0; i < importCount; ++i) {
            const RawImport& raw = rawImports[i];
            const std::string where = "import " + std::to_string(i);
            if (IsZero(raw.interfaceId)) throw std::runtime_error(where + " InterfaceId is zero");
            if (raw.minVersion == 0) throw std::runtime_error(where + " MinVersion is zero");
            if (raw.maxVersion != L::kVersionUnlimited && raw.maxVersion < raw.minVersion) {
                throw std::runtime_error(where + " version range is reversed");
            }
            if (raw.slotCount < L::kMinSlotCount || raw.slotCount > L::kMaxSlotCount) {
                throw std::runtime_error(where + " slot count " + std::to_string(raw.slotCount) + " is outside 1..64");
            }
            if (raw.reserved != 0) throw std::runtime_error(where + " Reserved is not zero");
            for (const ImportInfo& earlier : image.imports) {
                if (earlier.interfaceId == raw.interfaceId) throw std::runtime_error(where + " duplicates an earlier import for the same InterfaceId");
            }
            ImportInfo info;
            info.interfaceId = raw.interfaceId;
            info.minVersion = raw.minVersion;
            info.maxVersion = raw.maxVersion;
            info.patchSlots.reserve(raw.slotCount);
            for (std::uint16_t slot = 0; slot < raw.slotCount; ++slot) {
                const std::uint32_t value = Get32(bytes, static_cast<std::uint64_t>(raw.slotPatchOffset) + slot * 4);
                if (value % 8 != 0) throw std::runtime_error(where + " patch slot " + std::to_string(slot) + " is not 8-byte aligned");
                if (findContaining(value, 8, L::kTypeData, L::kTypeBss) == image.sections.size()) {
                    throw std::runtime_error(where + " patch slot " + std::to_string(slot) + " is not inside a writable section");
                }
                info.patchSlots.push_back(value);
            }
            image.imports.push_back(std::move(info));
        }

        // 依赖：标识非零且不等于本模块、区间不颠倒、同标识唯一
        std::memcpy(image.moduleId.data(), bytes.data() + L::kFieldModuleId, image.moduleId.size());
        if (IsZero(image.moduleId)) throw std::runtime_error("ModuleId is zero");
        image.dependencies.reserve(dependencyCount);
        for (std::uint16_t i = 0; i < dependencyCount; ++i) {
            const RawDependency& raw = rawDependencies[i];
            const std::string where = "dependency " + std::to_string(i);
            if (IsZero(raw.moduleId)) throw std::runtime_error(where + " ModuleId is zero");
            if (raw.moduleId == image.moduleId) throw std::runtime_error(where + " is a self dependency");
            if (raw.minMajor == 0) throw std::runtime_error(where + " MinMajor is zero");
            if (raw.flags != 0) throw std::runtime_error(where + " Flags are not zero");
            if (raw.maxMajor != L::kMajorUnlimited) {
                const bool higher = raw.maxMajor > raw.minMajor
                    || (raw.maxMajor == raw.minMajor && raw.maxMinor > raw.minMinor)
                    || (raw.maxMajor == raw.minMajor && raw.maxMinor == raw.minMinor && raw.maxPatch >= raw.minPatch);
                if (!higher) throw std::runtime_error(where + " version range is reversed");
            }
            for (const DependencyInfo& earlier : image.dependencies) {
                if (earlier.moduleId == raw.moduleId) throw std::runtime_error(where + " duplicates an earlier dependency");
            }
            image.dependencies.push_back({ raw.moduleId, raw.minMajor, raw.minMinor, raw.minPatch,
                raw.maxMajor, raw.maxMinor, raw.maxPatch });
        }

        // 重定位：类型白名单、保留字段、目标可写且按宽度对齐
        image.relocations.reserve(relocationCount);
        for (std::uint32_t i = 0; i < relocationCount; ++i) {
            const RawRelocation& raw = rawRelocations[i];
            const std::string where = "relocation " + std::to_string(i);
            if (raw.type != L::kRelocationRelative64 && raw.type != L::kRelocationRelative32) {
                throw std::runtime_error(where + " has unsupported type " + std::to_string(raw.type));
            }
            if (raw.reserved1 != 0 || raw.reserved16 != 0) throw std::runtime_error(where + " Reserved is not zero");
            const std::uint64_t width = raw.type == L::kRelocationRelative64 ? 8 : 4;
            if (raw.target % width != 0) throw std::runtime_error(where + " target is not aligned to the relocation width");
            if (findContaining(raw.target, width, L::kTypeData, L::kTypeBss) == image.sections.size()) {
                throw std::runtime_error(where + " target is not inside a writable section");
            }
            image.relocations.push_back({ raw.type, raw.target, raw.addend });
        }

        // 模块标识与版本
        image.versionMajor = Get16(bytes, L::kFieldModuleVersionMajor);
        image.versionMinor = Get16(bytes, L::kFieldModuleVersionMinor);
        image.versionPatch = Get16(bytes, L::kFieldModuleVersionPatch);
        image.abiMin = Get16(bytes, L::kFieldKernelAbiMin);
        image.abiMax = Get16(bytes, L::kFieldKernelAbiMax);
        if (image.versionMajor == 0) throw std::runtime_error("ModuleVersionMajor is zero");
        if (image.abiMin == 0) throw std::runtime_error("KernelAbiMin is zero");

        // 模块名：偏移与字节数要么同时为 0，要么指向字符串表内以 NUL 结尾的名字
        const std::uint32_t moduleNameOffset = Get32(bytes, L::kFieldModuleNameOffset);
        const std::uint16_t moduleNameBytes = Get16(bytes, L::kFieldModuleNameBytes);
        if ((moduleNameOffset == 0) != (moduleNameBytes == 0)) {
            throw std::runtime_error("ModuleName offset and byte count are inconsistent");
        }
        if (moduleNameBytes > 0) {
            if (moduleNameOffset >= stringTableBytes
                || static_cast<std::uint64_t>(moduleNameOffset) + moduleNameBytes >= stringTableBytes) {
                throw std::runtime_error("ModuleName is outside the string table");
            }
            if (Get8(bytes, static_cast<std::uint64_t>(stringTableOffset) + moduleNameOffset + moduleNameBytes) != 0) {
                throw std::runtime_error("ModuleName is not NUL-terminated");
            }
            image.moduleName.assign(reinterpret_cast<const char*>(bytes.data() + stringTableOffset + moduleNameOffset), moduleNameBytes);
        }
        return image;
    }

    void VerifyDependencyGraph(std::span<const DependencyNode> nodes) {
        // 标识到节点下标的映射，顺带检查全零与重复
        std::map<ModuleId, std::size_t> index;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (IsZero(nodes[i].moduleId)) throw std::runtime_error("dependency graph node " + std::to_string(i) + " has a zero ModuleId");
            if (!index.emplace(nodes[i].moduleId, i).second) {
                throw std::runtime_error("dependency graph has duplicate ModuleId " + HexText(nodes[i].moduleId));
            }
        }
        // 依赖边：缺节点与自依赖在这里拒绝
        std::vector<std::vector<std::size_t>> edges(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (const ModuleId& dependency : nodes[i].dependencies) {
                if (dependency == nodes[i].moduleId) {
                    throw std::runtime_error("dependency graph has a self dependency at module '" + nodes[i].name + "'");
                }
                const auto found = index.find(dependency);
                if (found == index.end()) {
                    throw std::runtime_error("dependency graph misses module " + HexText(dependency) + " needed by '" + nodes[i].name + "'");
                }
                edges[i].push_back(found->second);
            }
        }
        // 三色深度优先：1 表示在栈上，遇到它即环
        std::vector<std::uint8_t> colors(nodes.size(), 0);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (colors[i] == 0) VisitDependency(i, edges, nodes, colors);
        }
    }
}
