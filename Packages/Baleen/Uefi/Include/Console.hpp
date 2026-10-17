/* Console.hpp
    UEFI 侧控制台装配：分两个阶段接当前 Print 目标

    退出启动服务之前用固件控制台（给人看，并随固件镜像到串口）+ 0xE9 调试口；
    退出之后固件控制台不再合法，换成本阶段直接驱动的 COM1 串口 + 0xE9 调试口
    两个阶段都不重复写同一个物理通道：固件控制台已经镜像到串口时不再直接写串口，
    串口只在退出固件服务之后接管，内核随后按 BootInfo 的通道位继续写它
*/

#pragma once
#include <EfiTypes.hpp>
#include <Print.hpp>
#include <Print/DebugPortTarget.hpp>
#include <Print/EfiTextTarget.hpp>
#include <Print/SerialTarget.hpp>

namespace Baleen {
    namespace Uefi {
        // UEFI 侧控制台：持有三个阶段的目标，由 ExitFirmwareConsole 切换
        class Console {
        public:
            // 绑定系统表里的控制台输出并初始化串口与调试口，安装第一阶段目标
            // 固件控制台不可用时返回 false，并把当前目标置空
            bool Install(SystemTable* systemTable);
            // 退出启动服务之后调用：把当前目标换成串口与调试口，不再经过固件
            void ExitFirmwareConsole();
            // 当前是否有可用通道
            bool Ready() const;

        private:
            EfiTextTarget m_text;              // 固件控制台：简单文本输出协议，退出启动服务后失效
            Print::SerialTarget m_serial;      // COM1 串口：退出固件服务之后的可见通道
            Print::DebugPortTarget m_debug;    // 0xE9 调试口：QEMU / Bochs 的 debugcon
            Print::MultiplexTarget m_channels; // 当前阶段的扇出
        };
    }
}
