/* Console.cpp
    Core 引导期控制台：持有独立于 Stub 的控制台实例，向汇编入口暴露安装与写入包装
*/

#include "Print.hpp"
#include "Print/VgaTextTarget.hpp"
#include "PrintTarget.hpp"

namespace {
    // 引导期控制台用的 VGA 文本设备
    Baleen::Devices::Vga s_vga;
    // BIOS 路径引导期控制台：组合 VGA 文本、0xE9 与 COM1
    Baleen::PrintTargets::BiosConsole s_console;
}

// Core 正式入口确定后，由入口在确认文本模式后调用
extern "C" void _Core_Console_Initialize() {
    s_console.Install(s_vga);
}

// 供汇编入口或其他 C 调用点使用的写入包装
extern "C" void _Core_Console_Write(const char* text) {
    Print::Write(text);
}
