/* LoadCore.cpp
    Stub 的 Core 装载机制实现：读描述符与镜像头、查内存图、挑弹跳窗口、高位拷贝、摘要校验、填交权块

    镜像头先于整段装载核对：装载区大小、入口与静态内存跨度都取自头字段；整段读入后再比对摘要
    读盘只能落低 1MiB，高位装载因此分两步：先读进低地址弹跳窗口，再用 32 位平坦段整段拷上去
    这条路不引入 unreal 模式，也不要求固件认识高位缓冲；窗口本身按内存图挑，不假定固定地址可用
    本文件不产生诊断输出，也不决定步骤顺序：失败只回报原因文本，打印与停机都由 _Stub_Main 负责
*/

#include <stdint.h>

#include <Bios.hpp>
#include <BootInfo.hpp>
#include <CoreHandoff.hpp>
#include <ImageHeader.hpp>
#include <LoadCore.hpp>

// 由 Stub.asm 提供：把 ESI 置为交权块、跳到 Core 入口后不返回
extern "C" [[noreturn]] void _Stub_Enter_Core(const Baleen::CoreHandoff* handoff, uintptr_t entry);
// 交权块暴露给 Core 的控制台入口，由 Stub.cpp 提供；与三个 _Bios_* 一样是外部符号
extern "C" void _Stub_Write(const char* text);

namespace Baleen {
    namespace Stub {
        namespace {
            // —— 交权契约常量：取值必须与 Contract.inc 一致 ——
            // 汇编侧由 %INCLUDE 取 Contract.inc，C++ 侧包含不了 .inc，只能在此重申；
            // 描述符区布局：与 Packages/Baleen/Common/Include/Desc.inc 一致 ——
            // 头部给出本区起止位置与段序列起点；段头给出标记、版本与段长，读取方按段长遍历
            // 位置字段都是 512 字节基准（值 = 字节偏移 / 512），按本地单位右移换算成本地 LBA
            constexpr uint32_t kBaseShift = 9;                  // 位置字段的基准单位：1 << 9 = 512 字节
            constexpr uint32_t kAreaMagic = 0x43534442;         // 'BDSC'，头部标记
            constexpr uint32_t kFieldAreaHeadBytes = 0x06;      // 头部内头长字段
            constexpr uint32_t kAreaHeadLen = 16;               // 头部固定长度
            constexpr uint32_t kFieldAreaLba = 0x08;            // 头部内本区起始位置，512 字节基准
            constexpr uint32_t kFieldAreaEndLba = 0x0C;         // 头部内本区结束位置，512 字节基准，含
            constexpr uint32_t kSegmentHeadBytes = 8;           // 段头长度：标记 4 + 版本 2 + 段长 2
            constexpr uint32_t kFieldSegmentLength = 0x06;      // 段内段长字段，含段头
            constexpr uint32_t kFieldSegmentLba = 0x08;         // 段内载荷起始位置，512 字节基准
            constexpr uint32_t kFieldSegmentEndLba = 0x0C;      // 段内载荷结束位置，512 字节基准，含
            constexpr uint32_t kCoreMagic = 0x52444342;         // 'BCDR'，Core 段标记
            constexpr uint32_t kCoreLoad = 0x100000;            // Core 装入的物理地址，即镜像基址
            constexpr uint32_t kCoreMaxBytes = 0x400000;        // Core 文件字节数上限
            constexpr uint32_t kCoreMaxMemory = 0x400000;       // Core 静态内存跨度上限，含未落盘尾部
            constexpr uint32_t kHandoffMagic = 0x484E4C42;      // 'BLNH'
            constexpr uint32_t kHandoffVersion = 1;

            // 本地扇区大小上限：探测只认 512、2048、4096，缓冲按上限预留
            constexpr uint32_t kMaxSectorBytes = 4096;
            // 内存图缓冲条目数，与 Bios.asm 的 E820_MAX 一致
            constexpr uint32_t kMemoryMapCapacity = 64;
            // 弹跳窗口：从 0x10000 起按窗口大小步进，取第一个整段可用的 64KiB；装载期只在这里中转
            constexpr uint32_t kBounceBegin = 0x10000;
            constexpr uint32_t kBounceEnd = 0x80000;
            constexpr uint32_t kBounceBytes = 0x10000;

