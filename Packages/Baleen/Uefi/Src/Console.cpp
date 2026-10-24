/* Console.cpp
    UEFI 侧控制台装配与标签行写法

    第一阶段（固件启动服务仍有效）：固件控制台 + 0xE9 调试口；固件自己把控制台镜像到
    QEMU 给的串口上，因此这时不直接写 COM1，否则每行会重复一遍
    第二阶段（已退出启动服务）：COM1 + 0xE9；固件控制台不再合法，串口由本阶段直接驱动，
    内核随后按 BootInfo 的通道位继续写它，日志因此是连续的一份
    固件控制台的显示属性经简单文本输出协议的 SetAttribute 设置，属性编码与 VGA 一致：
    标签按等级着色，写完恢复缺省属性；直接驱动的串口与调试口忽略属性、只收纯文本
*/

#include <Console.hpp>

#include <Print.hpp>
#include <PrintTarget.hpp>

namespace Baleen {
    namespace PrintTargets {
        void WriteTag(Tag tag) {
            Print::Write("[ ");
            Print::SetAttribute(TagAttribute(tag));
            Print::Write(TagName(tag));
            Print::SetAttribute(Devices::Vga::kDefaultAttribute);
            Print::Write(" ] ");
        }

        void WriteLine(Tag tag, const char* text) {
            WriteTag(tag);
            if (text != nullptr) Print::Write(text);
            Print::Write("\r\n");
        }
    }

    namespace Uefi {
        bool Console::Install(SystemTable* systemTable) {
            if (systemTable == nullptr) {
                Print::SetTarget(nullptr);
                return false;
            }
            // 首选标准控制台输出；没有时退到标准错误，两者都是简单文本输出协议
            m_text.Bind(systemTable->consoleOut);
            if (!m_text.Ready()) m_text.Bind(systemTable->standardError);
            if (!m_text.Ready()) {
                Print::SetTarget(nullptr);
                return false;
            }
            // 串口现在就初始化（探测要趁固件服务还在），但要到退出启动服务之后才挂进扇出
            m_serial.Initialize();
            m_debug.Initialize();
            m_channels.Clear();
            m_channels.Add(&m_text);
            if (m_debug.Ready()) m_channels.Add(&m_debug);
            Print::SetTarget(&m_channels);
            return true;
        }

        void Console::ExitFirmwareConsole() {
            m_channels.Clear();
            if (m_serial.Ready()) m_channels.Add(&m_serial);
            if (m_debug.Ready()) m_channels.Add(&m_debug);
            Print::SetTarget(m_channels.Count() != 0 ? &m_channels : nullptr);
        }

        bool Console::Ready() const {
            return m_channels.Ready();
        }
    }
}
