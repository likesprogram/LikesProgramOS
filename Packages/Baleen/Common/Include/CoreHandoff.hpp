/* CoreHandoff.hpp
    Stub 交给 BaleenCore 的交权块：介质事实、内存图与 Stub 提供的固件服务入口

    这块是两份二进制之间的 ABI：Stub 在跳转前填好，入口时 ESI 指向它，BaleenCore 按固定偏移读取
    字段布局改动必须两边同时改，格式标记与长度见同目录的 Contract.inc
    BaleenCore 不从文件名或寄存器残留推导事实，交权块里没有的项一律由 BaleenCore 自己探测或拒绝
    Stub 交权后冻结并常驻，块、内存图缓冲、读盘失败记录、服务入口与 BaleenCore 自身在 BaleenCore 运行期间一直有效
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <BootInfo.hpp>
#include <ReadFailure.hpp>

namespace Baleen {
    // 交权块的格式标记、长度与字段偏移：C++ 侧用结构体成员，汇编与生成器用这里的常量
    // 与 Contract.inc 的 CORE_HANDOFF_* 是同一组取值，改了任一侧都会被下面的断言拦住
    struct CoreHandoffLayout {
        static constexpr uint32_t kMagic = 0x484E4C42;             // 'BLNH'
        static constexpr uint32_t kVersion = 1;                    // 未发布的开发格式标记
        static constexpr uint32_t kBytes = 84;                     // 结构长度
        static constexpr uint32_t kSectorBytes = 0x14;             // sectorBytes 字段
        static constexpr uint32_t kMemoryMap = 0x2C;               // memoryMap 指针
        static constexpr uint32_t kWrite = 0x30;                   // write 服务入口
        static constexpr uint32_t kReadSectors = 0x34;             // readSectors 服务入口
        static constexpr uint32_t kSectorSize = 0x38;              // sectorSize 服务入口
        static constexpr uint32_t kMemoryMapQuery = 0x3C;          // memoryMapQuery 服务入口
        static constexpr uint32_t kLastReadFailure = 0x40;         // lastReadFailure 指针
        static constexpr uint32_t kBootTscLow = 0x44;              // 进入 Stub 时的时间戳低 32 位
        static constexpr uint32_t kBootTscHigh = 0x48;             // 进入 Stub 时的时间戳高 32 位
        static constexpr uint32_t kTicksPerMs = 0x4C;              // 每毫秒的 TSC 计数；0 表示无计时
        static constexpr uint32_t kWriteSectors = 0x50;            // writeSectors 服务入口
    };

    // 交权块：Stub → BaleenCore 的全部事实与服务，长度固定 84 字节
    // 指针都是 32 位物理地址：交权时数据段是平坦段，分页关闭，虚拟地址等于物理地址
    struct CoreHandoff {
        uint32_t magic;                     // 格式标记，取值 CORE_HANDOFF_MAGIC，'BLNH'
        uint32_t version;                   // 开发格式标记，取值 CORE_HANDOFF_VER
        uint32_t bytes;                     // 本结构长度，BaleenCore 据此核对；取值 CORE_HANDOFF_BYTES
        uint32_t drive;                     // 启动驱动器的 BIOS 号，即 IPL 交权时的 DL
        uint32_t media;                     // 启动介质，取值同 Boot::Media
        uint32_t sectorBytes;               // 介质的本地扇区大小，读盘块数以此为单位
        uint32_t coreBytes;                 // BaleenCore 文件字节数
        uint32_t coreSectors;               // BaleenCore 按扇区上取整后的读入跨度；尾部填充不是 BaleenCore 内容
        uint32_t coreLoad;                  // BaleenCore 映像装入的物理地址，即 CORE_LOAD；本次入口 = 它 + 镜像头里的 EntryOffset
        uint32_t memoryMapCount;            // 内存图条数
        uint32_t memoryMapTruncated;        // 缓冲装不下后续条目时为 1
        const Boot::MemoryMapEntry* memoryMap;   // 内存图，指向 Stub 的静态缓冲
        // 固件服务入口：BaleenCore 在 32 位保护模式下调它们，Stub 负责弹回实模式并还原现场
        // 语义与 Stub 自身使用的一致：读盘不接受 1MiB 以上的目的地，失败返回 0 而不停机
        void (*write)(const char* text);                                      // 写入 Stub 的控制台：VGA 文本、0xE9 与 COM1
        uint32_t (*readSectors)(uint32_t lba, uint32_t count, void* dest, uint32_t sectBytes);   // 读盘，成功返回 1
        uint32_t (*sectorSize)();                                             // 探测启动驱动器的扇区大小
        uint32_t (*memoryMapQuery)(Boot::MemoryMapEntry* entries, uint32_t maxCount, uint32_t* truncated);   // 取内存图
        const ReadFailureFacts* lastReadFailure;                              // 读盘服务的最后一次失败现场，常驻 Stub 映像
        // 引导计时：进入 Stub 时的时间戳与校准值，BaleenCore 用它报出从 Stub 进入起的总耗时
        // 时间戳是自由运行的 TSC；ticksPerMs 为 0 表示 Stub 侧没有可用计时，两个时间戳字段无意义
        uint32_t bootTscLow;                // 进入 Stub 时的时间戳低 32 位
        uint32_t bootTscHigh;               // 进入 Stub 时的时间戳高 32 位
        uint32_t ticksPerMs;                // 每毫秒的 TSC 计数
        // 写盘：把整段字节写回启动介质上的扇区；源须 16 字节对齐并整段落低 1MiB，成功返回 1
        // 只服务引导期的裸块写（引导状态区）；失败即返回 0，由调用方按降级路径处理
        uint32_t (*writeSectors)(uint32_t lba, uint32_t count, const void* src, uint32_t sectBytes);
    };

    // 交权块是 ABI，长度与关键字段偏移在编译期固定；常量改动而这里没跟着改会直接编译失败
    // 只有 32 位的 Stub 与 BaleenCore 受这套布局约束，宿主工具的指针宽度不同，不参与偏移断言
#if UINTPTR_MAX == 0xFFFFFFFFu
    static_assert(sizeof(CoreHandoff) == CoreHandoffLayout::kBytes, "CoreHandoff 长度必须是 84 字节，见 Contract.inc 的 CORE_HANDOFF_BYTES");
    static_assert(offsetof(CoreHandoff, sectorBytes) == CoreHandoffLayout::kSectorBytes, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, memoryMap) == CoreHandoffLayout::kMemoryMap, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, write) == CoreHandoffLayout::kWrite, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, readSectors) == CoreHandoffLayout::kReadSectors, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, sectorSize) == CoreHandoffLayout::kSectorSize, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, memoryMapQuery) == CoreHandoffLayout::kMemoryMapQuery, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, lastReadFailure) == CoreHandoffLayout::kLastReadFailure, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, bootTscLow) == CoreHandoffLayout::kBootTscLow, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, ticksPerMs) == CoreHandoffLayout::kTicksPerMs, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
    static_assert(offsetof(CoreHandoff, writeSectors) == CoreHandoffLayout::kWriteSectors, "CoreHandoff 字段偏移已变，须同步 Stub 与 BaleenCore 两侧");
#endif
}
