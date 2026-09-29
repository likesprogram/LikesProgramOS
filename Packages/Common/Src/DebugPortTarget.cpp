/* DebugPortTarget.cpp
    调试口输出目标的实现：0xE9 端口字节写入
*/

#include <Print/DebugPortTarget.hpp>

namespace Print {
    void DebugPortTarget::Initialize(uint16_t port) {
        m_port = port;
        m_ready = true;
    }

    void DebugPortTarget::Write(const char* text) {
        if (!m_ready || text == nullptr) return;
        for (const char* p = text; *p != '\0'; ++p) Out8(m_port, static_cast<uint8_t>(*p));
    }
}
