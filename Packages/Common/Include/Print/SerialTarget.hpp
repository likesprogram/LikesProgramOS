/* SerialTarget.hpp
    串口输出目标：16550 兼容 UART，初始化后轮询发送，等待有上限
*/

#pragma once
#include "Print.hpp"

namespace Print {
    // 16550 兼容串口目标：直接访问 I/O 端口，不注册中断；发送等待超限时丢弃字符
    class SerialTarget : public OutTarget {
    public:
        static constexpr uint16_t kDefaultBase = 0x3F8;   // 默认端口：COM1
        static constexpr uint16_t kDefaultDivisor = 1;    // 默认分频值：1.8432 MHz 时钟下为 115200 baud
        static constexpr uint32_t kWaitLimit = 100000;    // 单字符等待发送保持寄存器为空的轮询次数上限

        // 按给定端口与分频值初始化 8N1、关中断、使能 FIFO；默认分频值按标准 PC 时钟，其他时钟由平台相关实现另行处理；可重复调用
        void Initialize(uint16_t base = kDefaultBase, uint16_t divisor = kDefaultDivisor);
        // 初始化后可用
        bool Ready() const override { return m_ready; }
        // 逐字符轮询发送；端口不存在时等待达到上限并丢弃，不阻塞
        void Write(const char* text) override;
    private:
        uint16_t m_base = kDefaultBase;   // 端口基址
        bool m_ready = false;             // 是否已初始化

        // 写 8 位 I/O 端口
        static void Out8(uint16_t port, uint8_t value) { asm volatile("OUTB %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
        // 读 8 位 I/O 端口
        static uint8_t In8(uint16_t port) { uint8_t value; asm volatile("INB %1, %0" : "=a"(value) : "Nd"(port) : "memory"); return value; }
    };
}
