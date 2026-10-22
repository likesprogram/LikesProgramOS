/* ModuleContainer.cpp
    内核模块容器（.osm）的有界解析实现：头、段表、导出 / 导入 / 依赖 / 重定位 / 字符串表

    全部读取按固定偏移与固定上界进行，不按容器内容长度分配内存；任何偏移与长度的加法先做
    64 位检查再比较，失败返回 ContainerError 分类，不打印、不停机、不依赖 Storage / Print
    解析顺序：文件头与表区边界 → 模块身份与模块名 → 段表与段文件区间 → 表项结构（槽位表 /
    回填表区间）→ 文件区间重叠 → 入口与各表条目语义；顺序保证同一处改坏只报最靠近根因的分类
*/

#include <ModuleContainer.hpp>

namespace {
    // 小端读取 16 位
    uint16_t Read16(const uint8_t* field) {
        return static_cast<uint16_t>(field[0] | (field[1] << 8));
    }

    // 小端读取 32 位
    uint32_t Read32(const uint8_t* field) {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
        return value;
    }

    // 小端读取 64 位
    uint64_t Read64(const uint8_t* field) {
        return static_cast<uint64_t>(Read32(field)) | (static_cast<uint64_t>(Read32(field + 4)) << 32);
    }

    // 判断一段字节是否全零
    bool IsZeroBytes(const uint8_t* bytes, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            if (bytes[i] != 0) return false;
        }
        return true;
    }

    // 比较两个 16 字节标识是否相等
    bool SameId(const uint8_t* left, const uint8_t* right) {
        for (uint32_t i = 0; i < 16; ++i) {
            if (left[i] != right[i]) return false;
        }
        return true;
    }

    // 复制 16 字节标识到目标字节数组
    void CopyId(uint8_t* target, const uint8_t* source) {
        for (uint32_t i = 0; i < 16; ++i) target[i] = source[i];
    }

    // 判断文件区间是否落在 [0x80, totalBytes) 内且不越过文件末尾
    bool RangeFits(uint64_t offset, uint64_t bytes, uint64_t totalBytes) {
        if (offset < Kernel::kContainerHeaderBytes || offset >= totalBytes) return false;
        return bytes <= totalBytes - offset;
    }

    // 段类型对应的合法权限组合；类型未知返回 false
    bool ExpectedFlags(uint32_t type, uint32_t& flags) {
        switch (type) {
            case static_cast<uint32_t>(Kernel::ContainerSectionType::Text):
                flags = Kernel::kContainerFlagRead | Kernel::kContainerFlagExecute;
                return true;
            case static_cast<uint32_t>(Kernel::ContainerSectionType::Rodata):
                flags = Kernel::kContainerFlagRead;
                return true;
            case static_cast<uint32_t>(Kernel::ContainerSectionType::Data):
            case static_cast<uint32_t>(Kernel::ContainerSectionType::Bss):
                flags = Kernel::kContainerFlagRead | Kernel::kContainerFlagWrite;
                return true;
            default:
                return false;
        }
    }

    // 在指定类型的段里查找完整包含 [offset, offset+bytes) 的段
    bool ContainsRange(const Kernel::ContainerView& view, uint64_t offset, uint64_t bytes,
        uint32_t typeA, uint32_t typeB) {
        for (uint32_t i = 0; i < view.sectionCount; ++i) {
            const Kernel::ContainerSection& section = view.sections[i];
            if (section.type != typeA && section.type != typeB) continue;
            if (offset < section.memoryOffset) continue;
            if (bytes > section.memoryBytes) continue;
            if (offset - section.memoryOffset > section.memoryBytes - bytes) continue;
            return true;
        }
        return false;
    }

    // 容器文件区间：表区、槽位表、回填表、字符串表与段文件数据的重叠检查单元
    struct Region {
        uint64_t begin;   // 起始字节偏移
        uint64_t end;     // 结束字节偏移（不含）
    };

    // 区间数量上界：6 个固定表 + 导出槽位表 + 导入回填表 + 段文件数据
    constexpr uint32_t kMaxRegions = 6 + Kernel::kContainerMaxExports + Kernel::kContainerMaxImports
        + Kernel::kContainerMaxSections;

    // 检查区间集合两两不重叠：先按起点插入排序，再比较相邻区间
    Kernel::ContainerError CheckRegions(Region* regions, uint32_t count) {
        for (uint32_t i = 1; i < count; ++i) {
            const Region current = regions[i];
            uint32_t position = i;
            while (position > 0 && regions[position - 1].begin > current.begin) {
                regions[position] = regions[position - 1];
                --position;
            }
            regions[position] = current;
        }
        for (uint32_t i = 0; i + 1 < count; ++i) {
            if (regions[i].end > regions[i + 1].begin) return Kernel::ContainerError::RegionOverlap;
        }
        return Kernel::ContainerError::None;
    }
}

