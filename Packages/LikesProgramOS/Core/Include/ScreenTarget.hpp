/* ScreenTarget.hpp
    内核自绘屏幕目标：按 BootInfo 的屏幕事实把 Print 文本写到屏幕，屏幕上与流式通道是同一行文本

    VgaText：直写 BootInfo 给出的文本页（VGA 0xB8000 / MDA 0xB0000），按事实的宽度、高度、
    步长与可写字节数落界，每单元写字符 + 属性；Framebuffer：按地址、步长与像素格式
    （Bgrx8888 / Rgbx8888）用内核内嵌的 8×16 字模绘制，不依赖固件字形与固件服务
    屏幕事实不成立（类型 None、地址为零、几何 / 步长 / 字节数放不下一格文本、像素格式不支持）
    时目标保持不可用，Ready() 返回 false，装配方跳过它、只走流式通道——这不是失败，
    也不改变等待与复位行为
    目标由内核静态持有，不分配内存、不阻塞、写不出去即丢弃；字符只覆盖可打印 ASCII，
    帧缓冲下其他字节画成空白（文本页模式按原字节写入，字形由文本硬件提供）
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <Print.hpp>

namespace Kernel {
    // 内核自绘屏幕目标：文本页直写与线性帧缓冲自绘两种形态，接线方式与其它 Print 目标一致
    class ScreenTarget final : public Print::OutTarget {
    public:
        // 字形尺寸：与内核内嵌字模一致
        static constexpr uint32_t kGlyphWidth = 8;
        static constexpr uint32_t kGlyphHeight = 16;
        // 文本页每单元字节数：字符一字节加属性一字节
        static constexpr uint32_t kTextCellBytes = 2;
        // 缺省属性：白字黑底，与引导期的缺省属性一致
        static constexpr uint8_t kDefaultAttribute = 0x0F;

        // 按 BootInfo 屏幕字段组装配：事实不成立或格式不支持时保持不可用，不切换显示模式
        void Install(Boot::ScreenKind kind, uint64_t address, uint32_t width, uint32_t height, uint32_t stride, uint32_t bytes, Boot::ScreenFormat format);
        // 目标是否可用：屏幕事实成立且缓冲能放下一格文本时为真
        bool Ready() const override { return m_mode != Mode::None; }
        // 写入以 '\0' 结尾的文本：'\r' 回到行首、'\n' 换行，其余字符按字形或文本单元写入
        void Write(const char* text) override;
        // 设置后续写入的显示属性：文本页按属性字节，帧缓冲把属性映射成前景色与背景色
        void SetAttribute(uint8_t attribute) override;
        // 清空整个屏幕（用当前属性的背景）并把光标回到左上
        void ClearScreen() override;

        // 装配后的文本区尺寸：能放下的字符列数与行数，未装配或不可用时为 0
        uint32_t Columns() const { return m_columns; }
        uint32_t Rows() const { return m_rows; }

    private:
        // 显示模式
        enum class Mode : uint8_t {
            None,          // 没有可用的屏幕事实
            Text,          // 文本页直写：每单元字符 + 属性
            Framebuffer,   // 线性帧缓冲：按像素格式画 8×16 字模
        };

        // 换行：光标下移一行，越过末行时上滚
        void NewLine();
        // 写一个字符到文本页
        void WriteTextCell(uint8_t character);
        // 写一个 8×16 字形到帧缓冲
        void WriteGlyph(uint8_t character);
        // 文本页上滚一字符行
        void ScrollText();
        // 帧缓冲上滚一字符行：整屏按 16 个像素行上移，末行用背景填充
        void ScrollFramebuffer();
        // 帧缓冲里某一像素行的可写指针：row 以字符格为单位
        volatile uint8_t* PixelRow(uint32_t row) const { return m_base + static_cast<uint64_t>(row) * m_stride; }
        // 显示属性对应的一种颜色的像素值：color 为 0..15 的 VGA 调色板编号
        uint32_t PixelValue(uint8_t color) const;
        // 当前属性的背景像素与前景像素
        uint32_t BackgroundPixel() const { return PixelValue(static_cast<uint8_t>(m_attribute >> 4)); }
        uint32_t ForegroundPixel() const { return PixelValue(static_cast<uint8_t>(m_attribute & 0x0F)); }

        Mode m_mode = Mode::None;                                    // 显示模式
        volatile uint8_t* m_base = nullptr;                          // 屏幕基址：物理地址恒等映射
        uint32_t m_width = 0;                                        // 文本列数 / 像素宽
        uint32_t m_height = 0;                                       // 文本行数 / 像素高
        uint32_t m_stride = 0;                                       // 每行字节数
        uint32_t m_bytes = 0;                                        // 可写字节数
        Boot::ScreenFormat m_format = Boot::ScreenFormat::VgaText;   // 像素格式
        uint32_t m_columns = 0;                                      // 能放下的字符列数
        uint32_t m_rows = 0;                                         // 能放下的字符行数
        uint32_t m_column = 0;                                       // 光标列
        uint32_t m_row = 0;                                          // 光标行
        uint8_t m_attribute = kDefaultAttribute;                     // 当前显示属性
    };
}
