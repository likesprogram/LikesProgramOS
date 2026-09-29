/* PrintTarget.cpp
    Baleen BIOS 路径引导期控制台的实现：三路目标组合与安装，诊断行标签前缀的写入
*/

#include <PrintTarget.hpp>

namespace Baleen {
    namespace PrintTargets {
        void WriteTag(Tag tag) {
            Print::Write("[ ");
            // 设置标签颜色
            Print::SetAttribute(TagAttribute(tag));
            Print::Write(TagName(tag));
            // 恢复默认颜色
            Print::SetAttribute(Devices::Vga::kDefaultAttribute);
            Print::Write(" ] ");
        }

        void WriteLine(Tag tag, const char* text) {
            WriteTag(tag);
            if (text != nullptr) Print::Write(text);
            Print::Write("\r\n");
        }

        void BiosConsole::Install(Devices::Vga& vga) {
            vga.Initialize();
            m_text.Initialize(vga);
            m_debug.Initialize();
            m_serial.Initialize();
            m_multiplex.Clear();
            m_multiplex.Add(&m_text);
            m_multiplex.Add(&m_debug);
            m_multiplex.Add(&m_serial);
            Print::SetTarget(&m_multiplex);
        }
    }
}