            // 读描述符用的扇区缓冲，整扇区读入，16 字节对齐是 EDD 的 DAP 前提
            uint8_t s_sector[kMaxSectorBytes] __attribute__((aligned(16)));
            // 内存图静态缓冲：交权块把它交给 Core，不能放在栈上
            Boot::MemoryMapEntry s_memoryMap[kMemoryMapCapacity];
            // 内存图的条数与截断标志，读完一次后与缓冲一同交给 Core
            uint32_t s_memoryMapCount = 0;
            uint32_t s_memoryMapTruncated = 0;
            // 交权块本体：_Stub_Main 走到最后一步时填好，地址随 ESI 交给 Core
            CoreHandoff s_handoff;

            // 从缓冲区按小端读取 16 位
            uint16_t Get16(const uint8_t* field) {
                return static_cast<uint16_t>(field[0] | (field[1] << 8));
            }

            // 从缓冲区按小端读取 32 位
            uint32_t Get32(const uint8_t* field) {
                uint32_t value = 0;
                for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
                return value;
            }

            // 扇区大小的移位量与掩码：探测只认 512、2048、4096，都是 2 的幂，换算用移位免除法
            uint32_t SectorShift(uint32_t sectorBytes) {
                return static_cast<uint32_t>(__builtin_ctz(sectorBytes));
            }

            // 位置字段换算的右移量：512 字节基准 → 本地 LBA（512 → 0、2048 → 2、4096 → 3）
            uint32_t BaseShift(uint32_t sectorBytes) {
                return SectorShift(sectorBytes) - kBaseShift;
            }

            // 从低地址弹跳窗口整段拷到高位：调用时数据段与目的段都是基址 0 的平坦段
            void CopyUp(uint8_t* dest, uint8_t* source, uint32_t bytes) {
                asm volatile("cld; rep movsb" : "+S"(source), "+D"(dest), "+c"(bytes) : : "memory", "cc");
            }

            // 把一段内存清零：交权前清零 Core 的未落盘尾部，平坦段下按物理地址直接写
            void ClearBytes(uint8_t* dest, uint32_t bytes) {
                if (bytes == 0) return;
                asm volatile("cld; rep stosb" : "+D"(dest), "+c"(bytes) : "a"(0) : "memory", "cc");
            }

            // 区间 [begin, begin + bytes) 是否整段落在内存图的可用区里
            bool RegionUsable(uint32_t begin, uint32_t bytes) {
                if (bytes == 0 || begin + bytes < begin) return false;
                const uint64_t end = static_cast<uint64_t>(begin) + bytes;
                for (uint32_t i = 0; i < s_memoryMapCount; ++i) {
                    if (s_memoryMap[i].type != 1) continue;
                    const uint64_t base = s_memoryMap[i].base;
                    const uint64_t tail = base + s_memoryMap[i].length;
                    if (tail < base) continue;
                    if (base <= begin && end <= tail) return true;
                }
                return false;
            }

            // 从 begin 起连续可用的字节数：沿内存图可用条目向外扩张到不再相邻为止，上限 limit，
            // 并按扇区大小向下取整；条目顺序不作假定，每轮扩张到不动点，最多 count 轮
            uint32_t UsableSpan(uint32_t begin, uint32_t limit, uint32_t sectorBytes) {
                const uint64_t ceil = static_cast<uint64_t>(begin) + limit;
                uint64_t reach = begin;
                for (uint32_t round = 0; round < s_memoryMapCount; ++round) {
                    bool grew = false;
                    for (uint32_t i = 0; i < s_memoryMapCount; ++i) {
                        if (s_memoryMap[i].type != 1) continue;
                        const uint64_t base = s_memoryMap[i].base;
                        const uint64_t tail = base + s_memoryMap[i].length;
                        if (tail < base || tail <= begin) continue;
                        if (base > reach) continue;                 // 与本段不相邻
                        const uint64_t end = tail < ceil ? tail : ceil;
                        if (end > reach) { reach = end; grew = true; }
                    }
                    if (!grew) break;
                }
                const uint32_t span = reach > begin ? static_cast<uint32_t>(reach - begin) : 0;
                const uint32_t capped = span < limit ? span : limit;
                return capped & ~(sectorBytes - 1);
            }

            // 挑弹跳窗口：从 kBounceBegin 起按窗口大小步进，取第一段整段可用的；找不到返回 0
            uint32_t FindBounceWindow(uint32_t sectorBytes) {
                for (uint32_t base = kBounceBegin; base + kBounceBytes <= kBounceEnd; base += kBounceBytes) {
                    if (UsableSpan(base, kBounceBytes, sectorBytes) >= kBounceBytes) return base;
                }
                return 0;
            }

