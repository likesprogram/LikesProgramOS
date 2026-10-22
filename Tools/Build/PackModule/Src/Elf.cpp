/* Elf.cpp
    ELF64 模块镜像的读取与到容器输入的翻译

    读取：只接受 ET_DYN、64 位、小端、x86-64；节表、.symtab/.dynsym 与 SHT_RELA 重定位由本文件解析
    翻译：可分配节按 4 KiB 页分组（同页不得混权限），入口取 _Module_Init 等符号，导出槽表取
    _Contract_Export_<稳定名去点>_v<版本> 符号，导入回填区取 _Contract_Import_<稳定名去点>_v<版本>，
    动态重定位只接受 R_X86_64_RELATIVE；翻译结果交给 Osm.cpp 的 Build 写出
*/

#include <Elf.hpp>

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

    // 把值向上取整到 align 的整数倍
    std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) { return (value + align - 1) / align * align; }

    // 把值向下取整到 align 的整数倍
    std::uint64_t AlignDown(std::uint64_t value, std::uint64_t align) { return value / align * align; }

    // 去掉接口稳定名里的点，得到契约表符号里的接口名片段
    std::string StripDots(std::string_view name) {
        std::string stripped;
        stripped.reserve(name.size());
        for (const char character : name) {
            if (character != '.') stripped.push_back(character);
        }
        return stripped;
    }

    // 容器段类型的诊断名
    const char* SectionTypeName(std::uint32_t type) {
        switch (type) {
            case L::kTypeText: return "TEXT";
            case L::kTypeRodata: return "RODATA";
            case L::kTypeData: return "DATA";
            case L::kTypeBss: return "BSS";
            default: return "UNKNOWN";
        }
    }
}

namespace elf {
    File File::Parse(ByteView bytes) {
        File file;
        file.m_bytes = bytes;
        if (bytes.size() < 64) throw std::runtime_error("ELF file is shorter than the 64-byte header");
        if (Get8(bytes, 0) != 0x7F || Get8(bytes, 1) != 'E' || Get8(bytes, 2) != 'L' || Get8(bytes, 3) != 'F') {
            throw std::runtime_error("input is not an ELF file");
        }
        if (Get8(bytes, 4) != kClass64) throw std::runtime_error("ELF class is not 64-bit");
        if (Get8(bytes, 5) != kDataLittleEndian) throw std::runtime_error("ELF data encoding is not little-endian");
        if (Get8(bytes, 6) != 1) throw std::runtime_error("unsupported ELF identification version");
        if (Get16(bytes, 16) != kTypeDyn) throw std::runtime_error("ELF type is not ET_DYN (build the module as a shared object)");
        if (Get16(bytes, 18) != kMachineX86_64) throw std::runtime_error("ELF machine is not x86-64");
        if (Get32(bytes, 20) != 1) throw std::runtime_error("unsupported ELF object version");
        if (Get16(bytes, 52) != 64) throw std::runtime_error("ELF header size is not 64");
        if (Get16(bytes, 58) != 64) throw std::runtime_error("ELF section header entry size is not 64");
        const std::uint64_t sectionTableOffset = Get64(bytes, 40);
        const std::uint16_t sectionCount = Get16(bytes, 60);
        const std::uint16_t sectionNameIndex = Get16(bytes, 62);
        if (sectionCount == 0) throw std::runtime_error("ELF has no section headers (extended section counts are not supported)");
        if (sectionNameIndex >= sectionCount) throw std::runtime_error("ELF section name table index is out of range");
        if (sectionTableOffset > bytes.size()
            || static_cast<std::uint64_t>(sectionCount) * 64 > bytes.size() - sectionTableOffset) {
            throw std::runtime_error("ELF section header table is out of range");
        }

        // 原始节头：先读固定字段，再解析被引用的字符串表
        struct RawSection {
            std::uint32_t nameOffset = 0;    // 节名在字符串表里的偏移
            std::uint32_t type = 0;          // 节类型
            std::uint64_t flags = 0;         // 节标志
            std::uint64_t address = 0;       // 装载地址
            std::uint64_t offset = 0;        // 文件偏移
            std::uint64_t size = 0;          // 字节数
            std::uint32_t link = 0;          // 关联节下标
            std::uint64_t addralign = 0;     // 地址对齐
        };
        std::vector<RawSection> rawSections(sectionCount);
        for (std::uint16_t i = 0; i < sectionCount; ++i) {
            const std::uint64_t entry = sectionTableOffset + static_cast<std::uint64_t>(i) * 64;
            RawSection& section = rawSections[i];
            section.nameOffset = Get32(bytes, entry + 0);
            section.type = Get32(bytes, entry + 4);
            section.flags = Get64(bytes, entry + 8);
            section.address = Get64(bytes, entry + 16);
            section.offset = Get64(bytes, entry + 24);
            section.size = Get64(bytes, entry + 32);
            section.link = Get32(bytes, entry + 40);
            section.addralign = Get64(bytes, entry + 48);
        }
        // 取一个节的文件字节；NOBITS 与空节返回空视图，其余越界即失败
        auto sectionBytes = [&bytes](const RawSection& section, const std::string& name) -> ByteView {
            if (section.type == kSectionNobits || section.size == 0) return ByteView();
            if (section.offset > bytes.size() || section.size > bytes.size() - section.offset) {
                throw std::runtime_error("ELF section '" + name + "' file range is out of bounds");
            }
            return bytes.subspan(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size));
        };
        const RawSection& nameSection = rawSections[sectionNameIndex];
        if (nameSection.type != kSectionStrtab) throw std::runtime_error("ELF section name table is not a string table");
        const ByteView sectionNames = sectionBytes(nameSection, ".shstrtab");
        auto readCString = [](ByteView table, std::uint64_t offset, const char* what) -> std::string {
            if (offset >= table.size()) throw std::runtime_error(std::string("ELF ") + what + " name offset is out of range");
            std::size_t length = 0;
            while (table[static_cast<std::size_t>(offset) + length] != 0) ++length;
            return std::string(reinterpret_cast<const char*>(table.data() + offset), length);
        };
        file.m_sections.reserve(sectionCount);
        for (std::uint16_t i = 0; i < sectionCount; ++i) {
            const RawSection& raw = rawSections[i];
            Section section;
            section.name = readCString(sectionNames, raw.nameOffset, "section");
            section.type = raw.type;
            section.flags = raw.flags;
            section.address = raw.address;
            section.offset = raw.offset;
            section.size = raw.size;
            section.link = raw.link;
            section.addralign = raw.addralign;
            file.m_sections.push_back(std::move(section));
        }

