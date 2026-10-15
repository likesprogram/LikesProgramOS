/* LoadCore.cpp
    Stub 的 BaleenCore 装载机制实现：读盘自检、读描述符与镜像头、查内存图、挑弹跳窗口、高位拷贝、摘要校验、填交权块

    镜像头先于整段装载核对：装载区大小、入口与静态内存跨度都取自头字段；整段读入后再比对摘要
    读盘只能落低 1MiB，高位装载因此分两步：先读进低地址弹跳窗口，再用 32 位平坦段整段拷上去
    这条路不引入 unreal 模式，也不要求固件认识高位缓冲；窗口本身按内存图挑，不假定固定地址可用
    本文件不产生诊断输出，也不决定步骤顺序：失败只回报带编号的原因，打印与停机都由 _Stub_Main 负责
*/

#include <stdint.h>

#include <Bios.hpp>
#include <BootInfo.hpp>
#include <BootMemory.hpp>
#include <CoreHandoff.hpp>
#include <Diagnostics.hpp>
#include <ImageHeader.hpp>
#include <LoadCore.hpp>
#include <ReadFailure.hpp>

// 由 Stub.asm 提供：把 ESI 置为交权块、跳到 BaleenCore 入口后不返回
extern "C" [[noreturn]] void _Stub_Enter_Core(const Baleen::CoreHandoff* handoff, uintptr_t entry);
// 交权块暴露给 BaleenCore 的控制台入口，由 Stub.cpp 提供；与三个 _Bios_* 一样是外部符号
extern "C" void _Stub_Write(const char* text);

namespace Baleen {
    namespace Stub {
        namespace {
            // —— 描述符区布局：与 Packages/Baleen/Common/Include/Desc.inc 一致 ——
            // 汇编侧由 %INCLUDE 取 .inc，C++ 侧包含不了 .inc，下面的常量只能在此重申；
            // 交权块的标记、版本与长度不在这里重申，直接取 CoreHandoffLayout（它按 Contract.inc 断言）
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
            constexpr uint32_t kCoreMagic = 0x52444342;         // 'BCDR'，BaleenCore 段标记
            constexpr uint32_t kCoreLoad = 0x100000;            // BaleenCore 装入的物理地址，即镜像基址
            constexpr uint32_t kCoreMaxBytes = 0x400000;        // BaleenCore 文件字节数上限
            constexpr uint32_t kCoreMaxMemory = 0x400000;       // BaleenCore 静态内存跨度上限，含未落盘尾部

            // 本地扇区大小上限：探测只认 512、2048、4096，缓冲按上限预留
            constexpr uint32_t kMaxSectorBytes = 4096;
            // 内存图缓冲条目数，与 Bios.asm 的 E820_MAX 一致
            constexpr uint32_t kMemoryMapCapacity = 64;

            // —— 本阶段失败原因的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailReadDescriptor{"E-STUB-20", "Read the CoreDescriptor"};
            constexpr Diagnostic kFailDescriptorMagic{"E-STUB-21", "The CoreDescriptor magic doesn't match"};
            constexpr Diagnostic kFailDescriptorHead{"E-STUB-22", "The CoreDescriptor head is too short"};
            constexpr Diagnostic kFailDescriptorArea{"E-STUB-23", "The CoreDescriptor area sector doesn't match"};
            constexpr Diagnostic kFailDescriptorInverted{"E-STUB-24", "The CoreDescriptor area range is inverted"};
            constexpr Diagnostic kFailDescriptorNoSegment{"E-STUB-25", "The CoreDescriptor has no BaleenCore segment"};
            constexpr Diagnostic kFailSegmentHead{"E-STUB-26", "The CoreDescriptor segment length is too small"};
            constexpr Diagnostic kFailSegmentRange{"E-STUB-27", "The CoreDescriptor segment length can't hold the range"};
            constexpr Diagnostic kFailDescriptorLbaZero{"E-STUB-28", "The CoreDescriptor LBA is zero"};
            constexpr Diagnostic kFailDescriptorRange{"E-STUB-29", "The CoreDescriptor range is inverted"};
            constexpr Diagnostic kFailDescriptorOverlap{"E-STUB-30", "The CoreDescriptor overlaps the descriptor sector"};
            constexpr Diagnostic kFailCoreBytesLimit{"E-STUB-31", "The BaleenCore image bytes are zero or beyond the limit"};
            constexpr Diagnostic kFailReadHeader{"E-STUB-32", "Read the BaleenCore header"};
            constexpr Diagnostic kFailSectorCount{"E-STUB-33", "The CoreDescriptor sector count doesn't match the BaleenCore header"};
            constexpr Diagnostic kFailMemoryLimit{"E-STUB-34", "The BaleenCore memory bytes are beyond the limit"};
            constexpr Diagnostic kFailLoadArea{"E-STUB-35", "The BaleenCore load area isn't usable memory"};
            constexpr Diagnostic kFailBounceWindow{"E-STUB-36", "Find a bounce window below 1MiB"};
            constexpr Diagnostic kFailReadCore{"E-STUB-37", "Read BaleenCore"};

