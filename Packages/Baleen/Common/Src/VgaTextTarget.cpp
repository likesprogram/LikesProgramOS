/* VgaTextTarget.cpp
    VGA 文本设备与对应 Print 目标的实现：文本页写入、光标同步与滚动
*/

#include "Print/VgaTextTarget.hpp"

namespace Baleen {
    namespace Devices {
        void Vga::Initialize() {
            m_text = reinterpret_cast<volatile uint16_t*>(kTextMemoryAddress);
            m_attribute = kDefaultAttribute;
            const uint32_t position = ReadCursor();   // 硬件当前光标位置
            SetPosition(position);
            m_hardwareCursor = true;
            SyncCursor();
        }

        void Vga::Initialize(volatile uint16_t* textMemory, uint32_t position) {
            m_text = textMemory;
            m_attribute = kDefaultAttribute;
            SetPosition(position);
            m_hardwareCursor = false;
        }

        void Vga::Clear() {
            if (!IsInitialized()) return;
            for (uint32_t i = 0; i < kColumns * kRows; ++i) m_text[i] = Cell(' ');
            m_row = 0;
            m_column = 0;
            SyncCursor();
        }

        bool Vga::SetCursor(uint32_t column, uint32_t row) {
            if (column >= kColumns || row >= kRows) return false;
            m_column = column;
            m_row = row;
            SyncCursor();
            return true;
        }

        void Vga::PutChar(char character) {
            if (!IsInitialized()) return;
            WriteChar(character);
            SyncCursor();
        }

        void Vga::Write(const char* text) {
            if (!IsInitialized() || text == nullptr) return;
            for (const char* p = text; *p != '\0'; ++p) WriteChar(*p);
            SyncCursor();
        }

        void Vga::Out8(uint16_t port, uint8_t value) {
            asm volatile("OUTB %0, %1" : : "a"(value), "Nd"(port) : "memory");
        }

        uint8_t Vga::In8(uint16_t port) {
            uint8_t value;
            asm volatile("INB %1, %0" : "=a"(value) : "Nd"(port) : "memory");
            return value;
        }

        uint32_t Vga::ReadCursor() {
            Out8(kCrtIndex, 0x0E);
            const uint32_t high = In8(kCrtData);   // 位置高字节
            Out8(kCrtIndex, 0x0F);
            const uint32_t low = In8(kCrtData);    // 位置低字节
            const uint32_t position = (high << 8) | low;
            return position < kColumns * kRows ? position : 0;
        }

        void Vga::SetPosition(uint32_t position) {
            if (position >= kColumns * kRows) position = 0;
            m_row = position / kColumns;
            m_column = position % kColumns;
        }

        uint16_t Vga::Cell(char character) const {
            return static_cast<uint16_t>((static_cast<uint16_t>(m_attribute) << 8) |
                                         static_cast<uint8_t>(character));
        }

        void Vga::SyncCursor() const {
            if (!m_hardwareCursor) return;
            uint32_t position = CursorPosition();
            // 写到右下角后保留待换行状态，但 CRT 光标不能指向屏幕后
            if (position >= kColumns * kRows) position = kColumns * kRows - 1;
            Out8(kCrtIndex, 0x0E);
            Out8(kCrtData, static_cast<uint8_t>(position >> 8));
            Out8(kCrtIndex, 0x0F);
            Out8(kCrtData, static_cast<uint8_t>(position));
        }

        void Vga::ScrollUp() {
            for (uint32_t i = kColumns; i < kColumns * kRows; ++i) m_text[i - kColumns] = m_text[i];
            for (uint32_t i = kColumns * (kRows - 1); i < kColumns * kRows; ++i) m_text[i] = Cell(' ');
            m_row = kRows - 1;
            m_column = 0;
        }

        void Vga::WriteChar(char character) {
            if (character == '\r') {
                m_column = 0;
                return;
            }
            if (character == '\n') {
                m_column = 0;
                ++m_row;
                if (m_row >= kRows) ScrollUp();
                return;
            }
            if (m_column >= kColumns) {
                m_column = 0;
                ++m_row;
                if (m_row >= kRows) ScrollUp();
            }
            m_text[m_row * kColumns + m_column] = Cell(character);
            ++m_column;
        }
    }
}