        // 符号表：.symtab 与 .dynsym 都读；未定义外部符号单独记录，供 nm -u 空检查
        for (std::size_t i = 0; i < file.m_sections.size(); ++i) {
            const Section& table = file.m_sections[i];
            if (table.type != kSectionSymtab && table.type != kSectionDynsym) continue;
            if (table.link >= file.m_sections.size()) throw std::runtime_error("ELF symbol table '" + table.name + "' has an out-of-range string table");
            const Section& strings = file.m_sections[table.link];
            if (strings.type != kSectionStrtab) throw std::runtime_error("ELF symbol table '" + table.name + "' does not link a string table");
            if (table.size % 24 != 0) throw std::runtime_error("ELF symbol table '" + table.name + "' entry size is not 24");
            const ByteView content = sectionBytes(rawSections[i], table.name);
            const ByteView stringBytes = sectionBytes(rawSections[table.link], strings.name);
            for (std::uint64_t offset = 0; offset < content.size(); offset += 24) {
                Symbol symbol;
                symbol.name = readCString(stringBytes, Get32(content, offset), "symbol");
                symbol.info = Get8(content, offset + 4);
                symbol.sectionIndex = Get16(content, offset + 6);
                symbol.value = Get64(content, offset + 8);
                symbol.size = Get64(content, offset + 16);
                if (symbol.sectionIndex == 0 && !symbol.name.empty()
                    && (symbol.info & 0x0F) != kSymbolFile) {
                    file.m_undefined.push_back(symbol.name);
                }
                file.m_symbols.push_back(std::move(symbol));
            }
        }