            // 读盘自检、描述符与镜像头共用的扇区缓冲：整扇区读入，16 字节对齐是 EDD 的 DAP 前提
            uint8_t s_sector[kMaxSectorBytes] __attribute__((aligned(16)));
            // 内存图静态缓冲：交权块把它交给 BaleenCore，不能放在栈上
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

            // 把一段内存清零：交权前清零 BaleenCore 的未落盘尾部，平坦段下按物理地址直接写
            void ClearBytes(uint8_t* dest, uint32_t bytes) {
                if (bytes == 0) return;
                asm volatile("cld; rep stosb" : "+D"(dest), "+c"(bytes) : "a"(0) : "memory", "cc");
            }

            // 读取并校验描述符区：头部定段序列起点，按段长遍历、按标记找 BaleenCore 段
            // 描述符区扇区号由 IPL 经 ESI 交来（本地单位）；段内位置字段是 512 字节基准，先换算成本地 LBA
            Diagnostic ReadCoreDescriptor(uint32_t sectorBytes, uint32_t descriptorLba, uint32_t& fileLba, uint32_t& fileSectors) {
                const uint32_t shift = BaseShift(sectorBytes);
                if (_Bios_Read_Sectors(descriptorLba, 1, s_sector, sectorBytes) == 0) return kFailReadDescriptor;
                if (Get32(s_sector) != kAreaMagic) return kFailDescriptorMagic;
                if (Get16(s_sector + kFieldAreaHeadBytes) < kAreaHeadLen) return kFailDescriptorHead;
                if ((Get32(s_sector + kFieldAreaLba) >> shift) != descriptorLba) return kFailDescriptorArea;
                if ((Get32(s_sector + kFieldAreaEndLba) >> shift) < descriptorLba) return kFailDescriptorInverted;
                // 段序列：从头部之后开始，按段长前进，按标记认段
                uint32_t offset = Get16(s_sector + kFieldAreaHeadBytes);
                for (;;) {
                    if (offset + kSegmentHeadBytes > sectorBytes) return kFailDescriptorNoSegment;
                    const uint32_t length = Get16(s_sector + offset + kFieldSegmentLength);
                    if (length < kSegmentHeadBytes) return kFailSegmentHead;
                    if (Get32(s_sector + offset) == kCoreMagic) {
                        // 段长必须容下起止扇区，读取方按段内字段取位置
                        if (length < kSegmentHeadBytes + 8) return kFailSegmentRange;
                        break;
                    }
                    offset += length;
                }
                fileLba = Get32(s_sector + offset + kFieldSegmentLba) >> shift;
                const uint32_t endLba = Get32(s_sector + offset + kFieldSegmentEndLba) >> shift;
                if (fileLba == 0) return kFailDescriptorLbaZero;
                if (endLba < fileLba) return kFailDescriptorRange;
                if (fileLba <= descriptorLba) return kFailDescriptorOverlap;
                fileSectors = endLba - fileLba + 1;
                if (fileSectors > kCoreMaxBytes / sectorBytes) return kFailCoreBytesLimit;
                return Diagnostic{};
            }

