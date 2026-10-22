/* Handoff.cpp
    Handoff 的实现：按 9 步核对交接包，失败只回报编号与原因文本

    本文件不做打印与停机，也不做摘要复算与签名验证；所有读取都经有界视图与固定偏移，
    清单 / 列表的解析在 ManifestView 与 ParseBootList 内，本文件只做交叉核对与身份判定
*/

#include <Handoff.hpp>

#include <Libc.hpp>
#include <ModulePath.hpp>
#include <Paging.hpp>
#include <Sha256.hpp>

// 生成的引导闭包期望集头（MakeContract 产物）用相对路径引入：内核 Core/Makefile 当前没有
// Contract 目录的 -I，与 Packages/Baleen/Core/Src/BootClosure.cpp 同一处理；接入 -I 后
// 可改写成 <BootClosure.hpp>，本文件不改搜索路径也能编译
#include "../../../Common/Out/Contract/BootClosure.hpp"

namespace {
    // 记录数组头字段偏移，相对数组起点
    constexpr uint32_t kHeaderCount = 0x00;
    constexpr uint32_t kHeaderReserved = 0x04;
    // 记录内字段偏移，相对记录起点
    constexpr uint32_t kRecordPhys = 0x00;
    constexpr uint32_t kRecordBytes = 0x08;
    constexpr uint32_t kRecordArtifactIndex = 0x0C;
    constexpr uint32_t kRecordReserved = 0x0E;

    // 列表固定头的字面量与长度
    constexpr char kListMagic[] = "LPML 1";
    constexpr uint32_t kListMagicBytes = 6;
    constexpr char kListMajorPrefix[] = "major ";
    constexpr uint32_t kListMajorPrefixBytes = 6;
    constexpr char kListRevisionPrefix[] = "revision ";
    constexpr uint32_t kListRevisionPrefixBytes = 9;
    constexpr char kListModulePrefix[] = "module ";
    constexpr uint32_t kListModulePrefixBytes = 7;
    constexpr char kModulesPrefix[] = "Modules/";
    constexpr uint32_t kModulesPrefixBytes = 8;

    // 按小端读取 16 位
    uint16_t Read16(const uint8_t* field) {
        return static_cast<uint16_t>(field[0] | (field[1] << 8));
    }

    // 按小端读取 32 位
    uint32_t Read32(const uint8_t* field) {
        return static_cast<uint32_t>(field[0]) | (static_cast<uint32_t>(field[1]) << 8) |
               (static_cast<uint32_t>(field[2]) << 16) | (static_cast<uint32_t>(field[3]) << 24);
    }

    // 按小端读取 64 位
    uint64_t Read64(const uint8_t* field) {
        return static_cast<uint64_t>(Read32(field)) | (static_cast<uint64_t>(Read32(field + 4)) << 32);
    }

    // 物理区间是否可用：非零、整段落在低 4GiB 直映射窗口内、64 位加法不回绕
    bool RangeValid(uint64_t physical, uint64_t bytes) {
        if (physical == 0 || bytes == 0) return false;
        if (physical >= Kernel::kMapLimitBytes) return false;
        return bytes <= Kernel::kMapLimitBytes - physical;
    }

    // 读一行：从游标起找到 '\n'，交出不含换行的行内容并推进游标；没有换行返回 false
    bool ReadLine(const uint8_t* bytes, uint32_t length, uint32_t& cursor, const uint8_t*& line, uint32_t& lineBytes) {
        if (cursor >= length) return false;
        uint32_t end = cursor;
        while (end < length && bytes[end] != '\n') ++end;
        if (end >= length) return false;
        line = bytes + cursor;
        lineBytes = end - cursor;
        cursor = end + 1;
        return true;
    }

    // 十进制解析：从 text 起读连续数字，超过 limit 即失败；used 回传消耗的字节数
    bool ParseDecimal(const uint8_t* text, uint32_t bytes, uint64_t limit, uint64_t& value, uint32_t& used) {
        uint64_t result = 0;
        uint32_t index = 0;
        while (index < bytes && text[index] >= '0' && text[index] <= '9') {
            if (result > limit / 10) return false;
            result = result * 10 + (text[index] - '0');
            if (result > limit) return false;
            ++index;
        }
        if (index == 0) return false;
        value = result;
        used = index;
        return true;
    }
}

