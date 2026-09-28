/* Bios.hpp
    Stub 暴露的 BIOS 服务声明：实模式固件调用在保护模式下的入口

    实现在 Bios.asm：从 32 位保护模式弹回实模式执行固件调用再回到保护模式，
    调用期间关中断且不允许嵌套；所有调用都按 cdecl 约定，返回值在 EAX
*/

#pragma once
#include <stdint.h>

#include "BootInfo.hpp"

// 探测启动驱动器的扇区大小，即 INT 13h AH=48 返回的每扇区字节数
// 驱动器号取 IPL 交权时保存的那个；只认 512、2048、4096，其余情况返回 0
extern "C" uint32_t _Bios_Sector_Size();

// 取 E820 内存图：最多写 max_count 条到 entries，返回写入条数
// 缓冲放不下而固件还有后续条目时，把 1 写进 truncated（可为空指针）
extern "C" uint32_t _Bios_E820(Boot::MemoryMapEntry* entries, uint32_t max_count, uint32_t* truncated);

// 读盘：从 lba 起读 count 个扇区到 dest，成功返回 1，失败返回 0
// dest 须 16 字节对齐且整段落低 1MiB，且每批不跨 64KiB 窗口；块数超过一次传输上限时内部分批
extern "C" uint32_t _Bios_Read_Sectors(uint32_t lba, uint32_t count, void* dest, uint32_t sect_bytes);
