/* Libc.hpp
    模块侧的最小 C 运行库声明：无宿主运行库环境下由 Libc.cpp 提供

    -ffreestanding 下编译器仍可能为目标代码生成 memcpy/memset 等调用，而模块链接带
    --no-undefined，所以每个模块包都要把这份实现编进来（Common/ModuleRules.mk 已含）
    符号是 hidden：模块只允许导出三个入口与契约表，这些函数不是模块接口
*/

#pragma once
#include <stddef.h>

extern "C" {
    // 拷贝不重叠的一段内存，返回 dest
    __attribute__((visibility("hidden"))) void* memcpy(void* dest, const void* source, size_t bytes);
    // 填充一段内存，返回 dest
    __attribute__((visibility("hidden"))) void* memset(void* dest, int value, size_t bytes);
    // 拷贝可能重叠的一段内存，返回 dest
    __attribute__((visibility("hidden"))) void* memmove(void* dest, const void* source, size_t bytes);
    // 比较两段内存：按无符号字节，返回首个不同字节的差或 0
    __attribute__((visibility("hidden"))) int memcmp(const void* left, const void* right, size_t bytes);
    // 求字符串长度，不含结尾零
    __attribute__((visibility("hidden"))) size_t strlen(const char* text);
}