        // 动态重定位：只读 SHT_RELA；静态重定位表（SHT_REL）与符号引用型重定位都不接受
        for (std::size_t i = 0; i < file.m_sections.size(); ++i) {
            const Section& section = file.m_sections[i];
            if (section.type == 9 && section.size > 0) throw std::runtime_error("ELF uses SHT_REL relocations; only SHT_RELA is supported");
            if (section.type != kSectionRela) continue;
            if (section.size % 24 != 0) throw std::runtime_error("ELF relocation section '" + section.name + "' entry size is not 24");
            const ByteView content = sectionBytes(rawSections[i], section.name);
            for (std::uint64_t offset = 0; offset < content.size(); offset += 24) {
                const std::uint64_t target = Get64(content, offset);
                const std::uint64_t info = Get64(content, offset + 8);
                const std::uint32_t type = static_cast<std::uint32_t>(info & 0xFFFFFFFFu);
                const std::uint32_t symbolIndex = static_cast<std::uint32_t>(info >> 32);
                if (type == kRelocation32 || type == kRelocation32S) {
                    throw std::runtime_error("ELF relocation type " + std::to_string(type) + " in '" + section.name
                        + "' is an absolute 32-bit relocation; the first batch only allows R_X86_64_RELATIVE");
                }
                if (symbolIndex != 0) {
                    throw std::runtime_error("ELF relocation in '" + section.name + "' references a symbol; only relative relocations are allowed");
                }
                if (type != kRelocationRelative) {
                    throw std::runtime_error("ELF relocation type " + std::to_string(type)
                        + " in '" + section.name + "'; only R_X86_64_RELATIVE is allowed");
                }
                file.m_relocations.push_back({ type, target, Get64(content, offset + 16) });
            }
        }
        return file;
    }

    const Section* File::FindSection(std::string_view name) const {
        for (const Section& section : m_sections) {
            if (section.name == name) return &section;
        }
        return nullptr;
    }

    const Symbol* File::FindSymbol(std::string_view name) const {
        // 先找已定义的符号，避免未定义引用遮蔽定义
        for (const Symbol& symbol : m_symbols) {
            if (symbol.name == name && symbol.sectionIndex != 0) return &symbol;
        }
        for (const Symbol& symbol : m_symbols) {
            if (symbol.name == name) return &symbol;
        }
        return nullptr;
    }

    const std::vector<Section>& File::Sections() const { return m_sections; }

    const std::vector<Relocation>& File::Relocations() const { return m_relocations; }

    const std::vector<std::string>& File::UndefinedSymbols() const { return m_undefined; }

    bool File::ReadMemory(std::uint64_t address, std::span<std::uint8_t> output) const {
        for (const Section& section : m_sections) {
            if ((section.flags & kSectionFlagAlloc) == 0 || section.type != kSectionProgbits) continue;
            if (address < section.address) continue;
            if (section.size < output.size() || address - section.address > section.size - output.size()) continue;
            const std::uint64_t fileOffset = section.offset + (address - section.address);
            if (fileOffset > m_bytes.size() || output.size() > m_bytes.size() - fileOffset) continue;
            std::memcpy(output.data(), m_bytes.data() + fileOffset, output.size());
            return true;
        }
        return false;
    }

    osm::Spec MakeSpec(const File& file, const PackInput& input) {
        osm::Spec spec;
        spec.moduleName = input.moduleName;
        spec.versionMajor = input.versionMajor;
        spec.versionMinor = input.versionMinor;
        spec.versionPatch = input.versionPatch;
        spec.abiMin = input.abiMin;
        spec.abiMax = input.abiMax;
        const osm::ModuleId selfId = osm::DeriveModuleId(input.moduleName);
        // 契约表符号缺失时，生成头还没接入的构建会停在这里
        if (!file.UndefinedSymbols().empty()) {
            throw std::runtime_error("ELF has undefined symbols (nm -u must be empty): " + file.UndefinedSymbols().front());
        }

        // 选择写进容器的节：可分配、有内容、不是链接器元数据；静态构造表必须为空
        struct Group {
            std::uint64_t pageBegin = 0;        // 起始页边界
            std::uint64_t pageEnd = 0;          // 结束页边界（不含）
            std::uint32_t type = 0;             // 容器段类型
            std::vector<const Section*> members; // 落在这一组页里的 ELF 节
        };
        std::vector<const Section*> included;
        for (const Section& section : file.Sections()) {
            if ((section.flags & kSectionFlagAlloc) == 0) continue;
            if (section.type == kSectionInitArray || section.type == kSectionFiniArray || section.type == kSectionPreinitArray) {
                if (section.size > 0) throw std::runtime_error("ELF section '" + section.name + "' is a non-empty static constructor table");
                continue;
            }
            if (section.type != kSectionProgbits && section.type != kSectionNobits) continue;
            if ((section.flags & kSectionFlagTls) != 0) throw std::runtime_error("ELF section '" + section.name + "' is TLS; TLS is not allowed");
            if ((section.flags & kSectionFlagExec) != 0 && (section.flags & kSectionFlagWrite) != 0) {
                throw std::runtime_error("ELF section '" + section.name + "' is writable and executable; W+X sections are not allowed");
            }
            if (section.name.rfind(".note", 0) == 0 || section.name == ".interp") continue;
            if (section.size == 0) continue;
            included.push_back(&section);
        }
        if (included.empty()) throw std::runtime_error("ELF has no allocatable sections with content");
        std::sort(included.begin(), included.end(), [](const Section* a, const Section* b) { return a->address < b->address; });
        for (std::size_t i = 1; i < included.size(); ++i) {
            if (included[i - 1]->address + included[i - 1]->size > included[i]->address) {
                throw std::runtime_error("ELF sections '" + included[i - 1]->name + "' and '" + included[i]->name + "' overlap in memory");
            }
        }
        // 按节标志分类；TEXT 必须有执行位，BSS 是 NOBITS，其余按可写位分 RODATA/DATA
        auto classify = [](const Section& section) -> std::uint32_t {
            if ((section.flags & kSectionFlagExec) != 0) return L::kTypeText;
            if (section.type == kSectionNobits) return L::kTypeBss;
            if ((section.flags & kSectionFlagWrite) != 0) return L::kTypeData;
            return L::kTypeRodata;
        };
        // 同页节合并成一组；同页出现不同权限即失败
        std::vector<Group> groups;
        for (const Section* section : included) {
            const std::uint32_t type = classify(*section);
            const std::uint64_t pageBegin = AlignDown(section->address, L::kPageBytes);
            const std::uint64_t pageEnd = AlignUp(section->address + section->size, L::kPageBytes);
            if (groups.empty() || pageBegin >= groups.back().pageEnd) {
                Group group;
                group.pageBegin = pageBegin;
                group.pageEnd = pageEnd;
                group.type = type;
                group.members.push_back(section);
                groups.push_back(std::move(group));
                continue;
            }
            const std::uint32_t current = groups.back().type;
            // 允许同权限合并，也允许 DATA 组尾部并入 BSS（同一页里 DATA 之后再出现零初始化数据）
            const bool mergeable = current == type || (current == L::kTypeData && type == L::kTypeBss);
            if (!mergeable) {
                throw std::runtime_error("ELF sections '" + groups.back().members.back()->name + "' and '" + section->name
                    + "' share a 4 KiB page but map to " + SectionTypeName(current) + " and " + SectionTypeName(type)
                    + "; link the module with -z separate-code and 4 KiB pages");
            }
            groups.back().pageEnd = std::max(groups.back().pageEnd, pageEnd);
            groups.back().members.push_back(section);
        }
        if (groups.size() > L::kMaxSectionCount) {
            throw std::runtime_error("ELF maps to " + std::to_string(groups.size())
                + " container sections, more than the limit of 16; merge code or data sections in the linker script");
        }
        // 生成容器段：内存偏移保持 ELF 地址，代码里的相对寻址与重定位加数才成立
        for (const Group& group : groups) {
            if (group.pageBegin > 0xFFFFFFFF || group.pageEnd > 0xFFFFFFFF) {
                throw std::runtime_error("ELF image address space exceeds the 32-bit container offsets");
            }
            osm::SectionSpec section;
            section.name = group.members.front()->name;
            section.type = group.type;
            switch (group.type) {
                case L::kTypeText: section.flags = L::kFlagRead | L::kFlagExecute; break;
                case L::kTypeRodata: section.flags = L::kFlagRead; break;
                default: section.flags = L::kFlagRead | L::kFlagWrite; break;
            }
            section.memoryOffset = static_cast<std::uint32_t>(group.pageBegin);
            section.memoryBytes = static_cast<std::uint32_t>(group.pageEnd - group.pageBegin);
            section.align = L::kPageBytes;
            if (group.type != L::kTypeBss) {
                // 非 BSS 段按整页落盘：节间空洞与尾部 BSS 都以零字节编码，装载后不产生未初始化尾部
                osm::Bytes data(section.memoryBytes, 0);
                for (const Section* member : group.members) {
                    if (member->type == kSectionNobits) continue;
                    const std::size_t begin = static_cast<std::size_t>(member->address - group.pageBegin);
                    if (!file.ReadMemory(member->address, std::span<std::uint8_t>(data.data() + begin, static_cast<std::size_t>(member->size)))) {
                        throw std::runtime_error("cannot read ELF section '" + member->name + "' bytes");
                    }
                }
                section.fileBytes = std::move(data);
            }
            spec.sections.push_back(std::move(section));
        }
        // 在容器段里按地址找组：typeA/typeB 给出允许的段类型
        auto findGroup = [&groups](std::uint64_t begin, std::uint64_t width, std::uint32_t typeA, std::uint32_t typeB) -> const Group* {
            for (const Group& group : groups) {
                if (group.type != typeA && group.type != typeB) continue;
                if (begin < group.pageBegin) continue;
                if (begin + width <= group.pageEnd) return &group;
            }
            return nullptr;
        };
        auto requireSymbol = [&file](const char* name, bool required, const char* what) -> std::uint32_t {
            const Symbol* symbol = file.FindSymbol(name);
            if (symbol == nullptr) {
                if (!required) return 0;
                throw std::runtime_error(std::string("ELF symbol ") + name + " is missing (" + what + "; the module must not be stripped)");
            }
            if (symbol->sectionIndex == 0) throw std::runtime_error(std::string("ELF symbol ") + name + " is undefined");
            if (symbol->value > 0xFFFFFFFF) throw std::runtime_error(std::string("ELF symbol ") + name + " offset exceeds 32 bits");
            return static_cast<std::uint32_t>(symbol->value);
        };
        // 入口：Init 必须有且非零，Activate/Abort 缺省或为零表示没有
        spec.entryInit = requireSymbol(kEntryInitSymbol, true, "module entry");
        if (spec.entryInit == 0) throw std::runtime_error("ELF symbol _Module_Init is at offset zero; EntryInit must be nonzero");
        spec.entryActivate = requireSymbol(kEntryActivateSymbol, false, "activate entry");
        spec.entryAbort = requireSymbol(kEntryAbortSymbol, false, "abort entry");
        auto requireText = [&findGroup](std::uint32_t offset, const char* what) {
            if (findGroup(offset, 1, L::kTypeText, L::kTypeText) == nullptr) {
                throw std::runtime_error(std::string(what) + " is not inside a TEXT segment");
            }
        };
        requireText(spec.entryInit, "_Module_Init");
        if (spec.entryActivate != 0) requireText(spec.entryActivate, "_Module_Activate");
        if (spec.entryAbort != 0) requireText(spec.entryAbort, "_Module_Abort");

        // 重定位：目标必须在可写段内、按 8 字节对齐；Addend 原样带进容器
        for (const Relocation& relocation : file.Relocations()) {
            if (relocation.offset > 0xFFFFFFFF) throw std::runtime_error("ELF relocation target exceeds 32 bits");
            if (relocation.offset % 8 != 0) throw std::runtime_error("ELF relocation target is not 8-byte aligned");
            if (findGroup(relocation.offset, 8, L::kTypeData, L::kTypeBss) == nullptr) {
                throw std::runtime_error("ELF relocation target is not inside a writable segment");
            }
            spec.relocations.push_back({ L::kRelocationRelative64, static_cast<std::uint32_t>(relocation.offset), relocation.addend });
        }
        // 重定位按目标地址建索引，导出槽表的槽值从加数里取（指针表在装载时才被修正）
        std::map<std::uint64_t, std::uint64_t> relocated;
        for (const Relocation& relocation : file.Relocations()) {
            if (!relocated.emplace(relocation.offset, relocation.addend).second) {
                throw std::runtime_error("ELF has duplicate relocations at the same target");
            }
        }

        // 导出：槽表符号给出各槽的代码偏移，槽表所在页必须在可写段（指针表靠重定位填写）
        // 唯一性口径与 Osm.cpp 的 verify 一致：同 InterfaceId + Version 才重复
        std::vector<std::pair<osm::InterfaceId, std::uint16_t>> exportIds;
        for (const ExportDecl& declaration : input.exports) {
            if (declaration.version == 0) throw std::runtime_error("export version is zero: " + declaration.stableName);
            if (declaration.slotCount < L::kMinSlotCount || declaration.slotCount > L::kMaxSlotCount) {
                throw std::runtime_error("export slot count for " + declaration.stableName + " is outside 1..64");
            }
            const osm::InterfaceId interfaceId = osm::DeriveInterfaceId(declaration.stableName);
            for (const std::pair<osm::InterfaceId, std::uint16_t>& earlier : exportIds) {
                if (earlier.first == interfaceId && earlier.second == declaration.version) {
                    throw std::runtime_error("duplicate export for interface: " + declaration.stableName);
                }
            }
            exportIds.push_back(std::make_pair(interfaceId, declaration.version));
            const std::string symbolName = std::string(kExportSymbolPrefix) + StripDots(declaration.stableName)
                + "_v" + std::to_string(declaration.version);
            const Symbol* symbol = file.FindSymbol(symbolName);
            if (symbol == nullptr) {
                throw std::runtime_error("ELF symbol " + symbolName + " is missing for export " + declaration.stableName);
            }
            if (symbol->sectionIndex == 0) throw std::runtime_error("ELF symbol " + symbolName + " is undefined");
            const std::uint64_t slotTableBytes = static_cast<std::uint64_t>(declaration.slotCount) * 8;
            if (findGroup(symbol->value, slotTableBytes, L::kTypeData, L::kTypeBss) == nullptr) {
                throw std::runtime_error("export slot table " + symbolName + " is not inside a writable segment");
            }
            osm::ExportSpec exportSpec;
            exportSpec.stableName = declaration.stableName;
            exportSpec.version = declaration.version;
            exportSpec.slots.reserve(declaration.slotCount);
            for (std::uint16_t slot = 0; slot < declaration.slotCount; ++slot) {
                const std::uint64_t location = symbol->value + static_cast<std::uint64_t>(slot) * 8;
                const auto relocation = relocated.find(location);
                std::uint64_t value = 0;
                if (relocation != relocated.end()) {
                    value = relocation->second;
                } else {
                    std::uint8_t raw[8] = {};
                    if (!file.ReadMemory(location, raw)) {
                        throw std::runtime_error("export slot " + std::to_string(slot) + " of " + declaration.stableName
                            + " has neither an initial value nor a relative relocation");
                    }
                    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(raw[i]) << (8 * i);
                }
                if (value > 0xFFFFFFFF) throw std::runtime_error("export slot value exceeds 32 bits");
                if (findGroup(value, 1, L::kTypeText, L::kTypeText) == nullptr) {
                    throw std::runtime_error("export slot " + std::to_string(slot) + " of " + declaration.stableName
                        + " is not inside a TEXT segment");
                }
                exportSpec.slots.push_back(static_cast<std::uint32_t>(value));
            }
            spec.exports.push_back(std::move(exportSpec));
        }

        // 导入：回填区符号给出函数指针槽的内存偏移；槽必须可写且 8 字节对齐
        std::vector<osm::InterfaceId> importIds;
        for (const ImportDecl& declaration : input.imports) {
            if (declaration.minVersion == 0) throw std::runtime_error("import MinVersion is zero: " + declaration.stableName);
            if (declaration.maxVersion != L::kVersionUnlimited && declaration.maxVersion < declaration.minVersion) {
                throw std::runtime_error("import version range is reversed: " + declaration.stableName);
            }
            if (declaration.slotCount < L::kMinSlotCount || declaration.slotCount > L::kMaxSlotCount) {
                throw std::runtime_error("import slot count for " + declaration.stableName + " is outside 1..64");
            }
            const osm::InterfaceId interfaceId = osm::DeriveInterfaceId(declaration.stableName);
            for (const osm::InterfaceId& earlier : importIds) {
                if (earlier == interfaceId) throw std::runtime_error("duplicate import for interface: " + declaration.stableName);
            }
            importIds.push_back(interfaceId);
            // 回填区符号的版本：显式版本优先，其次 MaxVersion，最后 MinVersion
            std::vector<std::uint16_t> versions;
            if (declaration.symbolVersion != 0) versions.push_back(declaration.symbolVersion);
            if (declaration.maxVersion != L::kVersionUnlimited) versions.push_back(declaration.maxVersion);
            versions.push_back(declaration.minVersion);
            const Symbol* symbol = nullptr;
            std::string symbolName;
            for (const std::uint16_t version : versions) {
                symbolName = std::string(kImportSymbolPrefix) + StripDots(declaration.stableName) + "_v" + std::to_string(version);
                symbol = file.FindSymbol(symbolName);
                if (symbol != nullptr) break;
            }
            if (symbol == nullptr) {
                throw std::runtime_error("ELF symbol " + std::string(kImportSymbolPrefix) + StripDots(declaration.stableName)
                    + "_v<version> is missing for import " + declaration.stableName);
            }
            if (symbol->sectionIndex == 0) throw std::runtime_error("ELF symbol " + symbolName + " is undefined");
            const std::uint64_t slotsBytes = static_cast<std::uint64_t>(declaration.slotCount) * 8;
            if (symbol->value % 8 != 0) throw std::runtime_error("import slot area " + symbolName + " is not 8-byte aligned");
            if (findGroup(symbol->value, slotsBytes, L::kTypeData, L::kTypeBss) == nullptr) {
                throw std::runtime_error("import slot area " + symbolName + " is not inside a writable segment");
            }
            osm::ImportSpec importSpec;
            importSpec.stableName = declaration.stableName;
            importSpec.minVersion = declaration.minVersion;
            importSpec.maxVersion = declaration.maxVersion;
            importSpec.patchSlots.reserve(declaration.slotCount);
            for (std::uint16_t slot = 0; slot < declaration.slotCount; ++slot) {
                const std::uint64_t location = symbol->value + static_cast<std::uint64_t>(slot) * 8;
                importSpec.patchSlots.push_back(static_cast<std::uint32_t>(location));
            }
            spec.imports.push_back(std::move(importSpec));
        }

        // 依赖：逻辑名派生 ModuleId；版本区间按 CLI 的 major 范围展开为闭区间
        std::vector<osm::ModuleId> dependencyIds;
        for (const DependencyDecl& declaration : input.dependencies) {
            const osm::ModuleId moduleId = osm::DeriveModuleId(declaration.logicalName);
            if (declaration.minMajor == 0) throw std::runtime_error("dependency MinMajor is zero: " + declaration.logicalName);
            if (declaration.maxMajor != L::kMajorUnlimited && declaration.maxMajor < declaration.minMajor) {
                throw std::runtime_error("dependency MaxMajor is below MinMajor: " + declaration.logicalName);
            }
            if (moduleId == selfId) throw std::runtime_error("module cannot depend on itself: " + declaration.logicalName);
            for (const osm::ModuleId& earlier : dependencyIds) {
                if (earlier == moduleId) throw std::runtime_error("duplicate dependency: " + declaration.logicalName);
            }
            dependencyIds.push_back(moduleId);
            osm::DependencySpec dependency;
            dependency.logicalName = declaration.logicalName;
            dependency.minMajor = declaration.minMajor;
            dependency.minMinor = 0;
            dependency.minPatch = 0;
            if (declaration.maxMajor == L::kMajorUnlimited) {
                dependency.maxMajor = 0;
                dependency.maxMinor = 0;
                dependency.maxPatch = 0;
            } else {
                dependency.maxMajor = declaration.maxMajor;
                dependency.maxMinor = 0xFFFF;
                dependency.maxPatch = 0xFFFF;
            }
            spec.dependencies.push_back(std::move(dependency));
        }
        return spec;
    }
}
