/* Libc.cpp
    模块侧的最小 C 运行库实现：只提供目标侧代码实际引用到的函数

    与 Packages/LikesProgramOS/Core/Src/Libc.cpp 各自独立：内核与模块是两份构建配置，
    共享一份实现只会把两个包绑在一起；函数语义按 C 标准，代码足够短，重复不构成维护负担
    不调用任何库函数，也不用内联汇编：编译器在 -O2 下不会把这里的循环识别成对同名函数的
    调用（-ftree-loop-distribute-patterns 在 -O3 才开），构建后的 nm -u 为空是硬约束
*/

#include <Libc.hpp>

#include <stdint.h>

extern "C" {
    void* memcpy(void* dest, const void* source, size_t bytes) {
        uint8_t* to = static_cast<uint8_t*>(dest);
        const uint8_t* from = static_cast<const uint8_t*>(source);
        for (size_t i = 0; i < bytes; ++i) to[i] = from[i];
        return dest;
    }

    void* memset(void* dest, int value, size_t bytes) {
        uint8_t* to = static_cast<uint8_t*>(dest);
        for (size_t i = 0; i < bytes; ++i) to[i] = static_cast<uint8_t>(value);
        return dest;
    }

    void* memmove(void* dest, const void* source, size_t bytes) {
        uint8_t* to = static_cast<uint8_t*>(dest);
        const uint8_t* from = static_cast<const uint8_t*>(source);
        if (to < from) {
            for (size_t i = 0; i < bytes; ++i) to[i] = from[i];
        } else if (to > from) {
            for (size_t i = bytes; i > 0; --i) to[i - 1] = from[i - 1];
        }
        return dest;
    }

    int memcmp(const void* left, const void* right, size_t bytes) {
        const uint8_t* a = static_cast<const uint8_t*>(left);
        const uint8_t* b = static_cast<const uint8_t*>(right);
        for (size_t i = 0; i < bytes; ++i) {
            if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        }
        return 0;
    }

    size_t strlen(const char* text) {
        size_t bytes = 0;
        while (text[bytes] != '\0') ++bytes;
        return bytes;
    }
}