            // 读 BaleenCore 文件首扇区并校验镜像头：头必须先于整段装载核对，装载区与入口都取自头字段
            // 头里的文件长度必须与描述符的扇区数一致：一个来自组装布局、一个来自镜像内容，分歧即拒绝
            Diagnostic ReadCoreHeader(uint32_t sectorBytes, uint32_t fileLba, uint32_t fileSectors, ImageFacts& facts) {
                if (_Bios_Read_Sectors(fileLba, 1, s_sector, sectorBytes) == 0) return kFailReadHeader;
                if (const Diagnostic reason = CheckImageHeader(s_sector, kCoreImageMagic, facts, ImageDiagStage::Stub); reason.code != nullptr) return reason;
                if (((facts.imageBytes + (sectorBytes - 1)) >> SectorShift(sectorBytes)) != fileSectors) return kFailSectorCount;
                if (facts.memoryBytes > kCoreMaxMemory) return kFailMemoryLimit;
                return Diagnostic{};
            }

            // 把 BaleenCore 读进高位：每轮读不超过弹跳窗口的一段，再从低地址整段拷上去
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

            // 填交权块：介质事实、内存图位置、BaleenCore 装载信息、固件服务入口、读盘失败记录与引导计时
            void FillHandoff(uint32_t drive, uint32_t media, uint32_t sectorBytes, uint32_t coreBytes, uint32_t coreSectors, uint64_t bootTsc, uint32_t ticksPerMs) {
                s_handoff.magic = CoreHandoffLayout::kMagic;
                s_handoff.version = CoreHandoffLayout::kVersion;
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
                s_handoff.writeSectors = _Bios_Write_Sectors;
                s_handoff.sectorSize = _Bios_Sector_Size;
                s_handoff.memoryMapQuery = _Bios_E820;
                s_handoff.lastReadFailure = &_Bios_Read_Failure;
                s_handoff.bootTscLow = static_cast<uint32_t>(bootTsc);
                s_handoff.bootTscHigh = static_cast<uint32_t>(bootTsc >> 32);
                s_handoff.ticksPerMs = ticksPerMs;
            }
        }

        // 读盘自检：读 0 号扇区并核对引导签名，缓冲与描述符、镜像头的读取共用
        // sectorBytes 是 IPL 会话单位；签名只在 512 字节介质上有意义，其余介质不参与判定
        DiskProbeFacts ProbeBootSector(uint32_t sectorBytes) {
            DiskProbeFacts facts;
            facts.readOk = _Bios_Read_Sectors(0, 1, s_sector, sectorBytes);
            facts.signatureOk = facts.readOk != 0 && s_sector[510] == 0x55 && s_sector[511] == 0xAA;
            return facts;
        }

        // 取内存图到静态缓冲，返回条数
        uint32_t QueryMemoryMap(uint32_t& truncated) {
            truncated = 0;
            s_memoryMapCount = _Bios_E820(s_memoryMap, kMemoryMapCapacity, &truncated);
            s_memoryMapTruncated = truncated;
#ifdef STUB_WINDOW_PROBE
            // 弹跳窗口探针：把查询结果换成一张注入的内存图，让 [kBounceBegin, kBounceEnd)
            // 整段不可用，走"找不到弹跳窗口"的失败路径（E-STUB-36）；只服务回归，正式构建不编进这段
            // 低 64KiB 与 1MiB 以上保持可用：装载区校验先于窗口搜索，先失败在别处就不是这条路径
            s_memoryMap[0] = Boot::MemoryMapEntry{ 0x000000, 0x010000, 1, 1 };
            s_memoryMap[1] = Boot::MemoryMapEntry{ kBounceBegin, kBounceEnd - kBounceBegin, 2, 1 };
            s_memoryMap[2] = Boot::MemoryMapEntry{ kBounceEnd, 0x20000, 1, 1 };
            s_memoryMap[3] = Boot::MemoryMapEntry{ 0x100000, 0x3F00000, 1, 1 };
            s_memoryMapCount = 4;
            s_memoryMapTruncated = 0;
            truncated = 0;
#endif
            return s_memoryMapCount;
        }

