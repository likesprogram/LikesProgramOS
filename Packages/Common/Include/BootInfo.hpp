/* BootInfo.hpp
    引导交接契约 BootInfo 的结构定义：引导期各阶段之间传递的介质与内存事实
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Boot {
    // 启动介质类型：取值与 IPL 交权时 DH 的编码一致
    enum class Media : uint32_t {
        Hdd = 2,        // 硬盘 / USB-HDD
        Cdrom = 4,      // 光盘
    };

    // E820 内存图条目：固件给出的一段物理区间，字段与固件的 24 字节条目一一对应
    // 属性字段不是可有可无的填充：32 位目标上 uint64_t 只按 4 字节对齐，少了它结构体会缩到
    // 20 字节，与固件写入的 24 字节步长错位，数组从第二条起全部读错
    struct MemoryMapEntry {
        uint64_t base;          // 区间起址，含
        uint64_t length;        // 区间长度，字节
        uint32_t type;          // 区间类型，1 为可用
        uint32_t attributes;    // ACPI 3.0 起的扩展属性，bit0 为 0 表示该条目应被忽略
    };

    // 条目布局是固件与各阶段之间的约定，长度与类型字段偏移在编译期固定
    static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry 必须与固件的 24 字节 E820 条目同长");
    static_assert(offsetof(MemoryMapEntry, type) == 16, "MemoryMapEntry 的类型字段必须落在 E820 条目的偏移 16");
}
