/* ScreenTarget.cpp
    内核自绘屏幕目标的实现：文本页直写、线性帧缓冲自绘、光标推进、上滚与调色板映射

    两种形态共用同一套光标与换行规则；落界一律以装配时核过的几何与可写字节数为准，
    写入不越出末行末列，坏事实在装配阶段就被拒掉
    物理地址按低 4GiB 恒等映射直接访问：引导器与内核自己的引导页表都保留低 4GiB 恒等映射，
    因此装配（换页表之前）与 FATAL 路径（换页表之后）用同一个指针写法
*/

#include <ScreenTarget.hpp>

#include <Font.hpp>

namespace Kernel {
    namespace {
        // VGA 缺省调色板：16 种颜色的 0xRRGGBB 取值
        constexpr uint32_t kPalette[16] = {
            0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
            0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
        };
        // 文本区几何上限：挡住坏事实造成的越界绘制
        constexpr uint32_t kMaxTextColumns = 132;
        constexpr uint32_t kMaxTextRows = 64;
        // 帧缓冲几何上限：常见固件分辨率远小于它
        constexpr uint32_t kMaxPixelWidth = 8192;
        constexpr uint32_t kMaxPixelHeight = 8192;
        // 每像素字节数：只支持 32 位打包格式
        constexpr uint32_t kBytesPerPixel = 4;
        // 屏幕物理地址上限：内核只按低 4GiB 建恒等映射
        constexpr uint64_t kAddressLimit = 0x100000000ull;
    }