namespace Kernel {
    ContainerError ParseModuleContainer(const uint8_t* bytes, uint32_t bytesCount, ContainerView& view) {
        view = ContainerView{};
        if (bytes == nullptr) return ContainerError::NullPointer;
        if (bytesCount < kContainerHeaderBytes) return ContainerError::TooShort;

        // —— 文件头固定字段 ——
        static constexpr uint8_t kMagic[8] = { 'L', 'P', 'O', 'S', 'M', 'O', 'D', 0 };
        for (uint32_t i = 0; i < 8; ++i) {
            if (bytes[i] != kMagic[i]) return ContainerError::HeaderMagic;
        }
        if (Read16(bytes + 0x08) != kContainerVersion) return ContainerError::HeaderVersion;
        if (Read16(bytes + 0x0A) != kContainerHeaderBytes) return ContainerError::HeaderBytes;
        if (Read32(bytes + 0x0C) != 0) return ContainerError::HeaderFlags;
        const uint32_t totalBytes = Read32(bytes + 0x10);
        if (totalBytes != bytesCount || totalBytes > kContainerMaxTotalBytes) return ContainerError::HeaderTotalBytes;
        const uint32_t imageBytes = Read32(bytes + 0x14);
        if (imageBytes % kContainerPageBytes != 0) return ContainerError::HeaderImageBytes;
        if (!IsZeroBytes(bytes + 0x70, 16)) return ContainerError::HeaderReserved;

        // —— 表偏移与计数：先验数量上限，再验偏移对齐与表区边界 ——
        const uint32_t sectionTableOffset = Read16(bytes + 0x24);
        const uint32_t sectionCount = Read16(bytes + 0x26);
        if (sectionCount < 1 || sectionCount > kContainerMaxSections) return ContainerError::SectionCount;
        if (Read16(bytes + 0x28) != kContainerSectionEntryBytes) return ContainerError::SectionEntryBytes;
        const uint32_t exportCount = Read16(bytes + 0x2A);
        if (exportCount > kContainerMaxExports) return ContainerError::ExportCount;
        const uint32_t exportTableOffset = Read32(bytes + 0x2C);
        const uint32_t importCount = Read16(bytes + 0x30);
        if (importCount > kContainerMaxImports) return ContainerError::ImportCount;
        if (Read16(bytes + 0x32) != kContainerImportEntryBytes) return ContainerError::ImportEntryBytes;
        const uint32_t importTableOffset = Read32(bytes + 0x34);
        const uint32_t dependencyCount = Read16(bytes + 0x38);
        if (dependencyCount > kContainerMaxDependencies) return ContainerError::DependencyCount;
        if (Read16(bytes + 0x3A) != kContainerDependencyEntryBytes) return ContainerError::DependencyEntryBytes;
        const uint32_t dependencyTableOffset = Read32(bytes + 0x3C);
        const uint32_t relocationCount = Read32(bytes + 0x40);
        if (relocationCount > kContainerMaxRelocations) return ContainerError::RelocationCount;
        const uint32_t relocationTableOffset = Read32(bytes + 0x44);
        const uint32_t stringTableOffset = Read32(bytes + 0x48);
        const uint32_t stringTableBytes = Read32(bytes + 0x4C);

        // 段表
        if (sectionTableOffset % 4 != 0) return ContainerError::SectionTableOffset;
        if (sectionTableOffset < kContainerHeaderBytes || sectionTableOffset >= totalBytes) return ContainerError::SectionTableOffset;
        if (static_cast<uint64_t>(sectionCount) * kContainerSectionEntryBytes > totalBytes - sectionTableOffset) return ContainerError::SectionTableBounds;
        // 导出表
        if (exportTableOffset % 4 != 0) return ContainerError::ExportTableOffset;
        if (exportTableOffset < kContainerHeaderBytes || exportTableOffset >= totalBytes) return ContainerError::ExportTableOffset;
        if (static_cast<uint64_t>(exportCount) * kContainerExportEntryBytes > totalBytes - exportTableOffset) return ContainerError::ExportTableBounds;
        // 导入表
        if (importTableOffset % 4 != 0) return ContainerError::ImportTableOffset;
        if (importTableOffset < kContainerHeaderBytes || importTableOffset >= totalBytes) return ContainerError::ImportTableOffset;
        if (static_cast<uint64_t>(importCount) * kContainerImportEntryBytes > totalBytes - importTableOffset) return ContainerError::ImportTableBounds;
        // 依赖表
        if (dependencyTableOffset % 4 != 0) return ContainerError::DependencyTableOffset;
        if (dependencyTableOffset < kContainerHeaderBytes || dependencyTableOffset >= totalBytes) return ContainerError::DependencyTableOffset;
        if (static_cast<uint64_t>(dependencyCount) * kContainerDependencyEntryBytes > totalBytes - dependencyTableOffset) return ContainerError::DependencyTableBounds;
        // 重定位表
        if (relocationTableOffset % 8 != 0) return ContainerError::RelocationTableOffset;
        if (relocationTableOffset < kContainerHeaderBytes || relocationTableOffset >= totalBytes) return ContainerError::RelocationTableOffset;
        if (static_cast<uint64_t>(relocationCount) * kContainerRelocationEntryBytes > totalBytes - relocationTableOffset) return ContainerError::RelocationTableBounds;
        // 字符串表：偏移在文件内、区间不越界、末字节为 NUL
        if (stringTableOffset < kContainerHeaderBytes || stringTableOffset >= totalBytes) return ContainerError::StringTableOffset;
        if (stringTableBytes > totalBytes - stringTableOffset) return ContainerError::StringTableBounds;
        if (stringTableBytes > 0 && bytes[stringTableOffset + stringTableBytes - 1] != 0) return ContainerError::StringTableNul;

        // —— 模块身份、版本、ABI 与模块名 ——
        if (IsZeroBytes(bytes + 0x50, 16)) return ContainerError::ModuleId;
        CopyId(view.moduleId.bytes, bytes + 0x50);
        view.versionMajor = Read16(bytes + 0x66);
        view.versionMinor = Read16(bytes + 0x68);
        view.versionPatch = Read16(bytes + 0x6A);
        if (view.versionMajor == 0) return ContainerError::ModuleVersion;
        view.abiMin = Read16(bytes + 0x6C);
        view.abiMax = Read16(bytes + 0x6E);
        if (view.abiMin == 0) return ContainerError::KernelAbi;
        const uint32_t moduleNameOffset = Read32(bytes + 0x60);
        const uint32_t moduleNameBytes = Read16(bytes + 0x64);
        if ((moduleNameOffset == 0) != (moduleNameBytes == 0)) return ContainerError::ModuleName;
        if (moduleNameBytes > 0) {
            if (stringTableBytes == 0 || moduleNameOffset >= stringTableBytes) return ContainerError::ModuleName;
            // 名字终止 NUL 必须落在字符串表内：offset + bytes 之后仍要有字节
            if (moduleNameBytes >= stringTableBytes - moduleNameOffset) return ContainerError::ModuleName;
            if (bytes[stringTableOffset + moduleNameOffset + moduleNameBytes] != 0) return ContainerError::ModuleName;
            view.moduleName = bytes + stringTableOffset + moduleNameOffset;
            view.moduleNameBytes = static_cast<uint16_t>(moduleNameBytes);
        }

        // —— 文件区间集合：固定表区、字符串表与段文件数据先登记，槽位表与回填表在结构检查时补 ——
        Region regions[kMaxRegions];
        uint32_t regionCount = 0;
        auto addRegion = [&regions, &regionCount](uint64_t begin, uint64_t size) {
            if (size == 0 || regionCount >= kMaxRegions) return;
            regions[regionCount].begin = begin;
            regions[regionCount].end = begin + size;
            ++regionCount;
        };
        addRegion(sectionTableOffset, static_cast<uint64_t>(sectionCount) * kContainerSectionEntryBytes);
        addRegion(exportTableOffset, static_cast<uint64_t>(exportCount) * kContainerExportEntryBytes);
        addRegion(importTableOffset, static_cast<uint64_t>(importCount) * kContainerImportEntryBytes);
        addRegion(dependencyTableOffset, static_cast<uint64_t>(dependencyCount) * kContainerDependencyEntryBytes);
        addRegion(relocationTableOffset, static_cast<uint64_t>(relocationCount) * kContainerRelocationEntryBytes);
        addRegion(stringTableOffset, stringTableBytes);

        // —— 段表：类型与权限、对齐、内存区间、文件区间与段名 ——
        view.sectionCount = sectionCount;
        for (uint32_t i = 0; i < sectionCount; ++i) {
            const uint8_t* const entry = bytes + sectionTableOffset + static_cast<uint64_t>(i) * kContainerSectionEntryBytes;
            ContainerSection& section = view.sections[i];
            section.name = nullptr;
            section.nameBytes = 0;
            section.type = Read32(entry + 0x04);
            section.flags = Read32(entry + 0x08);
            section.fileOffset = Read32(entry + 0x0C);
            section.fileBytes = Read32(entry + 0x10);
            section.memoryOffset = Read32(entry + 0x14);
            section.memoryBytes = Read32(entry + 0x18);
            section.align = Read32(entry + 0x1C);
            uint32_t expectedFlags = 0;
            if (!ExpectedFlags(section.type, expectedFlags)) return ContainerError::SectionType;
            if (section.flags != expectedFlags) return ContainerError::SectionFlags;
            if (section.align < 4 || section.align > 4096 || (section.align & (section.align - 1)) != 0) {
                return ContainerError::SectionAlign;
            }
            if (section.memoryOffset % kContainerPageBytes != 0) return ContainerError::SectionMemoryOffset;
            if (section.memoryBytes == 0 || section.memoryBytes % kContainerPageBytes != 0) return ContainerError::SectionMemoryBytes;
            if (static_cast<uint64_t>(section.memoryOffset) + section.memoryBytes > imageBytes) {
                return ContainerError::SectionMemoryRange;
            }
            if (section.type == static_cast<uint32_t>(ContainerSectionType::Bss)) {
                if (section.fileOffset != 0 || section.fileBytes != 0) return ContainerError::SectionFileBytes;
            } else {
                if (section.fileBytes == 0 || section.fileBytes > section.memoryBytes) return ContainerError::SectionFileBytes;
                if (section.fileOffset % 4 != 0) return ContainerError::SectionFileOffset;
                if (!RangeFits(section.fileOffset, section.fileBytes, totalBytes)) return ContainerError::SectionFileRange;
                addRegion(section.fileOffset, section.fileBytes);
            }
            const uint32_t nameOffset = Read32(entry + 0x00);
            if (nameOffset != 0) {
                if (stringTableBytes == 0 || nameOffset >= stringTableBytes) return ContainerError::SectionName;
                uint32_t nameLength = 0;
                // 字符串表末字节是 NUL，扫描一定在表内终止
                while (bytes[stringTableOffset + nameOffset + nameLength] != 0) ++nameLength;
                section.name = bytes + stringTableOffset + nameOffset;
                section.nameBytes = nameLength;
            }
        }
        // 段内存区间两两不重叠：读取方不依赖段表顺序
        for (uint32_t i = 0; i < sectionCount; ++i) {
            for (uint32_t j = i + 1; j < sectionCount; ++j) {
                const ContainerSection& first = view.sections[i];
                const ContainerSection& second = view.sections[j];
                const uint64_t firstEnd = static_cast<uint64_t>(first.memoryOffset) + first.memoryBytes;
                const uint64_t secondEnd = static_cast<uint64_t>(second.memoryOffset) + second.memoryBytes;
                if (first.memoryOffset < secondEnd && second.memoryOffset < firstEnd) return ContainerError::SectionMemoryOverlap;
            }
        }

        // —— 表项结构：槽位表与回填表的数量、对齐与文件区间（语义在重叠检查之后） ——
        for (uint32_t i = 0; i < exportCount; ++i) {
            const uint8_t* const entry = bytes + exportTableOffset + static_cast<uint64_t>(i) * kContainerExportEntryBytes;
            const uint16_t slotCount = Read16(entry + 0x12);
            if (slotCount < 1 || slotCount > kContainerMaxSlots) return ContainerError::ExportSlotCount;
            const uint32_t slotTableOffset = Read32(entry + 0x14);
            if (slotTableOffset % 4 != 0) return ContainerError::ExportSlotTableOffset;
            if (slotTableOffset < kContainerHeaderBytes || slotTableOffset >= totalBytes) return ContainerError::ExportSlotTableOffset;
            if (static_cast<uint64_t>(slotCount) * 4 > totalBytes - slotTableOffset) {
                return ContainerError::ExportSlotTableBounds;
            }
            addRegion(slotTableOffset, static_cast<uint64_t>(slotCount) * 4);
        }
        for (uint32_t i = 0; i < importCount; ++i) {
            const uint8_t* const entry = bytes + importTableOffset + static_cast<uint64_t>(i) * kContainerImportEntryBytes;
            const uint16_t slotCount = Read16(entry + 0x18);
            if (slotCount < 1 || slotCount > kContainerMaxSlots) return ContainerError::ImportSlotCount;
            const uint32_t slotPatchOffset = Read32(entry + 0x14);
            if (slotPatchOffset % 4 != 0) return ContainerError::ImportSlotTableOffset;
            if (slotPatchOffset < kContainerHeaderBytes || slotPatchOffset >= totalBytes) return ContainerError::ImportSlotTableOffset;
            if (static_cast<uint64_t>(slotCount) * 4 > totalBytes - slotPatchOffset) {
                return ContainerError::ImportSlotTableBounds;
            }
            addRegion(slotPatchOffset, static_cast<uint64_t>(slotCount) * 4);
        }

        // —— 表区、槽位表、回填表、字符串表与段文件区互不重叠 ——
        if (const ContainerError overlap = CheckRegions(regions, regionCount); overlap != ContainerError::None) return overlap;

        // —— 入口：Init 非零且落在 TEXT；Activate 与 Abort 允许为 0 ——
        view.imageBytes = imageBytes;
        view.bytes = bytes;
        view.bytesCount = bytesCount;
        view.entryInit = Read32(bytes + 0x18);
        view.entryActivate = Read32(bytes + 0x1C);
        view.entryAbort = Read32(bytes + 0x20);
        if (view.entryInit == 0) return ContainerError::EntryInit;
        if (!ContainsRange(view, view.entryInit, 1, static_cast<uint32_t>(ContainerSectionType::Text),
            static_cast<uint32_t>(ContainerSectionType::Text))) {
            return ContainerError::EntryInit;
        }
        if (view.entryActivate != 0 && !ContainsRange(view, view.entryActivate, 1,
            static_cast<uint32_t>(ContainerSectionType::Text), static_cast<uint32_t>(ContainerSectionType::Text))) {
            return ContainerError::EntryActivate;
        }
        if (view.entryAbort != 0 && !ContainsRange(view, view.entryAbort, 1,
            static_cast<uint32_t>(ContainerSectionType::Text), static_cast<uint32_t>(ContainerSectionType::Text))) {
            return ContainerError::EntryAbort;
        }

        // —— 导出表：标识、版本、保留字段、重复、槽位落在 TEXT ——
        view.exportCount = exportCount;
        for (uint32_t i = 0; i < exportCount; ++i) {
            const uint8_t* const entry = bytes + exportTableOffset + static_cast<uint64_t>(i) * kContainerExportEntryBytes;
            ContainerExport& out = view.exports[i];
            CopyId(out.interfaceId.bytes, entry);
            out.version = Read16(entry + 0x10);
            out.slotCount = Read16(entry + 0x12);
            out.slots = bytes + Read32(entry + 0x14);
            if (IsZeroBytes(entry, 16)) return ContainerError::ExportEntry;
            if (out.version == 0) return ContainerError::ExportEntry;
            if (Read32(entry + 0x18) != 0) return ContainerError::ExportEntry;
            for (uint32_t earlier = 0; earlier < i; ++earlier) {
                if (view.exports[earlier].version != out.version) continue;
                if (SameId(view.exports[earlier].interfaceId.bytes, out.interfaceId.bytes)) return ContainerError::ExportDuplicate;
            }
            for (uint32_t slot = 0; slot < out.slotCount; ++slot) {
                const uint32_t value = Read32(out.slots + static_cast<uint64_t>(slot) * 4);
                if (!ContainsRange(view, value, 1, static_cast<uint32_t>(ContainerSectionType::Text),
                    static_cast<uint32_t>(ContainerSectionType::Text))) {
                    return ContainerError::ExportSlot;
                }
            }
        }

        // —— 导入表：标识、版本区间、保留字段、重复、回填槽可写且 8 字节对齐 ——
        view.importCount = importCount;
        for (uint32_t i = 0; i < importCount; ++i) {
            const uint8_t* const entry = bytes + importTableOffset + static_cast<uint64_t>(i) * kContainerImportEntryBytes;
            ContainerImport& out = view.imports[i];
            CopyId(out.interfaceId.bytes, entry);
            out.minVersion = Read16(entry + 0x10);
            out.maxVersion = Read16(entry + 0x12);
            out.slotCount = Read16(entry + 0x18);
            out.patchSlots = bytes + Read32(entry + 0x14);
            if (IsZeroBytes(entry, 16)) return ContainerError::ImportEntry;
            if (out.minVersion == 0) return ContainerError::ImportEntry;
            if (out.maxVersion != kContainerVersionUnlimited && out.maxVersion < out.minVersion) return ContainerError::ImportEntry;
            if (Read16(entry + 0x1A) != 0) return ContainerError::ImportEntry;
            for (uint32_t earlier = 0; earlier < i; ++earlier) {
                if (SameId(view.imports[earlier].interfaceId.bytes, out.interfaceId.bytes)) return ContainerError::ImportDuplicate;
            }
            for (uint32_t slot = 0; slot < out.slotCount; ++slot) {
                const uint32_t value = Read32(out.patchSlots + static_cast<uint64_t>(slot) * 4);
                if (value % 8 != 0) return ContainerError::ImportSlot;
                if (!ContainsRange(view, value, 8, static_cast<uint32_t>(ContainerSectionType::Data),
                    static_cast<uint32_t>(ContainerSectionType::Bss))) {
                    return ContainerError::ImportSlot;
                }
            }
        }

        // —— 依赖表：标识非零且不是自依赖、主版本非零、Flags 为零、区间不颠倒、条目不重复 ——
        view.dependencyCount = dependencyCount;
        for (uint32_t i = 0; i < dependencyCount; ++i) {
            const uint8_t* const entry = bytes + dependencyTableOffset + static_cast<uint64_t>(i) * kContainerDependencyEntryBytes;
            ContainerDependency& out = view.dependencies[i];
            CopyId(out.moduleId.bytes, entry);
            out.minMajor = Read16(entry + 0x10);
            out.minMinor = Read16(entry + 0x12);
            out.minPatch = Read16(entry + 0x14);
            out.maxMajor = Read16(entry + 0x16);
            out.maxMinor = Read16(entry + 0x18);
            out.maxPatch = Read16(entry + 0x1A);
            if (IsZeroBytes(entry, 16)) return ContainerError::DependencyEntry;
            if (SameId(out.moduleId.bytes, view.moduleId.bytes)) return ContainerError::DependencyEntry;
            if (out.minMajor == 0) return ContainerError::DependencyEntry;
            if (Read32(entry + 0x1C) != 0) return ContainerError::DependencyEntry;
            if (out.maxMajor != kContainerMajorUnlimited) {
                const bool rangeOrdered = out.maxMajor > out.minMajor
                    || (out.maxMajor == out.minMajor && out.maxMinor > out.minMinor)
                    || (out.maxMajor == out.minMajor && out.maxMinor == out.minMinor && out.maxPatch >= out.minPatch);
                if (!rangeOrdered) return ContainerError::DependencyEntry;
            }
            for (uint32_t earlier = 0; earlier < i; ++earlier) {
                if (SameId(view.dependencies[earlier].moduleId.bytes, out.moduleId.bytes)) return ContainerError::DependencyDuplicate;
            }
        }

        // —— 重定位表：类型白名单、保留字段、目标可写且按宽度对齐（映射层再按索引展开） ——
        view.relocationCount = relocationCount;
        view.relocationTableOffset = relocationTableOffset;
        for (uint32_t i = 0; i < relocationCount; ++i) {
            const uint8_t* const entry = bytes + relocationTableOffset + static_cast<uint64_t>(i) * kContainerRelocationEntryBytes;
            const uint8_t type = entry[0];
            const uint32_t target = Read32(entry + 0x04);
            if (type != static_cast<uint8_t>(ContainerRelocationType::Relative64)
                && type != static_cast<uint8_t>(ContainerRelocationType::Relative32)) {
                return ContainerError::RelocationEntry;
            }
            if (entry[1] != 0 || Read16(entry + 0x02) != 0) return ContainerError::RelocationEntry;
            const uint64_t width = type == static_cast<uint8_t>(ContainerRelocationType::Relative64) ? 8 : 4;
            if (target % width != 0) return ContainerError::RelocationEntry;
            if (!ContainsRange(view, target, width, static_cast<uint32_t>(ContainerSectionType::Data),
                static_cast<uint32_t>(ContainerSectionType::Bss))) {
                return ContainerError::RelocationEntry;
            }
        }
        return ContainerError::None;
    }

