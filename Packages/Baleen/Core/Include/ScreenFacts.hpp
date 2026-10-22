/* ScreenFacts.hpp
    BIOS 路径的屏幕事实采集：读 BIOS 数据区的当前显示模式与列数，产出交给内核的屏幕形态

    采集规则：模式 2 / 3 的文本页在 0xB8000，模式 7（MDA）在 0xB0000，
    几何固定 80×25、每行 160 字节、可写 4000 字节、像素格式 0（文本）；
    模式不是文本模式或列数不是 80 时给 None
    屏幕是诊断增强不是启动前提：采集只读 BDA，不打印、不停机、不切换显示模式；
    退化的事实由控制台装配侧按 E-CORE-60 报过 WARN（见 Packages/Baleen/Core/Src/Console.cpp）

    事实先落在本阶段的局部结构里，接线时再逐字段拷进 BootInfo 的屏幕字段组：
    采集逻辑因此不依赖交接契约的字段是否已就位，也便于宿主侧单独验证模式到事实的映射
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>

// FATAL 后等待毫秒数：由构建配置给出（Makefile 以 -DFATAL_WAIT_MS=<毫秒> 传入），0 表示内核按默认 10 秒
// 缺省构建不改内核的默认行为；测试变体写小值加速失败注入，超过 10 分钟的上限由内核按默认处理
#ifndef FATAL_WAIT_MS
#define FATAL_WAIT_MS 0
#endif

namespace Baleen {
    namespace Core {
        // 屏幕事实：引导器采集的当前屏幕形态，接线时拷进 BootInfo 的屏幕字段组
        // 字段顺序与 BootInfo 的屏幕字段组一一对应，拷贝不需要换算
        struct ScreenFacts {
            Boot::ScreenKind kind;      // 屏幕事实类型；None 表示没有可用的屏幕事实，只走流式通道
            uint64_t address;           // 文本页 / 帧缓冲的物理地址（低 4GiB）
            uint32_t width;             // 宽度：文本模式为字符列数
            uint32_t height;            // 高度：文本模式为字符行数
            uint32_t stride;            // 每行字节数
            uint32_t bytes;             // 可写字节数
            Boot::ScreenFormat format;  // 像素格式；文本模式为 VgaText
        };

        // 文本模式的几何固定值：BaleenCore 只认 80×25 的 VGA / MDA 文本页
        constexpr uint32_t kScreenColumns = 80;    // 文本列数
        constexpr uint32_t kScreenRows = 25;       // 文本行数
        constexpr uint32_t kScreenStride = 160;    // 每行字节数（80 个字符单元 × 2 字节）
        constexpr uint32_t kScreenBytes = 4000;    // 文本页可写字节数（80 × 25 × 2）

        // FATAL 后等待毫秒数：0 = 内核按默认 10 秒，取值来自构建配置 FATAL_WAIT_MS
        constexpr uint32_t kFatalWaitMs = FATAL_WAIT_MS;

        // 纯映射：按 BDA 的当前显示模式与列数产出屏幕事实，不读内存、不打印
        // 模式 2 / 3 → 0xB8000，模式 7 → 0xB0000；列数不是 80 或模式不是这三种时给 None
        ScreenFacts DecodeVgaScreenFacts(uint8_t mode, uint8_t columns);

        // 采集：读 BDA 的 0040:0049（当前显示模式）与 0040:004A（当前文本列数），按纯映射产出事实
        // 只读 BDA，不切换显示模式、不打印、不停机
        ScreenFacts CollectScreenFacts();
    }
}
