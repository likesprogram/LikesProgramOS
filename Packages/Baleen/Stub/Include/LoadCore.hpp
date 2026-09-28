/* LoadCore.hpp
    Stub 的 Core 装载机制：CoreDescriptor 读取与校验、装载区检查、高位拷贝与交权

    这里的函数只做机制，既不打印也不停机：诊断行与步骤顺序由 _Stub_Main 决定，日志集中在一处
    实现在 Src/LoadCore.cpp，文件名用「装载 Core」这个动作，避免与 Baleen 的 Core 阶段混同
    CoreDescriptor 的字节表、装入地址与交权块细节见 Packages/Baleen/Stub/README.md 第二节
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    namespace Stub {
        // 装载计划：校验通过后由 PrepareCore 给出，供调用方打印进度行并按图装载
        struct CoreLoadPlan {
            uint32_t fileOffset = 0;     // Core 文件在介质上的绝对字节偏移
            uint32_t imageBytes = 0;     // Core 文件字节数
            uint32_t readBytes = 0;      // 按本地扇区上取整后的读入跨度，尾部填充也会写进内存
            uint32_t loadAddress = 0;    // 装入的物理地址，也是交权入口
            uint32_t bounceAddress = 0;  // 高位拷贝用的低地址弹跳窗口
        };

        // 取内存图到静态缓冲，返回条数；缓冲装不下固件还有的条目时把 1 写进 truncated
        // 内存图同时供装载区检查与交权块使用，因此落在静态缓冲而不是调用方的栈上
        uint32_t QueryMemoryMap(uint32_t& truncated);

        // 读取并校验 CoreDescriptor、校验装载区、挑弹跳窗口，并填好交权块
        // 成功返回空指针并把结果写进 plan，失败返回原因文本：打印与停机由调用方决定
        const char* PrepareCore(uint32_t drive, uint32_t media, uint32_t sectorBytes, CoreLoadPlan& plan);

        // 按 plan 把 Core 读进高位：每轮读不超过弹跳窗口的一段，再从低地址整段拷上去
        // 成功返回空指针，读盘失败返回原因文本
        const char* ReadCore(const CoreLoadPlan& plan, uint32_t sectorBytes);

        // 交权：把 ESI 置为交权块并跳到 Core 入口，不返回
        [[noreturn]] void EnterCore();
    }
}
