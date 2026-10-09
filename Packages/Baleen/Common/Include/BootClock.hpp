/* BootClock.hpp
    引导期时钟：用时间戳计数器（TSC）计量，启动时以 PIT 通道 2 校准出每毫秒的计数

    校准用 PIT 的一次性定时窗口：写计数前后各取一次 TSC，OUT 由低变高即到点；
    两次采样之间的误差在百微秒量级，引导预算按秒计时，够用
    Install 一开始就取一次时间戳作为阶段起点，校准在它之后进行，起点因此不受校准耗时影响
    没有 TSC 时 Stamp 返回 0；校准失败时 Available() 为假、MillisBetween 返回 0，计时行不输出
    本文件只做计时机制：不打印、不停机
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    // 两个时间戳之间按给定每毫秒计数换算的毫秒数：计数为 0、倒序或商超过 32 位时返回 0
    // 供读交权块里 Stub 校准值的阶段使用：换算不依赖本地时钟实例
    uint32_t MillisBetween(uint64_t start, uint64_t end, uint32_t ticksPerMs);

    // 引导期时钟：Install 探测并校准一次，之后只读查询
    class BootClock {
    public:
        // 探测 TSC 并用 PIT 通道 2 校准；可重复调用，失败时留下不可用状态
        // 入口先取一次时间戳：阶段的起点从这里算，校准耗时不计入
        void Install();
        // 是否可用：TSC 存在且校准得到非零的每毫秒计数
        bool Available() const { return m_hasTsc && m_ticksPerMs != 0; }
        // Install 开始时取的时间戳；没有 TSC 时为 0
        uint64_t EntryStamp() const { return m_entryStamp; }
        // 读当前 TSC；没有 TSC 时为 0
        uint64_t Stamp() const;
        // 每毫秒的 TSC 计数；不可用时为 0
        uint32_t TicksPerMs() const { return m_ticksPerMs; }
        // 两个时间戳之间的毫秒数：不可用、倒序或商超过 32 位时返回 0
        uint32_t MillisBetween(uint64_t start, uint64_t end) const;

    private:
        bool m_hasTsc = false;            // 是否支持 TSC
        uint64_t m_entryStamp = 0;        // Install 开始时取的时间戳
        uint32_t m_ticksPerMs = 0;        // 每毫秒的 TSC 计数；0 表示不可用
    };
}
