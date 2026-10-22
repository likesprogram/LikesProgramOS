/* BootMemory.hpp
    引导期内存图查询：区间可用性判定与低地址弹跳窗口搜索

    判定只有一份：区间可用看内存图里 type==1 的条目是否覆盖它，相邻的可用条目按同一段
    连续内存合并，窗口因此可以跨条目；Stub 与 BaleenCore 曾各写一份，语义分歧会让同一台
    机器上一个阶段找得到窗口、另一个阶段找不到
    本文件只做机制：不打印、不停机，调用方按返回值处理

    窗口范围与大小是 Stub 装载与 BaleenCore 读盘的共同策略，由本文件的常量统一给出
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>

namespace Baleen {
    // 低地址弹跳窗口：读盘服务只接受低 1MiB 的目的地，从 kBounceBegin 起按 kBounceBytes
    // 步进，取第一段整段可用的窗口；kBounceEnd 为搜索上端，不含
    constexpr uint32_t kBounceBegin = 0x10000;   // 搜索起点
    constexpr uint32_t kBounceEnd = 0x80000;     // 搜索上端，不含
    constexpr uint32_t kBounceBytes = 0x10000;   // 窗口大小，也是步进

    // 区间 [begin, begin + bytes) 是否整段落在内存图的可用区里
    // 相邻的可用条目合并覆盖同一个区间；bytes 为 0、区间回绕或条目长度回绕都判不可用
    bool SpanUsable(const Boot::MemoryMapEntry* memoryMap, uint32_t count, uint64_t begin, uint64_t bytes);

    // 在 [begin, end) 内按 windowBytes 整块划分，取第一段整段可用的窗口；找不到返回 0
    // 范围容不下一个窗口、windowBytes 为 0 或范围倒置都返回 0，不搜索
    uint32_t FindBounceWindow(const Boot::MemoryMapEntry* memoryMap, uint32_t count, uint32_t begin, uint32_t end, uint32_t windowBytes);
}
