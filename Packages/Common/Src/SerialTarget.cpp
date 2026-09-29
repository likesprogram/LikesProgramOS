/* SerialTarget.cpp
    串口输出目标的实现：16550 初始化与轮询发送
*/

#include <Print/SerialTarget.hpp>

namespace Print {
    void SerialTarget::Initialize(uint16_t base, uint16_t divisor) {
        m_base = base;
        Out8(m_base + 1, 0x00);   // 关中断
        Out8(m_base + 3, 0x80);   // DLAB=1，访问分频锁存
        Out8(m_base + 0, static_cast<uint8_t>(divisor & 0xFF));
        Out8(m_base + 1, static_cast<uint8_t>(divisor >> 8));
        Out8(m_base + 3, 0x03);   // 8 位数据、无校验、1 停止位，DLAB=0
        Out8(m_base + 2, 0xC7);   // 使能并清空 FIFO，触发级别 14
        Out8(m_base + 4, 0x03);   // DTR 与 RTS 有效
        m_ready = true;
    }

    void SerialTarget::Write(const char* text) {
        if (!m_ready || text == nullptr) return;
        for (const char* p = text; *p != '\0'; ++p) {
            uint32_t spins = 0;   // 已轮询次数
            while ((In8(m_base + 5) & 0x20) == 0 && spins < kWaitLimit) ++spins;
            if (spins >= kWaitLimit) continue;
            Out8(m_base + 0, static_cast<uint8_t>(*p));
        }
    }
}
