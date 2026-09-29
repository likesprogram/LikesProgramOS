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
            // 描述符的格式标记与头长会拿介质内容实际比对，改动漏了一边会在启动时就暴露
            constexpr uint32_t kDescriptorOffset = 0x340;       // CoreDescriptor 的介质绝对字节偏移
            constexpr uint32_t kDescriptorMagic = 0x52444342;   // 'BCDR'
            constexpr uint16_t kDescriptorVersion = 1;          // 开发格式标记
            constexpr uint16_t kDescriptorHeaderBytes = 32;     // 描述符固定总长
            constexpr uint32_t kFieldVersion = 0x04;            // 描述符内版本字段
            constexpr uint32_t kFieldHeader = 0x06;             // 描述符内头长字段
            constexpr uint32_t kFieldFile = 0x08;               // 描述符内文件偏移字段，8 字节小端
            constexpr uint32_t kFieldBytes = 0x10;              // 描述符内文件字节数字段，8 字节小端
            constexpr uint32_t kFieldReserved = 0x18;           // 描述符内保留字段，8 字节，须全 0
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

            // 从缓冲区按小端读取 64 位
            uint64_t Get64(const uint8_t* field) {
                uint64_t value = 0;
                for (uint32_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(field[i]) << (8 * i);
                return value;
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
            // 取值范围限制在 uint32_t 内：取整用 32 位除法，避免牵入 libgcc 的 __udivdi3
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
                return (capped / sectorBytes) * sectorBytes;
            }

            // 挑弹跳窗口：从 kBounceBegin 起按窗口大小步进，取第一段整段可用的；找不到返回 0
            uint32_t FindBounceWindow(uint32_t sectorBytes) {
                for (uint32_t base = kBounceBegin; base + kBounceBytes <= kBounceEnd; base += kBounceBytes) {
                    if (UsableSpan(base, kBounceBytes, sectorBytes) >= kBounceBytes) return base;
                }
                return 0;
            }

            // 读取并校验 CoreDescriptor：读到 fileOffset 与 imageBytes，成功返回空指针
            // 只读与 BootDescriptor 同一个扇区，字段校验规则与一级引导描述符一致
            // 字段按 64 位读入，校验通过后收窄到 32 位：读盘路径只寻址 32 位偏移，收窄也让后续取整只用 32 位除法
            const char* ReadCoreDescriptor(uint32_t sectorBytes, uint32_t& fileOffset, uint32_t& imageBytes) {
                const uint32_t lba = kDescriptorOffset / sectorBytes;
                const uint32_t within = kDescriptorOffset % sectorBytes;
                // 描述符须整个落在这一扇区内，否则读取缓冲里没有完整字段
                if (within + kDescriptorHeaderBytes > sectorBytes) return "The CoreDescriptor does not fit in one sector";
                if (_Bios_Read_Sectors(lba, 1, s_sector, sectorBytes) == 0) return "Read the CoreDescriptor";
                const uint8_t* const field = s_sector + within;
                if (Get32(field) != kDescriptorMagic) return "The CoreDescriptor magic doesn't match";
                if (Get16(field + kFieldVersion) != kDescriptorVersion || Get16(field + kFieldHeader) != kDescriptorHeaderBytes) return "The CoreDescriptor version or header length isn't supported";
                const uint64_t reserved = Get64(field + kFieldReserved);
                const uint64_t file = Get64(field + kFieldFile);
                const uint64_t bytes = Get64(field + kFieldBytes);
                // 偏移与长度都不得使用高 32 位，偏移还要按扇区对齐并落在描述符扇区之后
                if (reserved != 0 || (file >> 32) != 0 || (bytes >> 32) != 0) return "The CoreDescriptor reserved or high half isn't zero";
                fileOffset = static_cast<uint32_t>(file);
                imageBytes = static_cast<uint32_t>(bytes);
                if (fileOffset == 0 || fileOffset % sectorBytes != 0) return "The CoreDescriptor file offset isn't sector aligned";
                if (fileOffset < (lba + 1) * sectorBytes) return "The CoreDescriptor file offset overlaps the descriptor sector";
                if (imageBytes == 0 || imageBytes > kCoreMaxBytes) return "The CoreImageBytes is zero or beyond the limit";
                // 偏移加长度不得进位：32 位寻址的读盘路径放不下更大的范围
                if (fileOffset + imageBytes < fileOffset) return "The CoreDescriptor file range doesn't fit in 32 bits";
                return nullptr;
            }

            // 读 Core 文件首扇区并校验镜像头：头必须先于整段装载核对，装载区与入口都取自头字段
            // 头里的文件长度必须与描述符一致：一个来自组装布局、一个来自镜像内容，分歧即拒绝
            const char* ReadCoreHeader(uint32_t sectorBytes, uint32_t fileOffset, uint32_t descriptorBytes, ImageFacts& facts) {
                if (_Bios_Read_Sectors(fileOffset / sectorBytes, 1, s_sector, sectorBytes) == 0) return "Read the BaleanCore header";
                if (const char* reason = CheckImageHeader(s_sector, kCoreImageMagic, facts)) return reason;
                if (facts.imageBytes != descriptorBytes) return "The CoreDescriptor file bytes don't match the core header";
                if (facts.memoryBytes > kCoreMaxMemory) return "The CoreMemoryBytes is beyond the limit";
                return nullptr;
            }

            // 把 Core 读进高位：每轮读不超过弹跳窗口的一段，再从低地址整段拷上去
            bool ReadCoreImage(uint32_t sectorBytes, uint32_t bounce, uint32_t fileOffset, uint32_t bytes) {
                for (uint32_t done = 0; done < bytes; done += kBounceBytes) {
                    const uint32_t remain = bytes - done;
                    const uint32_t chunk = remain < kBounceBytes ? remain : kBounceBytes;
                    const uint32_t sectors = chunk / sectorBytes;   // 整扇区；bytes 已按扇区上取整
                    const uint32_t lba = fileOffset / sectorBytes + done / sectorBytes;
                    if (_Bios_Read_Sectors(lba, sectors, reinterpret_cast<void*>(bounce), sectorBytes) == 0) return false;
                    CopyUp(reinterpret_cast<uint8_t*>(kCoreLoad + done), reinterpret_cast<uint8_t*>(bounce), sectors * sectorBytes);
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
        const char* PrepareCore(uint32_t drive, uint32_t media, uint32_t sectorBytes, CoreLoadPlan& plan) {
            uint32_t fileOffset = 0;
            uint32_t imageBytes = 0;
            if (const char* reason = ReadCoreDescriptor(sectorBytes, fileOffset, imageBytes)) return reason;
            ImageFacts facts;
            if (const char* reason = ReadCoreHeader(sectorBytes, fileOffset, imageBytes, facts)) return reason;
            // 读入跨度按扇区上取整：尾部填充也会写进内存，Core 不得把它当成自己的内容
            const uint32_t sectors = (imageBytes + sectorBytes - 1) / sectorBytes;
            const uint32_t span = sectors * sectorBytes;
            // 读入跨度与头声明的静态内存跨度都必须整段可用：内存图是唯一依据，不假定 1MiB 以上一定可写
            const uint32_t needBytes = facts.memoryBytes > span ? facts.memoryBytes : span;
            if (!RegionUsable(kCoreLoad, needBytes)) return "The BaleenCore load area isn't usable memory";
            const uint32_t bounce = FindBounceWindow(sectorBytes);
            if (bounce == 0) return "Find a bounce window below 1MiB";

            plan.fileOffset = fileOffset;
            plan.imageBytes = imageBytes;
            plan.readBytes = span;
            plan.memoryBytes = facts.memoryBytes;
            plan.entryOffset = facts.entryOffset;
            plan.buildId = facts.buildId;
            plan.loadAddress = kCoreLoad;
            plan.bounceAddress = bounce;
            FillHandoff(drive, media, sectorBytes, imageBytes, sectors);
            return nullptr;
        }

        // 按 plan 把 Core 读进高位，再清零头声明的未落盘尾部
        const char* ReadCore(const CoreLoadPlan& plan, uint32_t sectorBytes) {
            if (!ReadCoreImage(sectorBytes, plan.bounceAddress, plan.fileOffset, plan.readBytes)) return "Read BaleanCore";
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