    ContainerError ReadContainerRelocation(const ContainerView& view, uint32_t index, ContainerRelocation& relocation) {
        if (view.bytes == nullptr || index >= view.relocationCount) return ContainerError::RelocationEntry;
        const uint8_t* const entry = view.bytes + view.relocationTableOffset
            + static_cast<uint64_t>(index) * kContainerRelocationEntryBytes;
        relocation.type = entry[0];
        relocation.target = Read32(entry + 0x04);
        relocation.addend = Read64(entry + 0x08);
        return ContainerError::None;
    }

    bool ContainerExportSlot(const ContainerExport& entry, uint32_t slot, uint32_t& offset) {
        if (slot >= entry.slotCount || entry.slots == nullptr) return false;
        offset = Read32(entry.slots + static_cast<uint64_t>(slot) * 4);
        return true;
    }

    bool ContainerImportSlot(const ContainerImport& entry, uint32_t slot, uint32_t& offset) {
        if (slot >= entry.slotCount || entry.patchSlots == nullptr) return false;
        offset = Read32(entry.patchSlots + static_cast<uint64_t>(slot) * 4);
        return true;
    }

    const char* ContainerErrorText(ContainerError error) {
        switch (error) {
            case ContainerError::None: return "";
            case ContainerError::NullPointer: return "the module container pointer is null";
            case ContainerError::TooShort: return "the module container is shorter than its header";
            case ContainerError::HeaderMagic: return "the module container magic doesn't match";
            case ContainerError::HeaderVersion: return "the module container version isn't supported";
            case ContainerError::HeaderBytes: return "the module container header length isn't supported";
            case ContainerError::HeaderFlags: return "the module container flags aren't zero";
            case ContainerError::HeaderTotalBytes: return "the module container length doesn't match its TotalBytes";
            case ContainerError::HeaderImageBytes: return "the module container ImageBytes isn't a page multiple";
            case ContainerError::HeaderReserved: return "the module container reserved fields aren't zero";
            case ContainerError::SectionCount: return "the section count is out of range";
            case ContainerError::SectionTableOffset: return "the section table offset is unaligned or out of range";
            case ContainerError::SectionTableBounds: return "the section table runs past the container";
            case ContainerError::SectionEntryBytes: return "the section entry length isn't supported";
            case ContainerError::SectionType: return "a section type isn't supported";
            case ContainerError::SectionFlags: return "a section permission combination doesn't match its type";
            case ContainerError::SectionAlign: return "a section alignment isn't a power of two in range";
            case ContainerError::SectionMemoryOffset: return "a section memory offset isn't page aligned";
            case ContainerError::SectionMemoryBytes: return "a section memory span isn't a page multiple";
            case ContainerError::SectionMemoryRange: return "a section memory range exceeds ImageBytes";
            case ContainerError::SectionFileOffset: return "a section file offset isn't 4-byte aligned";
            case ContainerError::SectionFileBytes: return "a section file byte count is invalid";
            case ContainerError::SectionFileRange: return "a section file range is out of range";
            case ContainerError::SectionName: return "a section name offset is outside the string table";
            case ContainerError::SectionMemoryOverlap: return "two section memory ranges overlap";
            case ContainerError::RegionOverlap: return "container tables or section data overlap";
            case ContainerError::EntryInit: return "the Init entry is zero or outside a TEXT section";
            case ContainerError::EntryActivate: return "the Activate entry is outside a TEXT section";
            case ContainerError::EntryAbort: return "the Abort entry is outside a TEXT section";
            case ContainerError::ExportCount: return "the export count is beyond the limit";
            case ContainerError::ExportTableOffset: return "the export table offset is unaligned or out of range";
            case ContainerError::ExportTableBounds: return "the export table runs past the container";
            case ContainerError::ExportEntry: return "an export entry has invalid identity, version or reserved fields";
            case ContainerError::ExportSlotCount: return "an export slot count is out of range";
            case ContainerError::ExportSlotTableOffset: return "an export slot table offset is unaligned or out of range";
            case ContainerError::ExportSlotTableBounds: return "an export slot table runs past the container";
            case ContainerError::ExportSlot: return "an export slot isn't inside a TEXT section";
            case ContainerError::ExportDuplicate: return "two exports share the same interface and version";
            case ContainerError::ImportCount: return "the import count is beyond the limit";
            case ContainerError::ImportEntryBytes: return "the import entry length isn't supported";
            case ContainerError::ImportTableOffset: return "the import table offset is unaligned or out of range";
            case ContainerError::ImportTableBounds: return "the import table runs past the container";
            case ContainerError::ImportEntry: return "an import entry has an invalid identity, version range or reserved fields";
            case ContainerError::ImportSlotCount: return "an import slot count is out of range";
            case ContainerError::ImportSlotTableOffset: return "an import slot patch table offset is unaligned or out of range";
            case ContainerError::ImportSlotTableBounds: return "an import slot patch table runs past the container";
            case ContainerError::ImportSlot: return "an import patch slot isn't writable or isn't 8-byte aligned";
            case ContainerError::ImportDuplicate: return "two imports share the same interface";
            case ContainerError::DependencyCount: return "the dependency count is beyond the limit";
            case ContainerError::DependencyEntryBytes: return "the dependency entry length isn't supported";
            case ContainerError::DependencyTableOffset: return "the dependency table offset is unaligned or out of range";
            case ContainerError::DependencyTableBounds: return "the dependency table runs past the container";
            case ContainerError::DependencyEntry: return "a dependency entry has an invalid identity, major, flags or version range";
            case ContainerError::DependencyDuplicate: return "two dependencies share the same module identity";
            case ContainerError::RelocationCount: return "the relocation count is beyond the limit";
            case ContainerError::RelocationTableOffset: return "the relocation table offset is unaligned or out of range";
            case ContainerError::RelocationTableBounds: return "the relocation table runs past the container";
            case ContainerError::RelocationEntry: return "a relocation entry has an invalid type, reserved bytes or target";
            case ContainerError::StringTableOffset: return "the string table offset is out of range";
            case ContainerError::StringTableBounds: return "the string table runs past the container";
            case ContainerError::StringTableNul: return "the string table isn't NUL-terminated";
            case ContainerError::ModuleName: return "the module name offset or byte count is invalid";
            case ContainerError::ModuleId: return "the module identity is zero";
            case ContainerError::ModuleVersion: return "the module major version is zero";
            case ContainerError::KernelAbi: return "the kernel ABI minimum is zero";
        }
        return "the module container error is unknown";
    }
}
