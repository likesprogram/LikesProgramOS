/* PrintTarget.hpp
    Baleen BIOS 路径引导期控制台：组合 VGA 文本、调试口与串口并安装为当前 Print 目标
*/

#pragma once
#include "Print.hpp"
#include "Print/DebugPortTarget.hpp"
#include "Print/SerialTarget.hpp"
#include "Print/VgaTextTarget.hpp"

namespace Baleen {
    namespace PrintTargets {
        // BIOS 路径引导期控制台：显存直写给人看，0xE9 与串口供自动化与无头机器抓取
        // 不走 BIOS 电传是有意的：CSM 下电传是逐字符固件调用，真机上慢到肉眼可见，
        // 而 IPL 交权后仍处于固件给出的文本模式，显存直写快几个数量级
        class BiosConsole {
        public:
            // 初始化绑定的 VGA 设备，组合三路目标并安装为当前 Print 目标；可重复调用
            void Install(Devices::Vga& vga);
        private:
            Print::VgaTextTarget m_text;          // VGA 文本目标
            Print::DebugPortTarget m_debug;       // 0xE9 调试口目标
            Print::SerialTarget m_serial;         // COM1 串口目标
            Print::MultiplexTarget m_multiplex;   // 扇出目标
        };
    }
}
