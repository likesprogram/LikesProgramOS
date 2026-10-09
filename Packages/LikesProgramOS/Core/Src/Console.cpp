/* Console.cpp
    内核早期控制台：按 BootInfo 声明的可用通道装配串口与调试口目标，安装为当前 Print 目标

    目标实例由内核自己持有，就地构造在未初始化区的存储上：与 Baleen 各阶段不共享对象，
    也不使用引导期的 VGA 文本目标——内核早期只有流式通道，帧缓冲与自绘目标随显示阶段加入
    BootInfo 不可用（空指针、版本或长度不符）时不安装任何目标，写入被静默丢弃，
    这也是无可用通道的合法状态：不停机、不阻塞、不影响失败判定
*/

#include <new>

#include <BootInfo.hpp>
#include <Print.hpp>
#include <Print/DebugPortTarget.hpp>
#include <Print/SerialTarget.hpp>

namespace {
    // 未初始化区里的存储：不带初值，由安装入口就地构造
    alignas(Print::SerialTarget) uint8_t s_serialStorage[sizeof(Print::SerialTarget)];
    alignas(Print::DebugPortTarget) uint8_t s_debugStorage[sizeof(Print::DebugPortTarget)];
    alignas(Print::MultiplexTarget) uint8_t s_multiplexStorage[sizeof(Print::MultiplexTarget)];

    // 通道位是否置位
    bool HasChannel(uint32_t channels, Boot::Channel channel) {
        return (channels & static_cast<uint32_t>(channel)) != 0;
    }
}

// 装配内核自己的早期控制台：按 BootInfo 声明的通道组合目标后置为当前 Print 目标
// 可重复调用，重复调用会重新构造实例；BootInfo 不可用时不安装目标
extern "C" void _Kernel_Console_Install(const Boot::Info* info) {
    if (info == nullptr || info->version != Boot::InfoLayout::kVersion || info->bytes < Boot::InfoLayout::kBytes) {
        Print::SetTarget(nullptr);
        return;
    }
    Print::MultiplexTarget& multiplex = *new (s_multiplexStorage) Print::MultiplexTarget();
    if (HasChannel(info->channels, Boot::Channel::Serial)) {
        Print::SerialTarget& serial = *new (s_serialStorage) Print::SerialTarget();
        serial.Initialize();
        multiplex.Add(&serial);
    }
    if (HasChannel(info->channels, Boot::Channel::DebugPort)) {
        Print::DebugPortTarget& debug = *new (s_debugStorage) Print::DebugPortTarget();
        debug.Initialize();
        multiplex.Add(&debug);
    }
    Print::SetTarget(multiplex.Count() != 0 ? &multiplex : nullptr);
}
