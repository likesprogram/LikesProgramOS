/* Paging.hpp
    CheckMemory 的宿主侧替身头：宿主编译内核 Memory.cpp 时取代 Core/Include/Paging.hpp

    内核的 PhysToVirt 加的是直映射窗口基址 0xFFFF800000000000，只在目标侧页表装好之后有效；
    宿主上没有页表，替身默认把「物理地址」当宿主指针；夹具要构造低物理地址（例如把内存图
    数组放到 0x10xxxx 让它的尾页跨页未对齐）时，把 g_hostPhysBias 设成宿主缓冲与假地址之差，
    PhysToVirt 就按 physical + bias 换算，用例结束后必须复位 0
    这份头只提供 Memory.cpp 用到的接口，其余页表接口不在宿主侧检查范围；它不进入任何交付物
*/

#pragma once
#include <stdint.h>

namespace Kernel {
    // 宿主侧直映射窗口基址：恒等映射，实际换算由 g_hostPhysBias 给出
    constexpr uintptr_t kPhysMapBase = 0;

    // PhysToVirt 的宿主侧偏移：0 表示物理地址就是宿主指针；低物理地址夹具临时设为缓冲与假地址之差
    extern uintptr_t g_hostPhysBias;

    // 物理地址到宿主指针的换算：physical + 偏移
    inline void* PhysToVirt(uint64_t physical) { return reinterpret_cast<void*>(physical + g_hostPhysBias); }
}
