/* Print.cpp
    文本输出前端与多路复用目标的实现
*/

#include <Print.hpp>

namespace Print {
    namespace Detail {
        OutTarget* currentTarget = nullptr;
    }

    bool MultiplexTarget::Add(OutTarget* target) {
        if (target == nullptr || m_count >= kMaxTargets) return false;
        m_targets[m_count] = target;
        ++m_count;
        return true;
    }

    bool MultiplexTarget::Ready() const {
        for (uint32_t i = 0; i < m_count; ++i) if (m_targets[i]->Ready()) return true;
        return false;
    }

    void MultiplexTarget::Write(const char* text) {
        for (uint32_t i = 0; i < m_count; ++i) m_targets[i]->Write(text);
    }

    void MultiplexTarget::SetAttribute(uint8_t attribute) {
        for (uint32_t i = 0; i < m_count; ++i) m_targets[i]->SetAttribute(attribute);
    }

    void MultiplexTarget::ClearScreen() {
        for (uint32_t i = 0; i < m_count; ++i) m_targets[i]->ClearScreen();
    }

    void PutChar(char character) { const char text[2] = { character, '\0' }; Write(text); }

    void WriteHex(uint32_t value) {
        static const char kDigits[] = "0123456789ABCDEF";
        char text[11];   // "0x" 加 8 位数字加结尾
        text[0] = '0';
        text[1] = 'x';
        for (uint32_t i = 0; i < 8; ++i) text[2 + i] = kDigits[(value >> (28 - 4 * i)) & 0xF];
        text[10] = '\0';
        Write(text);
    }
}
