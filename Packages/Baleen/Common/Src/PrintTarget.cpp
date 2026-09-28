/* PrintTarget.cpp
    Baleen BIOS 路径引导期控制台的实现：三路目标组合与安装
*/

#include "PrintTarget.hpp"

namespace Baleen {
    namespace PrintTargets {
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
