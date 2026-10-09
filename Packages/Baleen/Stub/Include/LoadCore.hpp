/* LoadCore.hpp
    Stub 的 BaleenCore 装载机制：CoreDescriptor 与镜像头读取校验、装载区检查、高位拷贝、摘要校验与交权

    这里的函数只做机制，既不打印也不停机：诊断行与步骤顺序由 _Stub_Main 决定，日志集中在一处
    实现在 Src/LoadCore.cpp，文件名用「装载 BaleenCore」这个动作，避免与 Baleen 的 BaleenCore 阶段混同
    CoreDescriptor 的字节表见 Packages/Baleen/Stub/README.md 第二节，镜像头的布局与字段来自
    Packages/Baleen/Common/Include/ImageHeader.hpp，两种镜像同构
*/

#pragma once
#include <stdint.h>

#include <Diagnostics.hpp>

namespace Baleen {
    namespace Stub {
        // 装载计划：校验通过后由 PrepareBaleenCore 给出，供调用方打印进度行并按图装载
        struct CoreLoadPlan {
            uint32_t fileLba = 0;        // BaleenCore 文件的起始 LBA，按设备逻辑扇区单位；读盘按它递进
            uint32_t imageBytes = 0;     // BaleenCore 文件字节数，描述符与镜像头已核对一致
            uint32_t readBytes = 0;      // 按本地扇区上取整后的读入跨度，尾部填充也会写进内存
            uint32_t memoryBytes = 0;    // 头声明的静态内存跨度，含未落盘尾部；由 Stub 清零
            uint32_t entryOffset = 0;    // 头声明的早期入口偏移，交权入口 = loadAddress + 它
            uint32_t buildId = 0;        // 头里 BuildId 的前 4 字节，仅用于诊断显示
            uint32_t loadAddress = 0;    // 装入的物理地址，即镜像基址
            uint32_t bounceAddress = 0;  // 高位拷贝用的低地址弹跳窗口
        };

        // 读盘自检结果：0 号扇区是否读成功，以及 512 字节介质上的引导签名是否成立
        struct DiskProbeFacts {
            uint32_t readOk = 0;        // 读盘服务的返回值，1 为成功
            bool signatureOk = false;   // 读成功且 0x55AA 引导签名成立；非 512 字节介质不参与判定
        };

        // 读 0 号扇区并核对引导签名：读数正确说明弹跳、DAP 与分批口径都按预期工作
        // 缓冲与描述符、镜像头的读取共用同一份静态扇区缓冲，不在调用方的栈上另开
        DiskProbeFacts ProbeBootSector(uint32_t sectorBytes);

        // 取内存图到静态缓冲，返回条数；缓冲装不下固件还有的条目时把 1 写进 truncated
        // 内存图同时供装载区检查与交权块使用，因此落在静态缓冲而不是调用方的栈上
        uint32_t QueryMemoryMap(uint32_t& truncated);

        // 读取并校验 CoreDescriptor 与 BaleenCore 镜像头、校验装载区、挑弹跳窗口，并填好交权块
        // descriptorLba 是 IPL 交来的描述符扇区号，CoreDescriptor 与它同扇区
        // bootTsc 与 ticksPerMs 是引导计时事实：进入 Stub 时的时间戳与每毫秒的 TSC 计数，
        // 写进交权块供 BaleenCore 报出从 Stub 进入起的总耗时；无计时时传 0
        // 成功返回空 code 并把结果写进 plan，失败返回带编号的原因：打印与停机由调用方决定
        Diagnostic PrepareCore(uint32_t drive, uint32_t media, uint32_t sectorBytes, uint32_t descriptorLba, uint64_t bootTsc, uint32_t ticksPerMs, CoreLoadPlan& plan);

        // 按 plan 把 BaleenCore 读进高位，并清零头声明的未落盘尾部
        // 成功返回空指针，读盘失败返回带编号的原因
        Diagnostic ReadCore(const CoreLoadPlan& plan, uint32_t sectorBytes);

        // 比对整幅 BaleenCore 镜像的摘要：内容与头里的 Digest 不符时返回带编号的原因
        Diagnostic CheckCore(const CoreLoadPlan& plan);

        // 交权：把 ESI 置为交权块、按头声明的入口跳到 BaleenCore，不返回
        [[noreturn]] void EnterCore(const CoreLoadPlan& plan);
    }
}
