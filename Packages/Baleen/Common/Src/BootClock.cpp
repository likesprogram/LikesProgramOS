/* BootClock.cpp
    BootClock.hpp 的实现：TSC 探测、PIT 校准与毫秒换算

    校准窗口取 10 ms：太短则 PIT 量化误差占比大，太长则拖慢引导本身
    换算不用运行库的 64 位除法：目标是带 -nostdlib 的引导镜像，商只有毫秒量级
*/

#include <BootClock.hpp>

namespace Baleen {
    namespace {
        // PIT 与扬声器口的寄存器
        constexpr uint16_t kPitChannel2 = 0x42;   // 通道 2 数据口
        constexpr uint16_t kPitCommand = 0x43;    // 模式字口
        constexpr uint16_t kPortSpeaker = 0x61;   // bit0 通道 2 门控，bit1 扬声器使能，bit5 通道 2 的 OUT
        constexpr uint8_t kPitMode0 = 0xB0;       // 通道 2、模式 0（一次性定时）、二进制、先低后高
        constexpr uint32_t kPitHz = 1193182;      // PIT 输入时钟，标准 PC
        constexpr uint32_t kCalibrationMs = 10;   // 校准窗口
        constexpr uint32_t kCalibrationCount = kPitHz / 1000 * kCalibrationMs;
        constexpr uint32_t kCalibrationWait = 1000000;   // 等待 OUT 变高的轮询上限
        constexpr uint32_t kTscFeature = 1u << 4;        // CPUID.01H:EDX bit 4：时间戳计数器

        // 写 8 位 I/O 端口
        inline void Out8(uint16_t port, uint8_t value) { asm volatile("OUTB %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
        // 读 8 位 I/O 端口
        inline uint8_t In8(uint16_t port) { uint8_t value; asm volatile("INB %1, %0" : "=a"(value) : "Nd"(port) : "memory"); return value; }

        // 读时间戳计数器
        inline uint64_t ReadTsc() {
            uint32_t low = 0;
            uint32_t high = 0;
            asm volatile("rdtsc" : "=a"(low), "=d"(high));
            return (static_cast<uint64_t>(high) << 32) | low;
        }

        // 是否支持 TSC：CPUID 基本叶 1 的 EDX bit4
        bool HasTsc() {
            uint32_t eax = 0;
            uint32_t ebx = 0;
            uint32_t ecx = 0;
            uint32_t edx = 0;
            asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
            return (edx & kTscFeature) != 0;
        }

        // 64 位除以 32 位的无符号除法：不带运行库的 64 位除法，用移位长除法逐位求商
        // 商必须不超过 32 位，调用场景是 TSC 差值与每毫秒计数，成立；除数为 0 时返回 0
        uint32_t Divide64By32(uint64_t value, uint32_t divisor) {
            if (divisor == 0) return 0;
            uint32_t quotient = 0;
            uint64_t remainder = value;
            for (uint32_t bit = 31; ; --bit) {
                const uint64_t scaled = static_cast<uint64_t>(divisor) << bit;
                if (remainder >= scaled) { remainder -= scaled; quotient |= (1u << bit); }
                if (bit == 0) break;
            }
            return quotient;
        }

        // PIT 通道 2 的一次性定时：返回两次 TSC 采样之间的计数，失败返回 0
        // 顺序：开门控并关扬声器 → 写模式与计数 → 等 OUT 变高 → 恢复门控状态
        uint64_t CalibrateWithPit() {
            const uint8_t saved = In8(kPortSpeaker);
            Out8(kPortSpeaker, static_cast<uint8_t>((saved & ~0x02) | 0x01));
            Out8(kPitCommand, kPitMode0);
            Out8(kPitChannel2, static_cast<uint8_t>(kCalibrationCount & 0xFF));
            const uint64_t start = ReadTsc();
            Out8(kPitChannel2, static_cast<uint8_t>(kCalibrationCount >> 8));
            bool done = false;
            for (uint32_t spins = 0; spins < kCalibrationWait; ++spins) {
                if ((In8(kPortSpeaker) & 0x20) != 0) { done = true; break; }
            }
            const uint64_t end = ReadTsc();
            Out8(kPortSpeaker, saved);
            return done && end > start ? end - start : 0;
        }
    }

    uint32_t MillisBetween(uint64_t start, uint64_t end, uint32_t ticksPerMs) {
        if (ticksPerMs == 0 || end < start) return 0;
        return Divide64By32(end - start, ticksPerMs);
    }

    void BootClock::Install() {
        m_ticksPerMs = 0;
        m_hasTsc = HasTsc();
        m_entryStamp = m_hasTsc ? ReadTsc() : 0;
        if (!m_hasTsc) return;
        const uint64_t delta = CalibrateWithPit();
        if (delta == 0) return;
        m_ticksPerMs = Divide64By32(delta, kCalibrationMs);
    }

    uint64_t BootClock::Stamp() const {
        return m_hasTsc ? ReadTsc() : 0;
    }

    uint32_t BootClock::MillisBetween(uint64_t start, uint64_t end) const {
        return Baleen::MillisBetween(start, end, m_ticksPerMs);
    }
}
