/* Font.hpp
    内核内嵌的 8×16 点阵字模：只覆盖可打印 ASCII 0x20..0x7E，供自绘屏幕目标在线性帧缓冲上绘制

    字形取自 X11 misc-fixed 9×18 位图（font-misc-misc，公有领域：原字体声明 "Public domain font.
    Share and enjoy."），裁掉单元左右各一列空白与上下填充行后按 8×16 单元内嵌；
    每字形 16 字节、一行一字节，最高位是最左像素，只读，不依赖固件字形，不做 Unicode 映射
    内核的 VGA 文本模式通道仍写字符 + 属性（字形由文本硬件提供），本字模只服务帧缓冲绘制
*/

#pragma once
#include <stdint.h>

namespace Kernel {
    namespace Font {
        // 字形尺寸：8 像素宽、16 像素高
        constexpr uint32_t kGlyphWidth = 8;
        constexpr uint32_t kGlyphHeight = 16;
        // 覆盖范围：可打印 ASCII，含首尾
        constexpr uint32_t kFirstCharacter = 0x20;
        constexpr uint32_t kLastCharacter = 0x7E;
        // 字形数量：95 个
        constexpr uint32_t kGlyphCount = kLastCharacter - kFirstCharacter + 1;
        // 每个字形的字节数：一行一字节
        constexpr uint32_t kGlyphBytes = kGlyphHeight;
        // 点阵数据总字节数：95 × 16 = 1520
        constexpr uint32_t kBitmapBytes = kGlyphCount * kGlyphBytes;

        // 取一个字符的点阵：返回 16 字节，按行自顶向下、每行最高位是最左像素
        // 不在 0x20..0x7E 内的字节返回空指针，调用方按空白处理
        const uint8_t* Glyph(uint8_t character);

        // 点阵数据本身：只读，按字符顺序连续存放
        const uint8_t* Bitmap();
    }
}
