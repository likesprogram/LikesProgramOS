/* SerialTarget.hpp
    串口输出目标：16550 兼容 UART，先做存在性探测，初始化后轮询发送，等待有上限

    探测先查 BDA 的 COM 端口表（BIOS 在 POST 已探过一遍），表里没有再做回环自测：
    端口不存在时写入被忽略、读回浮空值，据此拒绝装配，无串口的机器上不会每字符空转
*/

#pragma once
#include <Print.hpp>

namespace Print {
    // 16550 兼容串口目标：直接访问 I/O 端口，不注册中断；发送等待超限时丢弃字符
    class SerialTarget : public OutTarget {
    public:
        static constexpr uint16_t kDefaultBase = 0x3F8;   // 默认端口：COM1
        static constexpr uint16_t kDefaultDivisor = 1;    // 默认分频值：1.8432 MHz 时钟下为 115200 baud
        static constexpr uint32_t kWaitLimit = 100000;    // 单字符等待发送保持寄存器为空的轮询次数上限

        // 按给定端口与分频值初始化 8N1、关中断、使能 FIFO；默认分频值按标准 PC 时钟，其他时钟由平台相关实现另行处理；可重复调用
        // 端口不存在时保持不可用，Write 立即丢弃，不做等待
        void Initialize(uint16_t base = kDefaultBase, uint16_t divisor = kDefaultDivisor);
        // 初始化且探测通过后可用
        bool Ready() const override { return m_ready; }
        // 逐字符轮询发送；端口不存在或未就绪时直接丢弃，不阻塞
        void Write(const char* text) override;

        // 串口存在性探测：BDA 的 COM 端口表列出该基址，或回环自测通过，即认为存在
        // 静态方法便于宿主侧与测试单独使用；不改变任何实例状态
        static bool Probe(uint16_t base);
    private:
        uint16_t m_base = kDefaultBase;   // 端口基址
        bool m_ready = false;             // 是否已初始化且探测通过

        // 写 8 位 I/O 端口
        static void Out8(uint16_t port, uint8_t value) { asm volatile("OUTB %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
        // 读 8 位 I/O 端口
        static uint8_t In8(uint16_t port) { uint8_t value; asm volatile("INB %1, %0" : "=a"(value) : "Nd"(port) : "memory"); return value; }
        // 端口是否出现在 BDA 的 COM 端口表里
        static bool ListedInBda(uint16_t base);
        // 回环自测：MCR 置回环位，写入的字节应从接收端读回
        static bool LoopbackProbe(uint16_t base);
    };
}
