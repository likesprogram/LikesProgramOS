/* LoadKernel.hpp
    BaleenCore 的内核镜像检查与装载：定位系统卷内的内核镜像、核对内核镜像数据与整幅摘要、读进装载地址

    机制只做读取、校验与装载，不打印也不停机：失败回报带编号的原因，打印与停机由 Core.cpp 决定
    摘要按覆盖区间把 Digest 字段视作 32 个零，边读边算，只保留一块读取缓冲；
    装载区按交权块的内存图核对，落到不可用区间时在装载之前就拒绝
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <Diagnostics.hpp>
#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        // 校验通过后带出的内核事实
        struct KernelFacts {
            uint32_t imageBytes;       // 文件字节数
            uint32_t memoryBytes;      // 从装载地址起的静态内存跨度
            uint32_t entryOffset;      // 入口的文件偏移
            uint32_t loadAddress;      // 装载物理地址
            uint32_t bootInfoVersion;  // 内核要求的 BootInfo 版本
            uint32_t buildId;          // BuildId 前 4 字节按字节顺序装入的值，仅用于诊断
        };

        // 定位并校验内核镜像：按路径打开并取事实，读首块核对内核镜像数据，流式比对整幅摘要，
        // 再核对装载区落在内存图的可用区间内
        // 成功返回空 code；路径不存在时把 missing 置真并返回空 code；其余失败返回带编号的原因
        Diagnostic CheckKernelImage(Storage::FileSystem& fs, const char* path, const Boot::MemoryMapEntry* memoryMap, uint32_t memoryMapCount, KernelFacts& facts, bool& missing);

        // 把校验过的内核镜像整幅读进装载地址：按文件偏移顺序读满 imageBytes 字节
        // 成功返回空 code；读盘失败或读不满返回带编号的原因
        Diagnostic LoadKernelImage(Storage::FileSystem& fs, const char* path, uint32_t loadAddress, uint32_t imageBytes);
    }
}
