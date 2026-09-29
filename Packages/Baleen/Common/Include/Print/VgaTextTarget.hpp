/* VgaTextTarget.hpp
    VGA 文本设备与对应 Print 目标的声明：80×25 文本直写，供 BIOS 路径引导期装配
*/

#pragma once
#include <stdint.h>

#include <Print.hpp>

namespace Baleen {
    namespace Devices {
        // VGA 彩色文本模式设备
        // 调用方负责在装配前确认当前显示设备仍处于 80×25 文本模式，本类不探测或切换显示模式
        class Vga {
        public:
            static constexpr uint32_t kColumns = 80;             // 文本列数
            static constexpr uint32_t kRows = 25;                // 文本行数
            static constexpr uint8_t kDefaultAttribute = 0x0F;   // 默认属性字节：白字黑底

            // 常量初始化不依赖启动运行库；Stub/Core 都可以直接定义静态实例
            constexpr Vga() : m_text(nullptr), m_row(0), m_column(0), m_attribute(kDefaultAttribute), m_hardwareCursor(false) {}

            // 绑定物理 VGA 文本页，并接续硬件当前光标；不会清屏
            void Initialize();
            // 绑定指定文本页和光标位置，不访问 I/O 端口；用于 Core 的内存映射初始化和宿主侧的行为验证
            // 指定地址必须指向至少 kColumns*kRows 个单元
            void Initialize(volatile uint16_t* textMemory, uint32_t position);
            // 是否已绑定文本页
            bool IsInitialized() const { return m_text != nullptr; }
            // 清空整个文本页并把光标归零
            void Clear();
            // 设置后续写入使用的属性字节，不改变已有字符
            void SetAttribute(uint8_t attribute) { m_attribute = attribute; }
            // 当前属性字节
            uint8_t Attribute() const { return m_attribute; }
            // 把光标移到指定位置，越界返回 false
            bool SetCursor(uint32_t column, uint32_t row);
            // 当前线性光标位置
            uint32_t CursorPosition() const { return m_row * kColumns + m_column; }
            // 写入单个字符
            void PutChar(char character);
            // 写入以 '\0' 结尾的文本
            void Write(const char* text);

        private:
            static constexpr uintptr_t kTextMemoryAddress = 0xB8000;   // 文本页物理基址
            static constexpr uint16_t kCrtIndex = 0x3D4;               // CRT 控制器索引端口
            static constexpr uint16_t kCrtData = 0x3D5;                // CRT 控制器数据端口

            volatile uint16_t* m_text;   // 文本页基址；空表示未绑定
            uint32_t m_row;              // 当前行
            uint32_t m_column;           // 当前列
            uint8_t m_attribute;         // 当前属性字节
            bool m_hardwareCursor;       // 是否同步 CRT 硬件光标

            // 写 8 位 I/O 端口
            static void Out8(uint16_t port, uint8_t value);
            // 读 8 位 I/O 端口
            static uint8_t In8(uint16_t port);
            // 读取 CRT 光标位置；越界时归零
            static uint32_t ReadCursor();
            // 把线性位置换算成行列，越界归零
            void SetPosition(uint32_t position);
            // 组成带属性字节的文本单元
            uint16_t Cell(char character) const;
            // 把光标位置写回 CRT 控制器
            void SyncCursor() const;
            // 上滚一行，末行填空
            void ScrollUp();
            // 写入一个字符并处理回车、换行与自动换行
            void WriteChar(char character);
        };
    }
}

namespace Print {
    // VGA 文本目标：转发给 Baleen::Devices::Vga 实例，光标与文本模式沿用固件状态，仅引导期装配
    class VgaTextTarget : public OutTarget {
    public:
        // 绑定已初始化的 VGA 设备；设备须在目标使用期内保持有效
        void Initialize(Baleen::Devices::Vga& vga) { m_vga = &vga; }
        // 已绑定且设备已初始化时可用
        bool Ready() const override { return m_vga != nullptr && m_vga->IsInitialized(); }
        // 写入文本；未绑定或文本为空时丢弃
        void Write(const char* text) override { if (m_vga != nullptr && text != nullptr) m_vga->Write(text); }
        // 设置后续写入的属性字节；设备未绑定时丢弃
        void SetAttribute(uint8_t attribute) override { if (m_vga != nullptr) m_vga->SetAttribute(attribute); }
        // 清空文本页并把光标归零；设备未绑定时丢弃
        void ClearScreen() override { if (m_vga != nullptr) m_vga->Clear(); }
    private:
        Baleen::Devices::Vga* m_vga = nullptr;   // 被绑定的 VGA 设备；非拥有
    };
}
