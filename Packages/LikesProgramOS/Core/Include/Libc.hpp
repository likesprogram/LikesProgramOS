/* Libc.hpp
    内核侧引用的最小 C 运行库函数：无运行库环境下由 Libc.cpp 提供
*/

#pragma once
#include <stddef.h>

extern "C" {
    // 拷贝不重叠的一段内存，返回 dest
    void* memcpy(void* dest, const void* source, size_t bytes);
    // 填充一段内存，返回 dest
    void* memset(void* dest, int value, size_t bytes);
    // 拷贝可能重叠的一段内存，返回 dest
    void* memmove(void* dest, const void* source, size_t bytes);
    // 比较两段内存：按无符号字节，返回首个不同字节的差或 0
    int memcmp(const void* left, const void* right, size_t bytes);
    // 求字符串长度，不含结尾零
    size_t strlen(const char* text);
}