    void ScreenTarget::Install(Boot::ScreenKind kind, uint64_t address, uint32_t width, uint32_t height, uint32_t stride, uint32_t bytes, Boot::ScreenFormat format) {
        // 先回到不可用：任何一项事实不成立都停在 None，不停机、不改显示模式
        m_mode = Mode::None;
        m_base = nullptr;
        m_width = 0;
        m_height = 0;
        m_stride = 0;
        m_bytes = 0;
        m_format = format;
        m_columns = 0;
        m_rows = 0;
        m_column = 0;
        m_row = 0;
        if (address == 0 || address >= kAddressLimit || bytes == 0) return;
        if (kind == Boot::ScreenKind::VgaText) {
            if (format != Boot::ScreenFormat::VgaText) return;
            if (width == 0 || width > kMaxTextColumns || height == 0 || height > kMaxTextRows) return;
            if (stride < width * kTextCellBytes) return;
            const uint64_t lastRowEnd = static_cast<uint64_t>(stride) * (height - 1) + static_cast<uint64_t>(width) * kTextCellBytes;
            if (bytes < lastRowEnd) return;
            m_mode = Mode::Text;
            m_columns = width;
            m_rows = height;
        } else if (kind == Boot::ScreenKind::Framebuffer) {
            if (format != Boot::ScreenFormat::Bgrx8888 && format != Boot::ScreenFormat::Rgbx8888) return;
            // 像素按 32 位写：基址与行步长都要 4 字节对齐
            if ((address & 3) != 0 || (stride & 3) != 0) return;
            if (width < kGlyphWidth || width > kMaxPixelWidth || height < kGlyphHeight || height > kMaxPixelHeight) return;
            if (stride < width * kBytesPerPixel) return;
            const uint64_t lastRowEnd = static_cast<uint64_t>(stride) * (height - 1) + static_cast<uint64_t>(width) * kBytesPerPixel;
            if (bytes < lastRowEnd) return;
            m_mode = Mode::Framebuffer;
            m_columns = width / kGlyphWidth;
            m_rows = height / kGlyphHeight;
            if (m_columns == 0 || m_rows == 0) {
                m_mode = Mode::None;
                return;
            }
        }
        // 事实成立：绑定基址与几何；文本页的 stride 与 width 可能由调用方按实际单元长度给出
        m_base = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(address));
        m_width = width;
        m_height = height;
        m_stride = stride;
        m_bytes = bytes;
        // 接管屏幕：先按缺省属性清屏，避免引导期残留字符与本阶段的短行拼在一起
        // 只写显存，不切换显示模式、不重编程文本状态
        ClearScreen();
    }

    void ScreenTarget::Write(const char* text) {
        if (m_mode == Mode::None || text == nullptr) return;
        for (const char* cursor = text; *cursor != '\0'; ++cursor) {
            const uint8_t character = static_cast<uint8_t>(*cursor);
            if (character == '\r') {
                m_column = 0;
                continue;
            }
            if (character == '\n') {
                NewLine();
                continue;
            }
            if (m_column >= m_columns) {
                m_column = 0;
                NewLine();
            }
            if (m_mode == Mode::Text) WriteTextCell(character);
            else WriteGlyph(character);
            ++m_column;
        }
    }

    void ScreenTarget::SetAttribute(uint8_t attribute) {
        // 属性只影响后续写入：文本页按原字节写，帧缓冲把高 4 位当前景、低 4 位当背景
        m_attribute = attribute;
    }

    void ScreenTarget::ClearScreen() {
        m_column = 0;
        m_row = 0;
        if (m_mode == Mode::Text) {
            for (uint32_t row = 0; row < m_rows; ++row) {
                volatile uint8_t* const cellRow = m_base + static_cast<uint64_t>(row) * m_stride;
                for (uint32_t column = 0; column < m_width; ++column) {
                    cellRow[column * kTextCellBytes] = static_cast<uint8_t>(' ');
                    cellRow[column * kTextCellBytes + 1] = m_attribute;
                }
            }
            return;
        }
        if (m_mode == Mode::Framebuffer) {
            const uint32_t background = BackgroundPixel();
            for (uint32_t row = 0; row < m_height; ++row) {
                volatile uint32_t* const line = reinterpret_cast<volatile uint32_t*>(PixelRow(row));
                for (uint32_t x = 0; x < m_width; ++x) line[x] = background;
            }
        }
    }

    void ScreenTarget::NewLine() {
        ++m_row;
        m_column = 0;
        if (m_row < m_rows) return;
        if (m_mode == Mode::Text) ScrollText();
        else ScrollFramebuffer();
        m_row = m_rows - 1;
    }

    void ScreenTarget::WriteTextCell(uint8_t character) {
        volatile uint8_t* const cell = m_base + static_cast<uint64_t>(m_row) * m_stride + static_cast<uint64_t>(m_column) * kTextCellBytes;
        cell[0] = character;
        cell[1] = m_attribute;
    }

    void ScreenTarget::WriteGlyph(uint8_t character) {
        const uint8_t* const glyph = Font::Glyph(character);
        const uint32_t foreground = ForegroundPixel();
        const uint32_t background = BackgroundPixel();
        for (uint32_t line = 0; line < kGlyphHeight; ++line) {
            const uint64_t offset = static_cast<uint64_t>(m_row) * kGlyphHeight * m_stride
                + static_cast<uint64_t>(line) * m_stride
                + static_cast<uint64_t>(m_column) * kGlyphWidth * kBytesPerPixel;
            volatile uint32_t* const pixels = reinterpret_cast<volatile uint32_t*>(m_base + offset);
            const uint8_t bits = glyph != nullptr ? glyph[line] : 0;
            for (uint32_t column = 0; column < kGlyphWidth; ++column) {
                const bool set = (bits & static_cast<uint8_t>(0x80 >> column)) != 0;
                pixels[column] = set ? foreground : background;
            }
        }
    }

    void ScreenTarget::ScrollText() {
        // 先把第 1..rows-1 行搬到上一行，再把末行填成空格 + 当前属性
        const uint32_t rowBytes = m_stride;
        const uint64_t rowCells = static_cast<uint64_t>(m_width) * kTextCellBytes;
        for (uint32_t row = 0; row + 1 < m_rows; ++row) {
            volatile uint8_t* const to = m_base + static_cast<uint64_t>(row) * rowBytes;
            const volatile uint8_t* const from = m_base + static_cast<uint64_t>(row + 1) * rowBytes;
            for (uint32_t byte = 0; byte < rowCells; ++byte) to[byte] = from[byte];
        }
        volatile uint8_t* const last = m_base + static_cast<uint64_t>(m_rows - 1) * rowBytes;
        for (uint32_t column = 0; column < m_width; ++column) {
            last[column * kTextCellBytes] = static_cast<uint8_t>(' ');
            last[column * kTextCellBytes + 1] = m_attribute;
        }
    }

    void ScreenTarget::ScrollFramebuffer() {
        // 整屏按 16 个像素行上移，末 16 行填背景色
        const uint32_t background = BackgroundPixel();
        for (uint32_t row = 0; row < m_height; ++row) {
            volatile uint8_t* const to = PixelRow(row);
            const volatile uint8_t* from = row + kGlyphHeight < m_height ? PixelRow(row + kGlyphHeight) : nullptr;
            if (from == nullptr) {
                volatile uint32_t* const line = reinterpret_cast<volatile uint32_t*>(to);
                for (uint32_t x = 0; x < m_width; ++x) line[x] = background;
                continue;
            }
            // 逐行按字节搬：一行最多几 KiB，按 32 位字搬更快，但源与目标都按 4 字节对齐
            volatile uint32_t* const target = reinterpret_cast<volatile uint32_t*>(to);
            const volatile uint32_t* const source = reinterpret_cast<const volatile uint32_t*>(from);
            for (uint32_t word = 0; word < m_width; ++word) target[word] = source[word];
        }
    }

    uint32_t ScreenTarget::PixelValue(uint8_t color) const {
        const uint32_t rgb = kPalette[color & 0x0F];
        const uint32_t red = (rgb >> 16) & 0xFF;
        const uint32_t green = (rgb >> 8) & 0xFF;
        const uint32_t blue = rgb & 0xFF;
        // 32 位打包：Bgrx8888 的字节序是 B-G-R-X，Rgbx8888 是 R-G-B-X；X 填全 1
        if (m_format == Boot::ScreenFormat::Rgbx8888) return 0xFF000000u | (blue << 16) | (green << 8) | red;
        return 0xFF000000u | (red << 16) | (green << 8) | blue;
    }
}
