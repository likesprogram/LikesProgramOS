/* BootClosure.cpp
    引导闭包预装共享机制的实现：列表解析与期望集核对、内存图落点搜索、逐模块核对与读入、记录填写

    预装失败不交接半套：PlanPreload 失败不产出可用的计划，PreloadModules 失败把记录数组头清零
    并归还已分配的落点；调用方按所选集失败处理（候选集回退与终止由选择器负责）
    列表是内核侧书写的完整模块加载列表：本模块只预装其中命中引导闭包期望集的成员，
    非闭包条目（运行期模块）跳过，由内核挂卷后按同一份列表装载
    清单字节只作为交接副本传递，本模块不解析、不重验签名（已由 ManifestVerify 完成）
    诊断编号：94 最小读取面、95 主版本、96 列表与期望集、97 落点、67 读、68 摘要
*/

#include <BootClosure.hpp>

/* 生成的引导闭包期望集头与本模块同名（都叫 BootClosure.hpp），include 搜索路径会先命中本包头，
    因此用相对路径直接引入 Packages/Common/Out/Contract 下的生成物；构建接入时若在 Makefile 加上
    -I$(COMMON_DIR)，可改写成 <Out/Contract/BootClosure.hpp>，本实现不改搜索路径也能编译
*/
#include "../../../Common/Out/Contract/BootClosure.hpp"

