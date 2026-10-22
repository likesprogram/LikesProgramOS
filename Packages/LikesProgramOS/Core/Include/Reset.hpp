/* Reset.hpp
    FATAL 之后的等待与平台复位：有界等待、0xCF9 与 ACPI 复位路径、失败与回退编号

    等待先用 PIT 通道 2 的一-shot 标定 TSC，再按 TSC 计时；时基不可用时退到有界忙等并返回
    false——等待失败不是停机条件，调用方按 WARN 报出后继续走复位
    复位按 0xCF9（先 SYS_RST、再 RST_CPU|SYS_RST）为主、ACPI FADT 的 reset register 为辅；
    两条路径都没把机器复位时返回 false，由调用方报 FATAL 后停机
    等待与复位的编号落在 E-KRN-90 / 91（90–99 号段属停机、重启与恢复要求）：
    本文件先给出常量，编号在 Diagnostics.hpp 登记

    本文件只提供机制：不打印、不停机；FATAL 路径的接线（先等待、后复位、复位失败再停机）
    在 Kernel::Core.cpp 的失败路径上完成
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <Diagnostics.hpp>

// 等待 milliseconds 毫秒：PIT 标定 TSC 后按 TSC 计时，时基不可用时按固定迭代次数退避
// 返回 true 表示按时间源等待完成；false 表示只做了有界退避，等待时长只能保证上界
extern "C" bool _Kernel_WaitMs(uint32_t milliseconds);
// 返回 TSC 每毫秒计数：首次调用现场按 PIT 标定一次，时基不可用时返回 0
// Core.Time 槽 1 经它实现；标定参数与实现只有一份，在 Reset.cpp 的 CalibrateTsc
extern "C" uint64_t _Kernel_TicksPerMs();
// 请求平台复位：0xCF9 主、ACPI FADT reset register 辅；两条路径都试过后等一小段时间
// 正常情况下不返回（机器已复位）；返回 false 表示复位没有生效，由调用方报错后停机
extern "C" bool _Kernel_Reset();

namespace Kernel {
    namespace Reset {
        // FATAL 等待的有效毫秒数：0 表示按默认 10 秒，超过 10 分钟上限也按默认
        // 与 BootInfo 的语义一致：等待是失败可见性的一部分，不是安全边界，不做严格校验
        constexpr uint32_t FatalWaitMs(uint32_t configured) {
            if (configured == 0 || configured > Boot::InfoLayout::kFatalWaitMsMax) return Boot::InfoLayout::kFatalWaitMsDefault;
            return configured;
        }

        // 等待与复位的编号常量：单点定义在 Diagnostics.hpp（90–99 号段），这里只转发名字
        using Diagnostics::kWarnWaitFallback;
        using Diagnostics::kFailReset;
    }
}
