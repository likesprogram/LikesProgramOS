/* EfiTextTarget.cpp
    EfiTextTarget 的实现：窄字符按字节扩展到 UTF-16 后交给固件控制台
*/

#include <Print/EfiTextTarget.hpp>

namespace Baleen {
    namespace Uefi {
        void EfiTextTarget::Bind(SimpleTextOutput* console) {
            m_console = console;
        }

        bool EfiTextTarget::Ready() const {
            return m_console != nullptr && m_console->outputString != nullptr;
        }

        void EfiTextTarget::Write(const char* text) {
            if (!Ready() || text == nullptr) return;
            // 留一个位置放结尾的 '\0'：固件的 OutputString 要求以空字符结尾
            constexpr uint32_t kMaxChars = kLineChars - 1;
            uint32_t used = 0;
            for (const char* at = text; *at != '\0'; ++at) {
                // 按无符号字节扩展：非 ASCII 字节原样进 UTF-16 码位，不猜测编码
                m_line[used++] = static_cast<char16_t>(static_cast<uint8_t>(*at));
                if (used == kMaxChars) {
                    m_line[used] = u'\0';
                    m_console->outputString(m_console, m_line);
                    used = 0;
                }
            }
            if (used != 0) {
                m_line[used] = u'\0';
                m_console->outputString(m_console, m_line);
            }
        }
    }
}