#include <LayoutCheck.hpp>
#include <Libc.hpp>
#include <Manifest.hpp>
#include <ModulePath.hpp>
#include <Sha256.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            // 模块整文件读入时的分块字节数：块越大读盘调用越少，同时不额外占用落点之外的缓冲
            constexpr uint32_t kModuleReadChunkBytes = 0x10000;

            // —— 本阶段失败原因的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailArtifactRead{BALEEN_STAGE_CODE("67"), "Read a kernel set artifact"};
            constexpr Diagnostic kFailArtifactDigest{BALEEN_STAGE_CODE("68"), "A kernel set artifact digest doesn't match"};
            constexpr Diagnostic kFailModuleHeader{BALEEN_STAGE_CODE("94"), "The module file header isn't valid"};
            constexpr Diagnostic kFailModuleMajor{BALEEN_STAGE_CODE("95"), "The module major version doesn't match the kernel"};
            constexpr Diagnostic kFailBootList{BALEEN_STAGE_CODE("96"), "The module load list is invalid"};
            // 引导闭包期望集核对的两类现场共用 96，用文本区分漏 / 重；
            // 列表里的非闭包条目（运行期模块）由内核装载，不预装、不报错
            constexpr Diagnostic kFailBootListMissing{BALEEN_STAGE_CODE("96"), "The module load list is missing a boot closure module"};
            constexpr Diagnostic kFailBootListDuplicate{BALEEN_STAGE_CODE("96"), "The module load list repeats a boot closure module"};
            constexpr Diagnostic kFailNoMemory{BALEEN_STAGE_CODE("97"), "There is no memory for preloading the boot closure modules"};

            // 记录数组的静态存储：随 BootInfo 交给内核，交接期间常驻引导器映像
            PreloadArray s_records;
            // 模块路径拼接缓冲：集目录 + '/' + 列表里的相对路径，逐个模块复用
            char s_modulePath[kPreloadPathBytes];

            // 列表解析游标：在列表副本上按行推进，不复制内容
            struct ListCursor {
                const uint8_t* data;  // 列表字节
                uint32_t bytes;       // 列表字节数
                uint32_t pos;         // 当前偏移
            };

            // 读一行：内容不含换行，游标推进到下一行行首；缺结尾换行、含 CR 或 NUL、空行都返回 false
            bool ReadLine(ListCursor& cursor, const uint8_t*& line, uint32_t& lineBytes) {
                if (cursor.pos >= cursor.bytes) return false;
                const uint32_t begin = cursor.pos;
                uint32_t end = begin;
                while (end < cursor.bytes && cursor.data[end] != '\n') {
                    if (cursor.data[end] == '\r' || cursor.data[end] == 0) return false;
                    ++end;
                }
                if (end >= cursor.bytes) return false;
                if (end == begin) return false;
                cursor.pos = end + 1;
                line = cursor.data + begin;
                lineBytes = end - begin;
                return true;
            }

            // 行是否以给定字面量起头；字面量用长度给定时不依赖结尾零
            bool StartsWith(const uint8_t* line, uint32_t lineBytes, const char* literal, uint32_t literalBytes) {
                if (lineBytes < literalBytes) return false;
                for (uint32_t i = 0; i < literalBytes; ++i) if (line[i] != static_cast<uint8_t>(literal[i])) return false;
                return true;
            }

            // 解析十进制定点数并推进游标：零位、非数字、超过 kMaxValue 都返回 false
            // 上限取编译期常量：32 位目标用 -nostdlib 链接，不能引入 64 位除法的运行库辅助函数
            // 判据 result * 10 + digit <= kMaxValue 等价于 result <= (kMaxValue - digit) / 10，
            // 十个阈值全部在编译期算好
            template <uint64_t kMaxValue>
            bool ParseDecimal(const uint8_t*& text, uint32_t& remaining, uint64_t& value) {
                constexpr uint64_t kLimit[10] = {
                    (kMaxValue - 0) / 10, (kMaxValue - 1) / 10, (kMaxValue - 2) / 10, (kMaxValue - 3) / 10, (kMaxValue - 4) / 10,
                    (kMaxValue - 5) / 10, (kMaxValue - 6) / 10, (kMaxValue - 7) / 10, (kMaxValue - 8) / 10, (kMaxValue - 9) / 10,
                };
                uint64_t result = 0;
                uint32_t digits = 0;
                while (remaining > 0 && text[0] >= '0' && text[0] <= '9') {
                    const uint32_t digit = static_cast<uint32_t>(text[0] - '0');
                    if (result > kLimit[digit]) return false;
                    result = result * 10 + digit;
                    ++digits;
                    ++text;
                    --remaining;
                }
                if (digits == 0) return false;
                value = result;
                return true;
            }

            // 拼接模块路径：集目录 + '/' + 列表里的相对路径；相接处只保留一个 '/'，超长返回 false
            bool JoinModulePath(const char* directory, const uint8_t* relative, uint32_t relativeBytes) {
                if (directory == nullptr || relative == nullptr) return false;
                uint32_t length = 0;
                for (; directory[length] != '\0'; ++length) {
                    if (length + 1 >= kPreloadPathBytes) return false;
                    s_modulePath[length] = directory[length];
                }
                if (length > 0 && s_modulePath[length - 1] != '/') {
                    if (length + 1 >= kPreloadPathBytes) return false;
                    s_modulePath[length++] = '/';
                }
                for (uint32_t i = 0; i < relativeBytes; ++i) {
                    if (length + 1 >= kPreloadPathBytes) return false;
                    s_modulePath[length++] = static_cast<char>(relative[i]);
                }
                s_modulePath[length] = '\0';
                return true;
            }

            // 在清单 Type=2 条目视图里按字节相等找路径；找不到返回空
            const PreloadArtifact* FindArtifact(const PreloadSource& source, const uint8_t* path, uint32_t pathBytes) {
                for (uint32_t i = 0; i < source.artifactCount; ++i) {
                    const PreloadArtifact& artifact = source.artifacts[i];
                    if (artifact.pathBytes != pathBytes || artifact.path == nullptr) continue;
                    if (memcmp(artifact.path, path, pathBytes) == 0) return &artifact;
                }
                return nullptr;
            }

            // 向上对齐到 4 KiB
            uint64_t AlignUp(uint64_t value) {
                return (value + kPreloadPageBytes - 1) & ~(kPreloadPageBytes - 1);
            }

            // 保留跨度与低 4 GiB 的交集：长度回绕按无上界处理，越出低 4 GiB 的部分截掉
            // 与搜索区完全不相交时返回 false
            bool ClipSpan(uint64_t& begin, uint64_t& bytes) {
                if (bytes == 0 || begin >= kPreloadAddressLimit) return false;
                uint64_t end = begin + bytes;
                if (end < begin || end > kPreloadAddressLimit) end = kPreloadAddressLimit;
                if (end <= begin) return false;
                bytes = end - begin;
                return true;
            }

            // 若 [begin, begin+bytes) 与保留跨度重叠，把 advanced 设为该跨度末端并返回 true
            bool AdvancePast(uint64_t begin, uint64_t bytes, uint64_t otherBegin, uint64_t otherBytes, uint64_t& advanced) {
                if (!ClipSpan(otherBegin, otherBytes)) return false;
                const uint64_t end = begin + bytes;
                if (begin >= otherBegin + otherBytes || otherBegin >= end) return false;
                advanced = otherBegin + otherBytes;
                return true;
            }

            // 失败收尾：归还已分配的落点、记录数组头清成 0 条，保证不交接半套
            Diagnostic AbortPreload(PreloadSink& sink, const PreloadSpan* spans, uint32_t count, const Diagnostic& reason) {
                for (uint32_t i = count; i > 0; --i) sink.Release(spans[i - 1]);
                s_records.header.count = 0;
                s_records.header.reserved = 0;
                return reason;
            }

            // 关闭模块文件并做失败收尾：归还已分配的落点、记录数组头清零
            Diagnostic FailModule(PreloadSink& sink, uint32_t handle, const PreloadSpan* spans, uint32_t count, const Diagnostic& reason) {
                sink.Close(handle);
                return AbortPreload(sink, spans, count, reason);
            }

            // 句柄到槽位下标：非法句柄返回 kPreloadSlots
            uint32_t HandleSlot(uint32_t handle) {
                if (handle == 0 || handle > kPreloadSlots) return kPreloadSlots;
                return handle - 1;
            }
        }

        StoragePreloadSink::StoragePreloadSink(Storage::FileSystem& fs, SpanAllocator& allocator) : m_fs(fs), m_allocator(allocator) {}

        bool StoragePreloadSink::Open(const char* path, uint32_t& handle) {
            if (path == nullptr) return false;
            Storage::FileId id;
            if (m_fs.Open(path, id) != Storage::Error::Ok) return false;
            Storage::FileInfo info{};
            if (m_fs.Stat(path, info) != Storage::Error::Ok) return false;
            if (info.directory) return false;
            for (uint32_t slot = 0; slot < kPreloadSlots; ++slot) {
                if (m_files[slot].used) continue;
                m_files[slot].id = id;
                m_files[slot].bytes = info.bytes;
                m_files[slot].used = true;
                handle = slot + 1;
                return true;
            }
            return false;
        }

        bool StoragePreloadSink::Size(uint32_t handle, uint64_t& bytes) {
            const uint32_t slot = HandleSlot(handle);
            if (slot >= kPreloadSlots || !m_files[slot].used) return false;
            bytes = m_files[slot].bytes;
            return true;
        }

        bool StoragePreloadSink::Read(uint32_t handle, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) {
            const uint32_t slot = HandleSlot(handle);
            if (slot >= kPreloadSlots || !m_files[slot].used) return false;
            if (buffer == nullptr && bytes != 0) return false;
            read = 0;
            return m_fs.Read(m_files[slot].id, offset, buffer, bytes, read) == Storage::Error::Ok;
        }

        void StoragePreloadSink::Close(uint32_t handle) {
            const uint32_t slot = HandleSlot(handle);
            if (slot >= kPreloadSlots) return;
            m_files[slot].used = false;
        }

        bool StoragePreloadSink::Allocate(uint32_t bytes, PreloadSpan& span) {
            return m_allocator.Allocate(bytes, span);
        }

        void StoragePreloadSink::Release(const PreloadSpan& span) {
            m_allocator.Release(span);
        }

        MemoryMapAllocator::MemoryMapAllocator(const Boot::MemoryMapEntry* memoryMap, uint32_t memoryMapCount, uint64_t loaderBegin, uint64_t loaderBytes, uint64_t kernelBegin, uint64_t kernelBytes) {
            m_constraints.memoryMap = memoryMap;
            m_constraints.memoryMapCount = memoryMapCount;
            m_constraints.loaderBegin = loaderBegin;
            m_constraints.loaderBytes = loaderBytes;
            m_constraints.kernelBegin = kernelBegin;
            m_constraints.kernelBytes = kernelBytes;
            m_constraints.taken = m_taken;
            m_constraints.takenCount = 0;
        }

        bool MemoryMapAllocator::Allocate(uint32_t bytes, PreloadSpan& span) {
            const uint64_t phys = FindModuleSpan(m_constraints, bytes);
            if (phys == 0) return false;
            span.phys = phys;
            span.data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(phys));
            span.bytes = bytes;
            if (m_takenCount < kPreloadSlots) {
                m_taken[m_takenCount] = span;
                ++m_takenCount;
                m_constraints.takenCount = m_takenCount;
            }
            return true;
        }

        void MemoryMapAllocator::Release(const PreloadSpan& span) {
            for (uint32_t i = 0; i < m_takenCount; ++i) {
                if (m_taken[i].phys != span.phys) continue;
                m_taken[i] = m_taken[m_takenCount - 1];
                --m_takenCount;
                m_constraints.takenCount = m_takenCount;
                return;
            }
        }

        Diagnostic PlanPreload(const PreloadSource& source, PreloadPlan& plan) {
            plan = PreloadPlan{};
            // 输入基本校验：来源由调用方保证是验签通过的字节，这里只挡空指针与容量
            if (source.setDirectory == nullptr) return kFailBootList;
            if (source.manifest == nullptr || source.manifestBytes == 0) return kFailBootList;
            if (source.list == nullptr || source.listBytes == 0 || source.listBytes > kBootListMaxBytes) return kFailBootList;
            if (source.artifacts == nullptr || source.artifactCount == 0 || source.artifactCount > kPreloadSlots) return kFailBootList;

            // 固定头：LPML 1 / major <非零> / revision <非零>，顺序固定、不得缺失
            ListCursor cursor{source.list, source.listBytes, 0};
            const uint8_t* line = nullptr;
            uint32_t lineBytes = 0;
            if (!ReadLine(cursor, line, lineBytes)) return kFailBootList;
            if (lineBytes != 6 || memcmp(line, "LPML 1", 6) != 0) return kFailBootList;
            if (!ReadLine(cursor, line, lineBytes)) return kFailBootList;
            if (!StartsWith(line, lineBytes, "major ", 6)) return kFailBootList;
            const uint8_t* text = line + 6;
            uint32_t remaining = lineBytes - 6;
            uint64_t listMajor = 0;
            if (!ParseDecimal<0xFFFF>(text, remaining, listMajor) || remaining != 0 || listMajor == 0) return kFailBootList;
            if (!ReadLine(cursor, line, lineBytes)) return kFailBootList;
            if (!StartsWith(line, lineBytes, "revision ", 9)) return kFailBootList;
            text = line + 9;
            remaining = lineBytes - 9;
            uint64_t revision = 0;
            if (!ParseDecimal<0xFFFFFFFFFFFFFFFFull>(text, remaining, revision) || remaining != 0 || revision == 0) return kFailBootList;

            // 主版本硬门：列表主版本必须等于 .os 的 ModuleMajor；调用方没有给出 .os 事实时按列表值
            const uint16_t moduleMajor = source.moduleMajor != 0 ? source.moduleMajor : static_cast<uint16_t>(listMajor);
            if (static_cast<uint16_t>(listMajor) != moduleMajor) return kFailModuleMajor;

            // 条目分两遍核对：先做结构与容量（逐行格式、路径语法、Modules/ 前缀、去重、30 条上限），
            // 再与清单条目和引导闭包期望集做语义对应。结构错误先于语义错误报出，超限的列表不会
            // 先撞上语义核对而掩盖容量问题
            const uint8_t* listedPaths[kPreloadMaxEntries] = {};
            uint16_t listedPathBytes[kPreloadMaxEntries] = {};
            uint32_t listedCount = 0;
            while (cursor.pos < cursor.bytes) {
                if (!ReadLine(cursor, line, lineBytes)) return kFailBootList;
                if (!StartsWith(line, lineBytes, "module ", 7)) return kFailBootList;
                const uint8_t* const path = line + 7;
                const uint32_t pathBytes = lineBytes - 7;
                if (pathBytes == 0 || pathBytes > KernelSet::ManifestLayout::kEntryPathMaxBytes) return kFailBootList;
                if (!LayoutCheck::PathValid(path, pathBytes, false)) return kFailBootList;
                if (pathBytes <= 8 || !StartsWith(path, pathBytes, "Modules/", 8)) return kFailBootList;
                // 条目上限 30 条：超过即列表非法
                if (listedCount >= kPreloadMaxEntries) return kFailBootList;
                // 同一路径不得出现两次（逐字节比较，不做大小写或分隔符归一化）
                for (uint32_t i = 0; i < listedCount; ++i) {
                    if (listedPathBytes[i] != pathBytes) continue;
                    if (memcmp(listedPaths[i], path, pathBytes) == 0) return kFailBootList;
                }
                listedPaths[listedCount] = path;
                listedPathBytes[listedCount] = static_cast<uint16_t>(pathBytes);
                ++listedCount;
            }

            // 与清单 Type=2 条目逐字节对应：列表是配置，不允许指向清单没登记的产物
            // 路径主名派生模块标识，命中生成的闭包成员表（唯一事实源 Contract/BootClosure.def）的
            // 条目按列表顺序进预装计划；不命中的条目是运行期模块，由内核挂卷后装载，这里不预装、不报错
            // 闭包成员漏 / 重仍按 96 报出：Baleen 交不出完整闭包就不能交接
            bool claimed[kBootClosureCount] = {};
            uint32_t planned = 0;
            uint64_t totalBytes = 0;
            for (uint32_t index = 0; index < listedCount; ++index) {
                const uint8_t* const path = listedPaths[index];
                const uint32_t pathBytes = listedPathBytes[index];
                const PreloadArtifact* const artifact = FindArtifact(source, path, pathBytes);
                if (artifact == nullptr) return kFailBootList;
                const uint8_t* name = nullptr;
                uint32_t nameBytes = 0;
                // 取不出逻辑名（例如 Modules/Optional/ 下的可选运行期模块）按非闭包条目处理
                if (!Common::ModulePath::ExtractName(path, pathBytes, name, nameBytes)) continue;
                ModuleId derived;
                if (!Common::ModulePath::DeriveModuleId<Digest::Sha256>(name, nameBytes, derived)) return kFailBootList;
                uint16_t member = kBootClosureCount;
                for (uint16_t candidate = 0; candidate < kBootClosureCount; ++candidate) {
                    if (!(derived == kBootClosure[candidate].moduleId)) continue;
                    member = candidate;
                    break;
                }
                if (member == kBootClosureCount) continue;   // 运行期模块：不预装
                if (claimed[member]) return kFailBootListDuplicate;
                claimed[member] = true;
                if (artifact->digest == nullptr || artifact->dataBytes == 0) return kFailBootList;
                PreloadEntry& entry = plan.entries[planned];
                entry.artifactIndex = static_cast<uint16_t>(artifact->artifactIndex);
                entry.pathBytes = artifact->pathBytes;
                entry.path = artifact->path;
                entry.dataBytes = artifact->dataBytes;
                entry.digest = artifact->digest;
                totalBytes += entry.dataBytes;
                ++planned;
            }

            // 应有成员齐全：13 个成员每个都必须出现且只出现一次
            for (uint16_t member = 0; member < kBootClosureCount; ++member) if (!claimed[member]) return kFailBootListMissing;

            plan.manifest = source.manifest;
            plan.manifestBytes = source.manifestBytes;
            plan.list = source.list;
            plan.listBytes = source.listBytes;
            plan.setDirectory = source.setDirectory;
            plan.moduleMajor = moduleMajor;
            plan.revision = revision;
            plan.count = planned;
            plan.moduleBytes = totalBytes;
            return Diagnostic{};
        }

        uint64_t FindModuleSpan(const SpanConstraints& constraints, uint64_t bytes) {
            if (bytes == 0 || bytes > kPreloadAddressLimit) return 0;
            if (constraints.memoryMap == nullptr || constraints.memoryMapCount == 0) return 0;
            // 相邻的可用条目按同一段连续内存合并；只有紧邻（前一段末端等于后一段起址）才算连续
            uint32_t index = 0;
            while (index < constraints.memoryMapCount) {
                const Boot::MemoryMapEntry& entry = constraints.memoryMap[index];
                ++index;
                if (entry.type != 1 || entry.length == 0) continue;
                uint64_t runBegin = entry.base;
                uint64_t runEnd = entry.base + entry.length;
                if (runEnd < runBegin) runEnd = kPreloadAddressLimit;
                while (index < constraints.memoryMapCount) {
                    const Boot::MemoryMapEntry& next = constraints.memoryMap[index];
                    if (next.type != 1 || next.length == 0 || next.base > runEnd) break;
                    const uint64_t nextEnd = next.base + next.length;
                    if (nextEnd < next.base) {
                        runEnd = kPreloadAddressLimit;
                        ++index;
                        break;
                    }
                    if (nextEnd > runEnd) runEnd = nextEnd;
                    ++index;
                }
                // 截到低 4 GiB 之下、避开低 1 MiB；保留区上界与搜索上界都不含
                if (runEnd <= kPreloadLowLimit || runBegin >= kPreloadAddressLimit) continue;
                if (runBegin < kPreloadLowLimit) runBegin = kPreloadLowLimit;
                if (runEnd > kPreloadAddressLimit) runEnd = kPreloadAddressLimit;
                uint64_t start = AlignUp(runBegin);
                while (start < runEnd && bytes <= runEnd - start) {
                    // 与保留跨度重叠时整段跳过：推进到该跨度末端并重新对齐，再重查所有跨度
                    uint64_t advance = 0;
                    bool overlapped = false;
                    if (AdvancePast(start, bytes, constraints.loaderBegin, constraints.loaderBytes, advance)) {
                        overlapped = true;
                    } else if (AdvancePast(start, bytes, constraints.kernelBegin, constraints.kernelBytes, advance)) {
                        overlapped = true;
                    } else {
                        for (uint32_t taken = 0; taken < constraints.takenCount; ++taken) {
                            const PreloadSpan& span = constraints.taken[taken];
                            if (!AdvancePast(start, bytes, span.phys, span.bytes, advance)) continue;
                            overlapped = true;
                            break;
                        }
                    }
                    if (!overlapped) return start;
                    start = AlignUp(advance);
                }
            }
            return 0;
        }

        Diagnostic PreloadModules(const PreloadPlan& plan, PreloadSink& sink, PreloadResult& result) {
            result = PreloadResult{};
            PreloadSpan spans[kPreloadMaxEntries] = {};
            uint32_t allocated = 0;
            uint32_t records = 0;
            uint64_t totalBytes = 0;
            // 记录数组头先归零：任何失败都不留下上一轮或半套的记录
            s_records.header.count = 0;
            s_records.header.reserved = 0;
            if (plan.count > kPreloadMaxEntries) return AbortPreload(sink, spans, allocated, kFailBootList);
            for (uint32_t index = 0; index < plan.count; ++index) {
                const PreloadEntry& entry = plan.entries[index];
                if (entry.path == nullptr || entry.digest == nullptr) return AbortPreload(sink, spans, allocated, kFailBootList);
                if (!JoinModulePath(plan.setDirectory, entry.path, entry.pathBytes)) return AbortPreload(sink, spans, allocated, kFailBootList);
                uint32_t handle = 0;
                if (!sink.Open(s_modulePath, handle)) return AbortPreload(sink, spans, allocated, kFailArtifactRead);
                // 长度两处一致：清单 DataBytes 等于实际文件长度；TotalBytes 在最小读取面里再与它对一次
                uint64_t fileBytes = 0;
                if (!sink.Size(handle, fileBytes)) return FailModule(sink, handle, spans, allocated, kFailArtifactRead);
                if (fileBytes > 0xFFFFFFFFull || fileBytes < kModuleHeaderBytes) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                if (fileBytes != entry.dataBytes) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                // 最小读取面：头 0x00..0x14 的 Magic / Version / HeaderBytes / Flags / TotalBytes
                uint8_t header[kModuleHeaderBytes] = {};
                uint32_t read = 0;
                if (!sink.Read(handle, 0, header, kModuleHeaderBytes, read) || read != kModuleHeaderBytes) return FailModule(sink, handle, spans, allocated, kFailArtifactRead);
                if (memcmp(header, kModuleMagic, kModuleMagicBytes) != 0) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                if (LayoutCheck::Get16(header + 0x08) != kModuleFormatVersion) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                if (LayoutCheck::Get16(header + 0x0A) != kModuleHeaderSize) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                if (LayoutCheck::Get32(header + 0x0C) != 0) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                if (LayoutCheck::Get32(header + 0x10) != static_cast<uint32_t>(fileBytes)) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                // 主版本硬门：只定向读 0x66 两字节，跨主版本不搬运
                if (fileBytes < kModuleMajorOffset + 2) return FailModule(sink, handle, spans, allocated, kFailModuleHeader);
                uint8_t majorBytes[2] = {};
                if (!sink.Read(handle, kModuleMajorOffset, majorBytes, 2, read) || read != 2) return FailModule(sink, handle, spans, allocated, kFailArtifactRead);
                if (LayoutCheck::Get16(majorBytes) != plan.moduleMajor) return FailModule(sink, handle, spans, allocated, kFailModuleMajor);
                // 落点：低 4 GiB、4 KiB 对齐，由平台注入的分配器决定
                PreloadSpan span{};
                if (!sink.Allocate(static_cast<uint32_t>(fileBytes), span)) return FailModule(sink, handle, spans, allocated, kFailNoMemory);
                if (span.data == nullptr || span.bytes < fileBytes) {
                    sink.Release(span);
                    return FailModule(sink, handle, spans, allocated, kFailNoMemory);
                }
                spans[allocated] = span;
                if (allocated < kPreloadMaxEntries) ++allocated;
                // 整文件读入落点并复算 SHA-256，与清单 Digest 核对
                Digest::Sha256 digest;
                digest.Begin();
                uint32_t offset = 0;
                while (offset < fileBytes) {
                    const uint32_t remaining = static_cast<uint32_t>(fileBytes) - offset;
                    const uint32_t want = remaining < kModuleReadChunkBytes ? remaining : kModuleReadChunkBytes;
                    if (!sink.Read(handle, offset, span.data + offset, want, read) || read != want) return FailModule(sink, handle, spans, allocated, kFailArtifactRead);
                    digest.Update(span.data + offset, read);
                    offset += read;
                }
                uint8_t actual[Digest::kSha256Bytes] = {};
                digest.Finish(actual);
                if (memcmp(actual, entry.digest, Digest::kSha256Bytes) != 0) return FailModule(sink, handle, spans, allocated, kFailArtifactDigest);
                sink.Close(handle);
                // 填记录：物理地址、字节数、清单产物下标，保留位 0
                PreloadRecord& record = s_records.records[records];
                record.phys = span.phys;
                record.bytes = static_cast<uint32_t>(fileBytes);
                record.artifactIndex = entry.artifactIndex;
                record.reserved = 0;
                ++records;
                totalBytes += fileBytes;
            }
            // 成功：记录数等于引导闭包成员数，三类缓冲地址与长度交给 BootInfo
            s_records.header.count = records;
            s_records.header.reserved = 0;
            result.manifestPhys = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(plan.manifest));
            result.manifestBytes = plan.manifestBytes;
            result.listPhys = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(plan.list));
            result.listBytes = plan.listBytes;
            result.recordsPhys = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&s_records));
            result.recordCount = records;
            result.moduleBytes = totalBytes;
            return Diagnostic{};
        }
    }
}
