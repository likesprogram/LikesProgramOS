/* SerialTarget.cpp
    串口输出目标的实现：存在性探测、16550 初始化与轮询发送
*/

#include <Print/SerialTarget.hpp>

#include <stdint.h>

#include <Platform/PhysicalMemory.hpp>

namespace Print {
    namespace {
        // 16550 寄存器偏移
        constexpr uint16_t kRegData = 0;         // 发送保持 / 接收缓冲
        constexpr uint16_t kRegInterrupt = 1;    // 中断使能 / 分频高字节
        constexpr uint16_t kRegFifo = 2;         // FIFO 控制
        constexpr uint16_t kRegLine = 3;         // 线路控制 / DLAB
        constexpr uint16_t kRegModem = 4;        // 调制解调控制：bit4 回环，bit3 OUT2，bit1 RTS，bit0 DTR
        constexpr uint16_t kRegStatus = 5;       // 线路状态：bit0 接收数据就绪，bit5 发送保持空

        // 回环自测的参数与等待上限：写入与读回同一个字节，等待接收就绪的轮询次数有界
        constexpr uint8_t kLoopbackByte = 0xAE;
        constexpr uint8_t kModemLoopback = 0x1E;   // OUT2 | OUT1 | RTS | DTR | 回环
        constexpr uint32_t kProbeWaitLimit = 1000;

        // BIOS 数据区的 COM 端口表：0040:0000 起 4 个 16 位基址，0 表示该槽不存在
        constexpr uintptr_t kComTableAddress = 0x400;
        constexpr uint32_t kComPorts = 4;
    }

    bool SerialTarget::ListedInBda(uint16_t base) {
        const volatile uint8_t* const table = Platform::PhysicalBytePointer(kComTableAddress);
        for (uint32_t i = 0; i < kComPorts; ++i) {
            const uint16_t value = static_cast<uint16_t>(table[i * 2] | (table[i * 2 + 1] << 8));
            if (value == base) return true;
        }
        return false;
    }

    bool SerialTarget::LoopbackProbe(uint16_t base) {
        const uint8_t savedModem = In8(base + kRegModem);
        Out8(base + kRegModem, kModemLoopback);
        Out8(base + kRegData, kLoopbackByte);
        bool received = false;
        for (uint32_t spins = 0; spins < kProbeWaitLimit; ++spins) {
            if ((In8(base + kRegStatus) & 0x01) != 0) { received = true; break; }
        }
        const bool matches = received && In8(base + kRegData) == kLoopbackByte;
        Out8(base + kRegModem, savedModem);
        return matches;
    }

    bool SerialTarget::Probe(uint16_t base) {
        if (ListedInBda(base)) return true;
        return LoopbackProbe(base);
    }

    void SerialTarget::Initialize(uint16_t base, uint16_t divisor) {
        m_base = base;
        m_ready = false;
        // 端口不存在就不写寄存器：初始化序列本身会改动设备状态
        if (!Probe(base)) return;
        Out8(m_base + kRegInterrupt, 0x00);   // 关中断
        Out8(m_base + kRegLine, 0x80);        // DLAB=1，访问分频锁存
        Out8(m_base + kRegData, static_cast<uint8_t>(divisor & 0xFF));
        Out8(m_base + kRegInterrupt, static_cast<uint8_t>(divisor >> 8));
        Out8(m_base + kRegLine, 0x03);        // 8 位数据、无校验、1 停止位，DLAB=0
        Out8(m_base + kRegFifo, 0xC7);        // 使能并清空 FIFO，触发级别 14
        Out8(m_base + kRegModem, 0x03);       // DTR 与 RTS 有效
        m_ready = true;
    }

    void SerialTarget::Write(const char* text) {
        if (!m_ready || text == nullptr) return;
        for (const char* p = text; *p != '\0'; ++p) {
            uint32_t spins = 0;   // 已轮询次数
            while ((In8(m_base + kRegStatus) & 0x20) == 0 && spins < kWaitLimit) ++spins;
            if (spins >= kWaitLimit) continue;
            Out8(m_base + kRegData, static_cast<uint8_t>(*p));
        }
    }
}
