/* ScreenFacts.hpp
    UEFI 路径的屏幕事实采集：把 GOP 当前 mode 的帧缓冲事实产成交给内核的屏幕形态

    采集规则：像素格式 BlueGreenRedReserved8BitPerColor → Bgrx8888、
    RedGreenBlueReserved8BitPerColor → Rgbx8888，PixelBitMask / PixelBltOnly 与未知格式给 None；
    几何取像素宽高，步长取 PixelsPerScanLine × 4，可写字节数取 FrameBufferSize；
    帧缓冲整段必须落在低 4GiB 的恒等映射内（内核按恒等映射自绘），否则给 None
    屏幕是诊断增强不是启动前提：采集只读固件给出的 mode 事实，不打印、不停机、不切换模式

    事实先落在本阶段的局部结构里，接线时再逐字段拷进 BootInfo 的屏幕字段组：
    采集逻辑因此不依赖交接契约的字段是否已就位，也便于宿主侧用假的 mode 结构验证映射
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <EfiTypes.hpp>

// FATAL 后等待毫秒数：由构建配置给出（Makefile 以 -DFATAL_WAIT_MS=<毫秒> 传入），0 表示内核按默认 10 秒
// 缺省构建不改内核的默认行为；测试变体写小值加速失败注入，超过 10 分钟的上限由内核按默认处理
#ifndef FATAL_WAIT_MS
#define FATAL_WAIT_MS 0
#endif

namespace Baleen {
    namespace Uefi {
        // 屏幕事实：与 BIOS 路径同一套字段（类型 / 地址 / 宽 / 高 / 步长 / 字节数 / 像素格式）
        // 字段顺序与 BootInfo 的屏幕字段组一一对应，拷贝不需要换算
        struct ScreenFacts {
            Boot::ScreenKind kind;      // 屏幕事实类型；None 表示没有可用的屏幕事实，只走流式通道
            uint64_t address;           // 帧缓冲的物理地址（低 4GiB）
            uint32_t width;             // 宽度：像素数
            uint32_t height;            // 高度：像素数
            uint32_t stride;            // 每行字节数（PixelsPerScanLine × 4）
            uint32_t bytes;             // 可写字节数（FrameBufferSize）
            Boot::ScreenFormat format;  // 像素格式；只支持 Bgrx8888 / Rgbx8888
        };

        // 每像素字节数：只支持 32 位打包格式，行步长按它换算
        constexpr uint32_t kBytesPerPixel = 4;

        // 低 4GiB 恒等映射的上界：内核只按低 4GiB 建恒等映射，帧缓冲整段必须落在里面
        constexpr uint64_t kIdentityLimit = kLowMemoryLimit;

        // FATAL 后等待毫秒数：0 = 内核按默认 10 秒，取值来自构建配置 FATAL_WAIT_MS
        constexpr uint32_t kFatalWaitMs = FATAL_WAIT_MS;

        // 采集：按 GOP 协议的当前 mode 产出屏幕事实
        // mode 为空、mode->info 为空、像素格式不受支持、几何或步长或字节数为零、
        // 帧缓冲整段越出低 4GiB 时给 None；只读事实，不打印、不停机
        ScreenFacts CollectScreenFacts(const GraphicsOutputProtocolMode* mode);
    }
}