namespace Kernel {
    const uint8_t* MapPhysical(uint64_t physical, uint64_t bytes) {
        if (!RangeValid(physical, bytes)) return nullptr;
        return static_cast<const uint8_t*>(PhysToVirt(physical));
    }

    const char* ParseBootList(const uint8_t* bytes, uint32_t length, BootListView& view) {
        view = BootListView{};
        if (bytes == nullptr) return "the module load list copy is null";
        if (length == 0 || length > kHandoffListMaxBytes) return "the module load list length is out of range";

        uint32_t cursor = 0;
        const uint8_t* line = nullptr;
        uint32_t lineBytes = 0;
        if (!ReadLine(bytes, length, cursor, line, lineBytes)) return "the module load list is truncated";
        if (lineBytes != kListMagicBytes || memcmp(line, kListMagic, kListMagicBytes) != 0) return "the module load list magic doesn't match";

        if (!ReadLine(bytes, length, cursor, line, lineBytes)) return "the module load list is truncated";
        if (lineBytes <= kListMajorPrefixBytes || memcmp(line, kListMajorPrefix, kListMajorPrefixBytes) != 0) return "the module load list major line is missing";
        uint64_t major = 0;
        uint32_t used = 0;
        if (!ParseDecimal(line + kListMajorPrefixBytes, lineBytes - kListMajorPrefixBytes, 0xFFFF, major, used) || used != lineBytes - kListMajorPrefixBytes || major == 0)
            return "the module load list major is invalid";

        if (!ReadLine(bytes, length, cursor, line, lineBytes)) return "the module load list is truncated";
        if (lineBytes <= kListRevisionPrefixBytes || memcmp(line, kListRevisionPrefix, kListRevisionPrefixBytes) != 0) return "the module load list revision line is missing";
        uint64_t revision = 0;
        if (!ParseDecimal(line + kListRevisionPrefixBytes, lineBytes - kListRevisionPrefixBytes, 0xFFFFFFFFFFFFFFFFull, revision, used) || used != lineBytes - kListRevisionPrefixBytes || revision == 0)
            return "the module load list revision is invalid";

        view.major = static_cast<uint16_t>(major);
        view.revision = revision;
        while (cursor < length) {
            if (!ReadLine(bytes, length, cursor, line, lineBytes)) return "the module load list is truncated";
            if (lineBytes <= kListModulePrefixBytes || memcmp(line, kListModulePrefix, kListModulePrefixBytes) != 0) return "the module load list entry isn't a module line";
            const uint8_t* const path = line + kListModulePrefixBytes;
            const uint32_t pathBytes = lineBytes - kListModulePrefixBytes;
            if (view.count >= kHandoffMaxRecords) return "the module load list has too many entries";
            if (pathBytes > KernelSet::ManifestLayout::kEntryPathMaxBytes) return "the module load list entry path length is out of range";
            if (!PathValid(path, pathBytes)) return "the module load list entry path is invalid";
            if (pathBytes <= kModulesPrefixBytes || memcmp(path, kModulesPrefix, kModulesPrefixBytes) != 0) return "the module load list entry isn't under Modules/";
            for (uint32_t i = 0; i < view.count; ++i) {
                if (view.pathBytes[i] != pathBytes) continue;
                if (memcmp(view.paths[i], path, pathBytes) == 0) return "the module load list entry path is duplicated";
            }
            view.paths[view.count] = path;
            view.pathBytes[view.count] = static_cast<uint16_t>(pathBytes);
            ++view.count;
        }
        return nullptr;
    }

