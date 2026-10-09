/* TeletypeTarget.hpp
    电传输出目标：把文本交给平台提供的写入函数，BIOS 路径下是 INT 10h/AH=0Eh 的实模式弹跳
*/

#pragma once
#include <stdint.h>

#include <Print.hpp>

namespace Print {
    // 电传目标：显示设备不处于 80×25 文本模式时的回退屏幕通道
    // 逐字符走固件调用，比显存直写慢几个数量级，只在 VGA 文本不可用时装配
    class TeletypeTarget : public OutTarget {
    public:
        // 平台侧写入函数：接收以 '\0' 结尾的文本，成功返回 1；实现必须留在低地址且可重入调用
        using WriteFn = uint32_t (*)(const char* text);

        // 绑定写入函数；可重复调用
        void Initialize(WriteFn write) { m_write = write; }
        // 绑定后可用
        bool Ready() const override { return m_write != nullptr; }
        // 写入文本；未绑定或文本为空时丢弃
        void Write(const char* text) override { if (m_write != nullptr && text != nullptr) m_write(text); }
    private:
        WriteFn m_write = nullptr;   // 平台侧的电传写入函数；非拥有
    };
}