        // 读取并校验 CoreDescriptor 与 BaleenCore 镜像头、校验装载区、挑弹跳窗口，并填好交权块
        Diagnostic PrepareCore(uint32_t drive, uint32_t media, uint32_t sectorBytes, uint32_t descriptorLba, uint64_t bootTsc, uint32_t ticksPerMs, CoreLoadPlan& plan) {
            uint32_t fileLba = 0;
            uint32_t fileSectors = 0;
            if (const Diagnostic reason = ReadCoreDescriptor(sectorBytes, descriptorLba, fileLba, fileSectors); reason.code != nullptr) return reason;
            ImageFacts facts;
            if (const Diagnostic reason = ReadCoreHeader(sectorBytes, fileLba, fileSectors, facts); reason.code != nullptr) return reason;
            // 读入跨度按扇区上取整：尾部填充也会写进内存，BaleenCore 不得把它当成自己的内容
            const uint32_t span = fileSectors << SectorShift(sectorBytes);
            // 读入跨度与头声明的静态内存跨度都必须整段可用：内存图是唯一依据，不假定 1MiB 以上一定可写
            // 判定与 BaleenCore 侧共用 SpanUsable：相邻可用条目合并，两阶段不会对同一张图给出不同结论
            const uint32_t needBytes = facts.memoryBytes > span ? facts.memoryBytes : span;
            if (!SpanUsable(s_memoryMap, s_memoryMapCount, kCoreLoad, needBytes)) return kFailLoadArea;
            const uint32_t bounce = FindBounceWindow(s_memoryMap, s_memoryMapCount, kBounceBegin, kBounceEnd, kBounceBytes);
            if (bounce == 0) return kFailBounceWindow;

            plan.fileLba = fileLba;
            plan.imageBytes = facts.imageBytes;
            plan.readBytes = span;
            plan.memoryBytes = facts.memoryBytes;
            plan.entryOffset = facts.entryOffset;
            plan.buildId = facts.buildId;
            plan.loadAddress = kCoreLoad;
            plan.bounceAddress = bounce;
            FillHandoff(drive, media, sectorBytes, facts.imageBytes, fileSectors, bootTsc, ticksPerMs);
            return Diagnostic{};
        }

        // 按 plan 把 BaleenCore 读进高位，再清零头声明的未落盘尾部
        Diagnostic ReadCore(const CoreLoadPlan& plan, uint32_t sectorBytes) {
            if (!ReadCoreImage(sectorBytes, plan.bounceAddress, plan.fileLba, plan.readBytes)) return kFailReadCore;
            // BSS 由 Stub 清零：BaleenCore 不得依赖扇区填充或残留内容，尾部填充本身不属于 BaleenCore 内容
            ClearBytes(reinterpret_cast<uint8_t*>(plan.loadAddress + plan.imageBytes), plan.memoryBytes - plan.imageBytes);
            return Diagnostic{};
        }

        // 比对整幅 BaleenCore 镜像的摘要：头与描述符已经核对通过，这里只核对内容与头里的 Digest
        Diagnostic CheckCore(const CoreLoadPlan& plan) {
            return CheckImageDigest(reinterpret_cast<const uint8_t*>(plan.loadAddress), plan.imageBytes, ImageDiagStage::Stub);
        }

        // 交权：描述符、镜像头与摘要都已核对，入口取头声明的 EntryOffset，此后由 BaleenCore 接管
        [[noreturn]] void EnterCore(const CoreLoadPlan& plan) {
            _Stub_Enter_Core(&s_handoff, plan.loadAddress + plan.entryOffset);
        }
    }
}