    Diagnostics::Diagnostic CheckHandoff(const Boot::Info& info, const uint8_t osBuildId[32], uint16_t osModuleMajor, HandoffFacts& facts, const char** reason, PhysicalMap map) {
        facts = HandoffFacts{};
        if (reason != nullptr) *reason = nullptr;
        if (map == nullptr) {
            if (reason != nullptr) *reason = "no physical memory reader is installed";
            return Diagnostics::kFailHandoffMissing;
        }

        // —— 第 2 步：存在性一致：三类缓冲的地址与长度都要给出，缺一项即交接包缺失 ——
        if (info.kernelSetManifest == 0 || info.kernelSetManifestBytes == 0) return Diagnostics::kFailHandoffMissing;
        if (info.bootList == 0 || info.bootListBytes == 0) return Diagnostics::kFailHandoffMissing;
        if (info.bootModules == 0) return Diagnostics::kFailHandoffMissing;

        // —— 第 3 步：越界与溢出：先查区间回绕，再取可读指针 ——
        if (!RangeValid(info.kernelSetManifest, info.kernelSetManifestBytes)) return Diagnostics::kFailManifestCopy;
        if (info.kernelSetManifestBytes > KernelSet::ManifestLayout::kManifestMaxBytes) return Diagnostics::kFailManifestCopy;
        if (!RangeValid(info.bootList, info.bootListBytes) || info.bootListBytes > kHandoffListMaxBytes) return Diagnostics::kFailHandoffMissing;

        const uint8_t* const header = map(info.bootModules, kHandoffHeaderBytes);
        if (header == nullptr) return Diagnostics::kFailPreloadRecords;
        const uint32_t recordCount = Read32(header + kHeaderCount);
        if (recordCount == 0) return Diagnostics::kFailHandoffMissing;
        if (recordCount > kHandoffMaxRecords) {
            if (reason != nullptr) *reason = "the preload record count is beyond the limit";
            return Diagnostics::kFailPreloadRecords;
        }
        if (Read32(header + kHeaderReserved) != 0) {
            if (reason != nullptr) *reason = "the preload record array reserved field isn't zero";
            return Diagnostics::kFailPreloadRecords;
        }
        const uint32_t recordsBytes = kHandoffHeaderBytes + recordCount * kHandoffRecordBytes;
        if (!RangeValid(info.bootModules, recordsBytes)) return Diagnostics::kFailPreloadRecords;

        const uint8_t* const manifest = map(info.kernelSetManifest, info.kernelSetManifestBytes);
        if (manifest == nullptr) return Diagnostics::kFailManifestCopy;
        const uint8_t* const list = map(info.bootList, info.bootListBytes);
        if (list == nullptr) return Diagnostics::kFailHandoffMissing;
        const uint8_t* const records = map(info.bootModules, recordsBytes);
        if (records == nullptr) return Diagnostics::kFailPreloadRecords;

        // —— 第 4 步：清单副本结构与列表副本结构（有界解析，失败只回报原因文本） ——
        if (const char* text = ParseManifest(manifest, info.kernelSetManifestBytes, facts.manifest); text != nullptr) {
            if (reason != nullptr) *reason = text;
            return Diagnostics::kFailManifestFormat;
        }
        if (const char* text = ParseBootList(list, info.bootListBytes, facts.list); text != nullptr) {
            if (reason != nullptr) *reason = text;
            return Diagnostics::kFailBootClosure;
        }
        // 交接的列表必须是清单 Type=3 条目描述的那一份：长度与清单登记值一致
        // 解析器已保证恰好一条 Type=3 且路径为 Modules.list，这里只核对字节数
        {
            static const uint8_t kListPath[] = "Modules.list";
            const ManifestEntry* const listEntry = FindManifestEntry(facts.manifest, kListPath, sizeof(kListPath) - 1);
            if (listEntry == nullptr || listEntry->dataBytes != info.bootListBytes) {
                if (reason != nullptr) *reason = "the module load list length doesn't match the manifest entry";
                return Diagnostics::kFailHandoffMissing;
            }
        }

        // —— 第 5 步：列表与清单的全集对应、闭包 / 运行期分类、记录只覆盖闭包条目 ——
        // 列表是内核侧书写的完整模块加载列表：条目集合必须与清单 Type=2 条目集合逐一相等，
        // 少一条会让模块漏装、多一条会指向清单没有的产物，两者都拒绝（E-KRN-54）。
        // 路径主名派生 ModuleId，命中生成期望集的是引导闭包成员，其余是运行期模块（由内核挂卷后装载）；
        // 预装记录只覆盖闭包条目，记录数必须等于闭包成员数（E-KRN-55）。
        bool matchedArtifact[KernelSet::ManifestLayout::kEntryMaxCount] = {};
        uint16_t listOfArtifact[KernelSet::ManifestLayout::kEntryMaxCount];
        for (uint32_t index = 0; index < KernelSet::ManifestLayout::kEntryMaxCount; ++index) listOfArtifact[index] = static_cast<uint16_t>(kHandoffMaxRecords);
        uint32_t osmEntryCount = 0;
        for (uint32_t index = 0; index < facts.manifest.entryCount; ++index) {
            if (facts.manifest.entries[index].type == KernelSet::ManifestLayout::kTypeOsm) ++osmEntryCount;
        }
        if (facts.list.count != osmEntryCount) {
            if (reason != nullptr) *reason = "the module load list doesn't enumerate every manifest .osm entry";
            return Diagnostics::kFailPreloadEntry;
        }
        bool seenClosure[kBootClosureCount] = {};
        for (uint32_t index = 0; index < facts.list.count; ++index) {
            const uint8_t* const path = facts.list.paths[index];
            const uint32_t pathBytes = facts.list.pathBytes[index];
            // 列表条目必须逐字节命中清单的 .osm 条目，且每个清单条目只能被一条列表条目占用
            uint16_t artifactIndex = static_cast<uint16_t>(KernelSet::ManifestLayout::kEntryMaxCount);
            for (uint32_t candidate = 0; candidate < facts.manifest.entryCount; ++candidate) {
                const ManifestEntry& entry = facts.manifest.entries[candidate];
                if (entry.type != KernelSet::ManifestLayout::kTypeOsm) continue;
                if (entry.pathBytes != pathBytes) continue;
                if (memcmp(entry.path, path, pathBytes) != 0) continue;
                if (matchedArtifact[candidate]) {
                    if (reason != nullptr) *reason = "two module load list entries claim the same manifest entry";
                    return Diagnostics::kFailPreloadEntry;
                }
                matchedArtifact[candidate] = true;
                artifactIndex = static_cast<uint16_t>(candidate);
                break;
            }
            if (artifactIndex == KernelSet::ManifestLayout::kEntryMaxCount) {
                if (reason != nullptr) *reason = "a module load list entry has no manifest .osm artifact";
                return Diagnostics::kFailPreloadEntry;
            }
            listOfArtifact[artifactIndex] = static_cast<uint16_t>(index);
            facts.list.artifactIndex[index] = artifactIndex;
            facts.list.closureMember[index] = kNotClosureMember;
            // 取不出逻辑名的条目（例如 Modules/Optional/ 下的可选运行期模块）不是闭包成员
            const uint8_t* name = nullptr;
            uint32_t nameBytes = 0;
            if (!Common::ModulePath::ExtractName(path, pathBytes, name, nameBytes)) continue;
            ModuleId derived{};
            if (!Common::ModulePath::DeriveModuleId<Common::Digest::Sha256>(name, nameBytes, derived)) continue;
            for (uint16_t member = 0; member < kBootClosureCount; ++member) {
                if (!(derived == kBootClosure[member].moduleId)) continue;
                if (seenClosure[member]) {
                    if (reason != nullptr) *reason = "the module load list repeats a boot closure module";
                    return Diagnostics::kFailBootClosure;
                }
                seenClosure[member] = true;
                facts.list.closureMember[index] = member;
                break;
            }
        }
        // 列表已与清单全集相等；闭包成员必须齐全，漏一个就不能交接
        for (uint16_t member = 0; member < kBootClosureCount; ++member) {
            if (seenClosure[member]) continue;
            if (reason != nullptr) *reason = "a boot closure member is missing from the module load list";
            return Diagnostics::kFailBootClosure;
        }

        // 记录与列表的闭包条目一一对应：下标指向清单 .osm，列表条目必须是闭包成员，
        // 每条闭包条目只被一条记录占用；记录数在下面按闭包成员数核对
        const uint8_t* modulePointers[kHandoffMaxRecords] = {};
        uint16_t recordOfList[kHandoffMaxRecords];
        for (uint32_t index = 0; index < kHandoffMaxRecords; ++index) recordOfList[index] = static_cast<uint16_t>(kHandoffMaxRecords);
        uint64_t moduleBytes = 0;
        for (uint32_t index = 0; index < recordCount; ++index) {
            const uint8_t* const raw = records + kHandoffHeaderBytes + index * kHandoffRecordBytes;
            HandoffRecord& record = facts.records[index];
            record.phys = Read64(raw + kRecordPhys);
            record.bytes = Read32(raw + kRecordBytes);
            record.artifactIndex = Read16(raw + kRecordArtifactIndex);
            record.reserved = Read16(raw + kRecordReserved);
            if (record.reserved != 0 || record.bytes == 0) return Diagnostics::kFailPreloadRecords;
            if (record.artifactIndex >= facts.manifest.entryCount) {
                if (reason != nullptr) *reason = "a preload record index is beyond the manifest entries";
                return Diagnostics::kFailPreloadRecords;
            }
            if (!RangeValid(record.phys, record.bytes)) {
                if (reason != nullptr) *reason = "a preload record points outside the low 4GiB identity window";
                return Diagnostics::kFailPreloadRecords;
            }
            const ManifestEntry& entry = facts.manifest.entries[record.artifactIndex];
            if (entry.type != KernelSet::ManifestLayout::kTypeOsm) return Diagnostics::kFailPreloadEntry;
            if (entry.dataBytes != record.bytes) return Diagnostics::kFailPreloadEntry;
            const uint16_t listIndex = listOfArtifact[record.artifactIndex];
            if (listIndex == kHandoffMaxRecords) return Diagnostics::kFailPreloadEntry;
            if (facts.list.closureMember[listIndex] == kNotClosureMember) {
                if (reason != nullptr) *reason = "a preload record isn't a boot closure member";
                return Diagnostics::kFailPreloadEntry;
            }
            if (recordOfList[listIndex] != kHandoffMaxRecords) {
                if (reason != nullptr) *reason = "two preload records claim the same module load list entry";
                return Diagnostics::kFailPreloadEntry;
            }
            recordOfList[listIndex] = static_cast<uint16_t>(index);
            // 模块字节：区间已核对，指针在这里取一次供后面几步复用
            const uint8_t* const module = map(record.phys, record.bytes);
            if (module == nullptr) return Diagnostics::kFailPreloadRecords;
            if (record.bytes < kModuleFixedFieldsEnd) return Diagnostics::kFailOsmContainer;
            modulePointers[index] = module;
            facts.moduleRanges[index].phys = record.phys;
            facts.moduleRanges[index].bytes = record.bytes;
            moduleBytes += record.bytes;
        }
        // 闭包成员与记录一一对应：记录数 = 闭包成员数；每条闭包条目恰好一条记录
        uint32_t closureEntryCount = 0;
        for (uint32_t listIndex = 0; listIndex < facts.list.count; ++listIndex) {
            if (facts.list.closureMember[listIndex] != kNotClosureMember) ++closureEntryCount;
        }
        if (recordCount != closureEntryCount) {
            if (reason != nullptr) *reason = "the preload record count doesn't match the closure entries";
            return Diagnostics::kFailPreloadEntry;
        }
        for (uint32_t listIndex = 0; listIndex < facts.list.count; ++listIndex) {
            const uint16_t member = facts.list.closureMember[listIndex];
            if (member == kNotClosureMember) {
                facts.runtimeListIndex[facts.runtimeCount] = static_cast<uint16_t>(listIndex);
                ++facts.runtimeCount;
                continue;
            }
            if (recordOfList[listIndex] == kHandoffMaxRecords) {
                if (reason != nullptr) *reason = "a boot closure member has no preload record";
                return Diagnostics::kFailBootClosure;
            }
            facts.closureListIndex[facts.closureCount] = static_cast<uint16_t>(listIndex);
            facts.closureRecordIndex[facts.closureCount] = recordOfList[listIndex];
            ++facts.closureCount;
        }
        // 闭包成员数必须等于期望集成员数：派生身份命中数已在上面核对，这里只防实现缺陷
        if (facts.closureCount != kBootClosureCount) {
            if (reason != nullptr) *reason = "the module load list doesn't cover the boot closure expectation set";
            return Diagnostics::kFailBootClosure;
        }

        // —— 第 6 步：主版本硬门：.os / 列表 / 每个模块的 ModuleVersionMajor 三者相等 ——
        if (osModuleMajor == 0) return Diagnostics::kFailBuildId;
        if (facts.list.major != osModuleMajor) {
            if (reason != nullptr) *reason = "the module load list major doesn't match the kernel";
            return Diagnostics::kFailBuildId;
        }
        for (uint32_t index = 0; index < recordCount; ++index) {
            if (Read16(modulePointers[index] + kModuleMajorOffset) != osModuleMajor) {
                if (reason != nullptr) *reason = "a preloaded module major doesn't match the kernel";
                return Diagnostics::kFailBuildId;
            }
        }

        // —— 第 7 步：BuildId 一致：.os 的 BuildId 与清单 BuildId 逐字节相等 ——
        if (osBuildId == nullptr) return Diagnostics::kFailBuildId;
        if (memcmp(facts.manifest.buildId, osBuildId, Boot::InfoLayout::kKernelSetBuildIdBytes) != 0) {
            if (reason != nullptr) *reason = "the manifest BuildId doesn't match the kernel image";
            return Diagnostics::kFailBuildId;
        }

        // —— 第 8 步：期望集完整性：容器 ModuleId 命中生成期望集，缺 / 重 / 未知都报出 ——
        bool seen[kBootClosureCount] = {};
        for (uint32_t index = 0; index < recordCount; ++index) {
            ModuleId id{};
            memcpy(id.bytes, modulePointers[index] + kModuleIdOffset, kModuleIdBytes);
            bool known = false;
            for (uint16_t member = 0; member < kBootClosureCount; ++member) {
                if (!(id == kBootClosure[member].moduleId)) continue;
                if (seen[member]) {
                    if (reason != nullptr) *reason = "a boot closure member has more than one preload record";
                    return Diagnostics::kFailBootClosure;
                }
                seen[member] = true;
                known = true;
                break;
            }
            if (!known) {
                if (reason != nullptr) *reason = "a preload record isn't a boot closure member";
                return Diagnostics::kFailBootClosure;
            }
        }
        for (uint16_t member = 0; member < kBootClosureCount; ++member) {
            if (seen[member]) continue;
            if (reason != nullptr) *reason = "a boot closure member has no preload record";
            return Diagnostics::kFailBootClosure;
        }

        // —— 第 9 步：摘要策略：闭包摘要信任 Baleen 交接，这里只交出条目视图，不复算 ——
        memcpy(facts.buildId, osBuildId, Boot::InfoLayout::kKernelSetBuildIdBytes);
        facts.moduleMajor = osModuleMajor;
        facts.moduleRevision = facts.list.revision;
        facts.recordCount = recordCount;
        facts.moduleBytes = moduleBytes;
        facts.manifestRange.phys = info.kernelSetManifest;
        facts.manifestRange.bytes = info.kernelSetManifestBytes;
        facts.listRange.phys = info.bootList;
        facts.listRange.bytes = info.bootListBytes;
        facts.recordsRange.phys = info.bootModules;
        facts.recordsRange.bytes = recordsBytes;
        return Diagnostics::Diagnostic{};
    }
}