            // 读取并校验描述符区：头部定段序列起点，按段长遍历、按标记找 Core 段
            // 描述符区扇区号由 IPL 经 ESI 交来（本地单位）；段内位置字段是 512 字节基准，先换算成本地 LBA
            const char* ReadCoreDescriptor(uint32_t sectorBytes, uint32_t descriptorLba,
                                           uint32_t& fileLba, uint32_t& fileSectors) {
                const uint32_t shift = BaseShift(sectorBytes);
                if (_Bios_Read_Sectors(descriptorLba, 1, s_sector, sectorBytes) == 0) return "Read the CoreDescriptor";
                if (Get32(s_sector) != kAreaMagic) return "The CoreDescriptor magic doesn't match";
                if (Get16(s_sector + kFieldAreaHeadBytes) < kAreaHeadLen) return "The CoreDescriptor head is too short";
                if ((Get32(s_sector + kFieldAreaLba) >> shift) != descriptorLba) return "The CoreDescriptor area sector doesn't match";
                if ((Get32(s_sector + kFieldAreaEndLba) >> shift) < descriptorLba) return "The CoreDescriptor area range is inverted";
                // 段序列：从头部之后开始，按段长前进，按标记认段
                uint32_t offset = Get16(s_sector + kFieldAreaHeadBytes);
                for (;;) {
                    if (offset + kSegmentHeadBytes > sectorBytes) return "The CoreDescriptor has no core segment";
                    const uint32_t length = Get16(s_sector + offset + kFieldSegmentLength);
                    if (length < kSegmentHeadBytes) return "The CoreDescriptor segment length is too small";
                    if (Get32(s_sector + offset) == kCoreMagic) {
                        // 段长必须容下起止扇区，读取方按段内字段取位置
                        if (length < kSegmentHeadBytes + 8) return "The CoreDescriptor segment length is too small";
                        break;
                    }
                    offset += length;
                }
                fileLba = Get32(s_sector + offset + kFieldSegmentLba) >> shift;
                const uint32_t endLba = Get32(s_sector + offset + kFieldSegmentEndLba) >> shift;
                if (fileLba == 0) return "The CoreDescriptor LBA is zero";
                if (endLba < fileLba) return "The CoreDescriptor range is inverted";
                if (fileLba <= descriptorLba) return "The CoreDescriptor overlaps the descriptor sector";
                fileSectors = endLba - fileLba + 1;
                if (fileSectors > kCoreMaxBytes / sectorBytes) return "The CoreImageBytes is zero or beyond the limit";
                return nullptr;
            }

            // 读 Core 文件首扇区并校验镜像头：头必须先于整段装载核对，装载区与入口都取自头字段
            // 头里的文件长度必须与描述符的扇区数一致：一个来自组装布局、一个来自镜像内容，分歧即拒绝
            const char* ReadCoreHeader(uint32_t sectorBytes, uint32_t fileLba, uint32_t fileSectors, ImageFacts& facts) {
                if (_Bios_Read_Sectors(fileLba, 1, s_sector, sectorBytes) == 0) return "Read the BaleenCore header";
                if (const char* reason = CheckImageHeader(s_sector, kCoreImageMagic, facts)) return reason;
                if (((facts.imageBytes + (sectorBytes - 1)) >> SectorShift(sectorBytes)) != fileSectors) return "The CoreDescriptor sector count doesn't match the core header";
                if (facts.memoryBytes > kCoreMaxMemory) return "The CoreMemoryBytes is beyond the limit";
                return nullptr;
            }

            // 把 Core 读进高位：每轮读不超过弹跳窗口的一段，再从低地址整段拷上去
            // 段内都是整扇区，读盘按起始 LBA 逐段递进，不做字节与扇区的换算
            bool ReadCoreImage(uint32_t sectorBytes, uint32_t bounce, uint32_t fileLba, uint32_t bytes) {
                const uint32_t shift = SectorShift(sectorBytes);
                const uint32_t bounceSectors = kBounceBytes >> shift;
                uint32_t lba = fileLba;
                for (uint32_t done = 0; done < bytes; done += kBounceBytes, lba += bounceSectors) {
                    const uint32_t remain = bytes - done;
                    const uint32_t chunk = remain < kBounceBytes ? remain : kBounceBytes;
                    if (_Bios_Read_Sectors(lba, chunk >> shift, reinterpret_cast<void*>(bounce), sectorBytes) == 0) return false;
                    CopyUp(reinterpret_cast<uint8_t*>(kCoreLoad + done), reinterpret_cast<uint8_t*>(bounce), chunk);
                }
                return true;
            }

