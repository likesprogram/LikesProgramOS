/* UefiClock.hpp
    UEFI 阶段的引导计时：量从固件交来到交接给内核之间的耗时，并按预算给出判定

    BIOS 侧的时间由 Stub 用固件时钟校准后随交权块传下来；UEFI 侧没有那一份校准，只能自己找频率：
    先问 CPUID 0x15（核心晶体时钟与 TSC/晶体比），再退到 0x16（基准频率 MHz）；
    两者都给不出时（纯软件模拟的固件常见）用 8254 定时器实测一段已知时长来校准。
    全都拿不到时不报毫秒数（Verdict=NoClock）——不猜频率，也不用固件的时间服务：
    退出启动服务之后它本就不可用

    预算与 BIOS 侧同一个，判定只在能换算成毫秒时才给
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    namespace Uefi {
        // 引导时间预算：从本阶段入口（固件把控制权交给本映像）到内核入口之前的毫秒数
        // 取值与 BIOS 侧一致（见 Packages/Baleen/Core/Src/Core.cpp 的 kBootBudgetMs）
        constexpr uint32_t kBootBudgetMs = 3000;

        // 引导计时：入口取时间戳，能校准时给出毫秒数
        class Clock {
        public:
            // 取入口时间戳并尝试用 CPUID 校准；可重复调用
            void Install();
            // 用 8254 定时器校准；只在还没校准成功时动作，返回是否校准成立
            // 退出启动服务之后调用：那时 8254 归本阶段使用，固件不再碰它
            bool CalibrateWithPit();
            // 校准是否成立；不成立时 MillisSinceEntry 返回 0
            bool IsCalibrated() const { return m_ticksPerMs != 0; }
            // 从入口到现在的毫秒数；未校准时返回 0
            uint32_t MillisSinceEntry() const;

        private:
            uint64_t m_entryStamp = 0;    // 入口时间戳
            uint32_t m_ticksPerMs = 0;    // 每毫秒的 TSC 计数；0 表示没有校准源
        };
    }
}
