/* BootMemory.cpp
    BootMemory.hpp 的实现：内存图可用性判定与弹跳窗口搜索
*/

#include <BootMemory.hpp>

namespace Baleen {
    bool SpanUsable(const Boot::MemoryMapEntry* memoryMap, uint32_t count, uint64_t begin, uint64_t bytes) {
        if (memoryMap == nullptr || count == 0 || bytes == 0) return false;
        const uint64_t end = begin + bytes;
        if (end < begin) return false;
        // 条目顺序不作假定：每轮沿可用条目把已覆盖的右端向外推到不动点，最多 count 轮
        uint64_t reach = begin;
        for (uint32_t round = 0; round < count; ++round) {
            bool grew = false;
            for (uint32_t i = 0; i < count; ++i) {
                const Boot::MemoryMapEntry& entry = memoryMap[i];
                if (entry.type != 1) continue;
                const uint64_t entryEnd = entry.base + entry.length;
                if (entryEnd < entry.base || entryEnd <= begin) continue;   // 长度回绕或整条落在区间之下
                if (entry.base > reach) continue;                           // 与本段不相邻，覆盖被截断
                const uint64_t capped = entryEnd < end ? entryEnd : end;
                if (capped > reach) { reach = capped; grew = true; }
            }
            if (!grew || reach >= end) break;
        }
        return reach >= end;
    }

    uint32_t FindBounceWindow(const Boot::MemoryMapEntry* memoryMap, uint32_t count, uint32_t begin, uint32_t end, uint32_t windowBytes) {
        if (windowBytes == 0 || end < begin || end - begin < windowBytes) return 0;
        // base 逐窗口步进，末端不会越过 end，回绕不可能发生
        const uint32_t last = end - windowBytes;
        for (uint32_t base = begin; base <= last; base += windowBytes) {
            if (SpanUsable(memoryMap, count, base, windowBytes)) return base;
        }
        return 0;
    }
}