            // 填交权块：介质事实、内存图位置、Core 装载信息与固件服务入口
            void FillHandoff(uint32_t drive, uint32_t media, uint32_t sectorBytes, uint32_t coreBytes, uint32_t coreSectors) {
                s_handoff.magic = kHandoffMagic;
                s_handoff.version = kHandoffVersion;
                s_handoff.bytes = sizeof(CoreHandoff);
                s_handoff.drive = drive;
                s_handoff.media = media;
                s_handoff.sectorBytes = sectorBytes;
                s_handoff.coreBytes = coreBytes;
                s_handoff.coreSectors = coreSectors;
                s_handoff.coreLoad = kCoreLoad;
                s_handoff.memoryMap = s_memoryMap;
                s_handoff.memoryMapCount = s_memoryMapCount;
                s_handoff.memoryMapTruncated = s_memoryMapTruncated;
                s_handoff.write = _Stub_Write;
                s_handoff.readSectors = _Bios_Read_Sectors;
                s_handoff.sectorSize = _Bios_Sector_Size;
                s_handoff.memoryMapQuery = _Bios_E820;
            }
        }

        // 取内存图到静态缓冲，返回条数
        uint32_t QueryMemoryMap(uint32_t& truncated) {
            truncated = 0;
            s_memoryMapCount = _Bios_E820(s_memoryMap, kMemoryMapCapacity, &truncated);
            s_memoryMapTruncated = truncated;
            return s_memoryMapCount;
        }

        // 读取并校验 CoreDescriptor 与 Core 镜像头、校验装载区、挑弹跳窗口，并填好交权块
        const char* PrepareCore(uint32_t drive, uint32_t media, uint32_t sectorBytes, uint32_t descriptorLba, CoreLoadPlan& plan) {
            uint32_t fileLba = 0;
            uint32_t fileSectors = 0;
            if (const char* reason = ReadCoreDescriptor(sectorBytes, descriptorLba, fileLba, fileSectors)) return reason;
            ImageFacts facts;
            if (const char* reason = ReadCoreHeader(sectorBytes, fileLba, fileSectors, facts)) return reason;
            // 读入跨度按扇区上取整：尾部填充也会写进内存，Core 不得把它当成自己的内容
            const uint32_t span = fileSectors << SectorShift(sectorBytes);
            // 读入跨度与头声明的静态内存跨度都必须整段可用：内存图是唯一依据，不假定 1MiB 以上一定可写
            const uint32_t needBytes = facts.memoryBytes > span ? facts.memoryBytes : span;
            if (!RegionUsable(kCoreLoad, needBytes)) return "The BaleenCore load area isn't usable memory";
            const uint32_t bounce = FindBounceWindow(sectorBytes);
            if (bounce == 0) return "Find a bounce window below 1MiB";

            plan.fileLba = fileLba;
            plan.imageBytes = facts.imageBytes;
            plan.readBytes = span;
            plan.memoryBytes = facts.memoryBytes;
            plan.entryOffset = facts.entryOffset;
            plan.buildId = facts.buildId;
            plan.loadAddress = kCoreLoad;
            plan.bounceAddress = bounce;
            FillHandoff(drive, media, sectorBytes, facts.imageBytes, fileSectors);
            return nullptr;
        }

        // 按 plan 把 Core 读进高位，再清零头声明的未落盘尾部
        const char* ReadCore(const CoreLoadPlan& plan, uint32_t sectorBytes) {
            if (!ReadCoreImage(sectorBytes, plan.bounceAddress, plan.fileLba, plan.readBytes)) return "Read BaleenCore";
            // BSS 由 Stub 清零：Core 不得依赖扇区填充或残留内容，尾部填充本身不属于 Core 内容
            ClearBytes(reinterpret_cast<uint8_t*>(plan.loadAddress + plan.imageBytes), plan.memoryBytes - plan.imageBytes);
            return nullptr;
        }

        // 比对整幅 Core 镜像的摘要：头与描述符已经核对通过，这里只核对内容与头里的 Digest
        const char* CheckCore(const CoreLoadPlan& plan) {
            return CheckImageDigest(reinterpret_cast<const uint8_t*>(plan.loadAddress), plan.imageBytes);
        }

        // 交权：描述符、镜像头与摘要都已核对，入口取头声明的 EntryOffset，此后由 Core 接管
        [[noreturn]] void EnterCore(const CoreLoadPlan& plan) {
            _Stub_Enter_Core(&s_handoff, plan.loadAddress + plan.entryOffset);
        }
    }
}
