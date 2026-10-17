/* UefiClock.cpp
    Clock 的实现：TSC 计数、CPUID 频率探测与 8254 实测校准

    频率来源按可信度依次退让：CPUID 0x15 给出晶体时钟与 TSC/晶体比，能算准；
    0x16 给出基准频率 MHz，是处理器标称值，够用于“有没有超预算”这种量级的判定；
    两者都没有时用 8254 定时器通道 2 实测一段已知时长（纯软件模拟的固件就是这种情况）。
    实测值要过一道量级检查，落在 100 MHz..100 GHz 之外就当校准失败——宁可报 NoClock 也不报假数字
*/

#include <UefiClock.hpp>

namespace {
    // 读时间戳计数器：x86-64 上恒可用（固件与内核都依赖它）
    uint64_t ReadStamp() {
        uint32_t low = 0;
        uint32_t high = 0;
        __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
        return (static_cast<uint64_t>(high) << 32) | low;
    }

    // 读 CPUID：先看最大叶号，再按叶号取值
    void ReadCpuid(uint32_t leaf, uint32_t subLeaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subLeaf));
    }

    // 探测 TSC 频率（Hz）：0 表示 CPUID 给不出
    uint64_t DetectStampHz() {
        uint32_t eax = 0;
        uint32_t ebx = 0;
        uint32_t ecx = 0;
        uint32_t edx = 0;
        ReadCpuid(0, 0, eax, ebx, ecx, edx);
        const uint32_t maxLeaf = eax;
        // 叶子 0x15：晶体时钟（ECX，Hz）与 TSC/晶体比（EBX/EAX）
        if (maxLeaf >= 0x15) {
            ReadCpuid(0x15, 0, eax, ebx, ecx, edx);
            if (eax != 0 && ebx != 0 && ecx != 0) {
                const uint64_t crystalHz = ecx;
                return crystalHz * static_cast<uint64_t>(ebx) / static_cast<uint64_t>(eax);
            }
        }
        // 叶子 0x16：基准频率（EAX，MHz）
        if (maxLeaf >= 0x16) {
            ReadCpuid(0x16, 0, eax, ebx, ecx, edx);
            if (eax != 0) return static_cast<uint64_t>(eax) * 1000000ull;
        }
        return 0;
    }

    // —— 8254 定时器：通道 2 的一次性计时，用于实测 TSC 频率 ——
    // 输入时钟固定 1.193182 MHz，计数值 11932 约合 10 毫秒
    constexpr uint16_t kPitChannel = 0x42;    // 通道 2 的数据口
    constexpr uint16_t kPitCommand = 0x43;    // 命令口
    constexpr uint16_t kPitGate = 0x61;       // 端口 B：位 0 是通道 2 的门控，位 5 是通道 2 的输出
    constexpr uint16_t kPitTicks = 11932;     // 约 10 毫秒

    // 写 8 位 I/O 端口
    void Out8(uint16_t port, uint8_t value) { __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory"); }

    // 读 8 位 I/O 端口
    uint8_t In8(uint16_t port) {
        uint8_t value = 0;
        __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }

    // 用 8254 通道 2 实测 TSC 频率（Hz）：0 表示这次实测不可信
    uint64_t MeasureStampHz() {
        // 门控置 0 再置 1：让通道 2 从确定的起点开始计数
        const uint8_t gate = In8(kPitGate);
        Out8(kPitGate, static_cast<uint8_t>(gate & ~0x01));
        // 命令：通道 2、只写低字节再写高字节、方式 0（计到零时输出变高）
        Out8(kPitCommand, 0xB0);
        Out8(kPitChannel, static_cast<uint8_t>(kPitTicks & 0xFF));
        Out8(kPitChannel, static_cast<uint8_t>(kPitTicks >> 8));
        const uint64_t begin = ReadStamp();
        Out8(kPitGate, static_cast<uint8_t>(gate | 0x01));
        // 等输出位变高：方式 0 计到零时置高；轮询次数设上限，避免在没有定时器的机器上卡死
        uint32_t spins = 0;
        while ((In8(kPitGate) & 0x20) == 0) {
            if (++spins > 50000000u) return 0;
        }
        const uint64_t elapsed = ReadStamp() - begin;
        // 计数时长换算：kPitTicks / 1.193182 MHz ≈ 10000 微秒
        const uint64_t micros = static_cast<uint64_t>(kPitTicks) * 1000000ull / 1193182ull;
        if (micros == 0) return 0;
        return elapsed * 1000000ull / micros;
    }
}

namespace Baleen {
    namespace Uefi {
        void Clock::Install() {
            m_entryStamp = ReadStamp();
            const uint64_t stampHz = DetectStampHz();
            // 每毫秒的计数：换算成 32 位够用（4 GHz 也才 4e6）
            m_ticksPerMs = stampHz == 0 ? 0 : static_cast<uint32_t>(stampHz / 1000ull);
        }

        bool Clock::CalibrateWithPit() {
            if (m_ticksPerMs != 0) return true;
            const uint64_t stampHz = MeasureStampHz();
            // 量级检查：落在这个区间之外说明实测被干扰或定时器不可用，宁可报 NoClock
            const uint64_t minimumHz = 100ull * 1000ull * 1000ull;         // 100 MHz
            const uint64_t maximumHz = 100ull * 1000ull * 1000ull * 1000ull; // 100 GHz
            if (stampHz < minimumHz || stampHz > maximumHz) return false;
            m_ticksPerMs = static_cast<uint32_t>(stampHz / 1000ull);
            return true;
        }

        uint32_t Clock::MillisSinceEntry() const {
            if (m_ticksPerMs == 0) return 0;
            const uint64_t elapsed = ReadStamp() - m_entryStamp;
            const uint64_t millis = elapsed / m_ticksPerMs;
            return millis > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(millis);
        }
    }
}
