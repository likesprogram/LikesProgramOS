/* DebugPortTarget.hpp
    调试口输出目标：写模拟器 0xE9 端口，真实硬件上无对应设备
*/

#pragma once
#include "Print.hpp"

namespace Print {
    // 0xE9 调试口目标：Bochs 与 QEMU 捕获写到该端口的字节，真实硬件上写入被忽略
    class DebugPortTarget : public OutTarget {
    public:
        static constexpr uint16_t kDefaultPort = 0xE9;   // 默认端口；端口是否存在无法从软件探测，启用即视为可用

        // 启用目标；可重复调用
        void Initialize(uint16_t port = kDefaultPort);
        // 启用后可用
        bool Ready() const override { return m_ready; }
        // 逐字节写入；不做换行补全
        void Write(const char* text) override;
    private:
        uint16_t m_port = kDefaultPort;   // 调试口端口
        bool m_ready = false;             // 是否已启用

        // 写 8 位 I/O 端口
        static void Out8(uint16_t port, uint8_t value) { asm volatile("OUTB %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
    };
}
