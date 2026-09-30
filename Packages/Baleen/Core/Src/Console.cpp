/* Console.cpp
    Core 引导期控制台：持有独立于 Stub 的 VGA 文本、0xE9 与 COM1 目标，
    在未初始化区的存储上就地构造并安装为当前 Print 目标

    带虚表的对象不能作为普通全局量：引导镜像没有运行库，不会执行构造调用，
    Vga 与组合控制台就会缺虚表指针；这里只留存储，由安装入口就地构造
    控制台是 Core 自己的实例，不经过交权块的写服务：Stub 冻结后输出仍然可用
*/

#include <new>

#include <Print.hpp>
#include <Print/VgaTextTarget.hpp>
#include <PrintTarget.hpp>

namespace {
    // 未初始化区里的存储：不带初值，由安装入口就地构造；Stub 交权前已按 MemoryBytes 清零
    alignas(Baleen::Devices::Vga) uint8_t s_vgaStorage[sizeof(Baleen::Devices::Vga)];
    alignas(Baleen::PrintTargets::BiosConsole) uint8_t s_consoleStorage[sizeof(Baleen::PrintTargets::BiosConsole)];
}

// 装配并安装 Core 自己的控制台：组合 VGA 文本、0xE9 与 COM1 后置为当前 Print 目标
// 可重复调用，重复调用会重新构造实例；与 Stub 的同类装配互不影响
extern "C" void _Core_Console_Install() {
    Baleen::Devices::Vga& vga = *new (s_vgaStorage) Baleen::Devices::Vga();
    Baleen::PrintTargets::BiosConsole& console = *new (s_consoleStorage) Baleen::PrintTargets::BiosConsole();
    console.Install(vga);
}
