/* PhysicalMemory.hpp
    按物理地址读取低端内存的字节：BDA 一类固定物理地址的访问方式

    地址经 asm 空语句阻断编译器的常量地址推断：直接对接近零的常量地址做下标，
    编译器会按零长对象做数组越界推断（-Warray-bounds），经不透明指针访问可避开
    本文件只做访问机制：调用方保证地址在当前地址空间里可直接访问
*/

#pragma once
#include <stdint.h>

namespace Platform {
    // 物理地址处的只读字节指针；阻断常量折叠后可以安全下标
    inline const volatile uint8_t* PhysicalBytePointer(uintptr_t address) {
        const volatile uint8_t* pointer = reinterpret_cast<const volatile uint8_t*>(address);
        asm volatile("" : "+r"(pointer));
        return pointer;
    }

    // 读物理地址处的一个字节
    inline uint8_t ReadPhysicalByte(uintptr_t address) { return *PhysicalBytePointer(address); }
}
