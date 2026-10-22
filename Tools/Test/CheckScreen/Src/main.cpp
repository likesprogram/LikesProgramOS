/* main.cpp
    CheckScreen：内核自绘屏幕目标与 8×16 字模的宿主侧检查，手工执行，不构成默认构建依赖

    检查对象是 Packages/LikesProgramOS/Core/Src/ScreenTarget.cpp 与 Font.cpp 的目标侧源码：
    用 vpath 按相对路径直接编进本工具，不复制实现
    宿主替身提供低 4GiB 内的可写缓冲（mmap MAP_32BIT），把缓冲地址当屏幕物理地址注入；
    有效区前后各留守卫字节，任何越界写都会让用例变红
    用例覆盖：字模字符范围与几何、VGA 文本页的字符 + 属性 / 步长 / 末行末列 / 上滚、
    线性帧缓冲的 Bgrx8888 与 Rgbx8888 像素值 / 步长填充 / 边界 / 上滚、
    屏幕事实不成立时 Ready() 为 false 且不写内存、不可用目标的写入被静默丢弃（无通道降级）

    每个用例打印 [ OK ] 或 [ FAIL ]；用例数固定，失败返回 1
*/

#include <Font.hpp>
#include <Print.hpp>
#include <ScreenTarget.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/mman.h>

namespace {
    // 用例总数：与用例表长度核对，防止用例被误删
    constexpr unsigned kCaseCount = 39;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;
    // 守卫字节数：有效区前后各留这么多字节
    constexpr std::size_t kGuardBytes = 64;
    // 守卫填充值
    constexpr std::uint8_t kGuardValue = 0xA5;
    // 有效区初始值：确认"没有写过"用
    constexpr std::uint8_t kUnwritten = 0xCC;
    // VGA 文本模式的固定几何：80×25，每单元 2 字节，步长 160，共 4000 字节
    constexpr std::uint32_t kTextColumns = 80;
    constexpr std::uint32_t kTextRows = 25;
    constexpr std::uint32_t kTextStride = 160;
    constexpr std::uint32_t kTextBytes = 4000;
    // 帧缓冲测试的固定几何：64×32 像素，每像素 4 字节，步长 256，共 8192 字节
    constexpr std::uint32_t kFrameWidth = 64;
    constexpr std::uint32_t kFrameHeight = 32;
    constexpr std::uint32_t kFrameStride = 256;
    constexpr std::uint32_t kFrameBytes = 8192;
    // VGA 调色板的固定取值：黑 0x000000、红 0xAA0000、黄 0xFFFF55
    constexpr std::uint32_t kBlack = 0x000000;
    constexpr std::uint32_t kRed = 0xAA0000;
    constexpr std::uint32_t kYellow = 0xFFFF55;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) {
        if (!value) Fail(message);
    }

    // Bgrx8888 的 32 位打包值：小端字节序 B-G-R-X，X 填全 1
    constexpr std::uint32_t Bgrx(std::uint32_t rgb) { return 0xFF000000u | rgb; }

    // Rgbx8888 的 32 位打包值：小端字节序 R-G-B-X，X 填全 1
    constexpr std::uint32_t Rgbx(std::uint32_t rgb) {
        return 0xFF000000u | ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);
    }

    // 32 位地址空间内的宿主内存替身：缓冲地址就是注入的屏幕物理地址
    class GuardedBuffer {
    public:
        // 申请 bytes 字节有效区，前后各留 kGuardBytes 守卫
        explicit GuardedBuffer(std::size_t bytes) : m_bytes(bytes) {
            const std::size_t total = kGuardBytes * 2 + bytes;
            void* const memory = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
            if (memory == MAP_FAILED) Fail("cannot allocate the host screen buffer");
            m_memory = static_cast<std::uint8_t*>(memory);
            m_total = total;
            std::memset(m_memory, kGuardValue, total);
            std::memset(m_memory + kGuardBytes, kUnwritten, bytes);
        }
        // 释放
        ~GuardedBuffer() { ::munmap(m_memory, m_total); }
        // 内存与指针一一对应，不允许拷贝
        GuardedBuffer(const GuardedBuffer&) = delete;
        GuardedBuffer& operator=(const GuardedBuffer&) = delete;
        // 有效区起址：直接当屏幕物理地址用，必须落在低 4GiB
        std::uint64_t Address() const {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m_memory + kGuardBytes));
        }
        // 有效区数据
        std::uint8_t* Data() { return m_memory + kGuardBytes; }
        // 读有效区的一个字节
        std::uint8_t Byte(std::size_t offset) const { return m_memory[kGuardBytes + offset]; }
        // 读有效区的一个 32 位像素
        std::uint32_t Pixel(std::size_t offset) const {
            std::uint32_t value = 0;
            std::memcpy(&value, m_memory + kGuardBytes + offset, sizeof(value));
            return value;
        }
        // 守卫字节是否完好
        bool GuardsIntact() const {
            for (std::size_t i = 0; i < kGuardBytes; ++i) {
                if (m_memory[i] != kGuardValue) return false;
                if (m_memory[kGuardBytes + m_bytes + i] != kGuardValue) return false;
            }
            return true;
        }
        // 有效区是否全部保持未写状态
        bool Untouched() const {
            for (std::size_t i = 0; i < m_bytes; ++i) {
                if (m_memory[kGuardBytes + i] != kUnwritten) return false;
            }
            return true;
        }
        // 有效区字节数
        std::size_t Bytes() const { return m_bytes; }
    private:
        std::uint8_t* m_memory = nullptr;   // mmap 得到的整块内存
        std::size_t m_total = 0;            // 整块字节数
        std::size_t m_bytes = 0;            // 有效区字节数
    };

    // 一个 VGA 文本单元：字符与属性
    struct Cell {
        std::uint8_t character;   // 字符字节
        std::uint8_t attribute;   // 属性字节
    };

    // 读 VGA 文本页的一个单元
    Cell TextCell(const GuardedBuffer& buffer, std::uint32_t column, std::uint32_t row) {
        const std::size_t offset = static_cast<std::size_t>(row) * kTextStride + static_cast<std::size_t>(column) * 2;
        return { buffer.Byte(offset), buffer.Byte(offset + 1) };
    }

    // 安装一个规范的 VGA 文本目标
    void InstallVga(Kernel::ScreenTarget& target, const GuardedBuffer& buffer) {
        target.Install(Boot::ScreenKind::VgaText, buffer.Address(), kTextColumns, kTextRows, kTextStride, kTextBytes, Boot::ScreenFormat::VgaText);
    }

    // 安装一个规范的 Bgrx8888 帧缓冲目标
    void InstallFramebuffer(Kernel::ScreenTarget& target, const GuardedBuffer& buffer) {
        target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
    }

    // 帧缓冲里字符格的像素偏移：列以字符格为单位
    std::size_t FrameCellOffset(std::uint32_t column, std::uint32_t row) {
        return static_cast<std::size_t>(row) * Kernel::ScreenTarget::kGlyphHeight * kFrameStride
            + static_cast<std::size_t>(column) * Kernel::ScreenTarget::kGlyphWidth * 4;
    }

    // 字模位图里第 line 行第 column 列是否有墨
    bool GlyphBit(const std::uint8_t* glyph, std::uint32_t line, std::uint32_t column) {
        if (glyph == nullptr) return false;
        return (glyph[line] & static_cast<std::uint8_t>(0x80 >> column)) != 0;
    }

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // ---- 字模 ----
        cases.push_back({ "font covers printable ASCII", [] {
            for (std::uint32_t character = Kernel::Font::kFirstCharacter; character <= Kernel::Font::kLastCharacter; ++character) {
                const std::uint8_t* const glyph = Kernel::Font::Glyph(static_cast<std::uint8_t>(character));
                Require(glyph != nullptr, "printable ASCII character has no glyph");
                Require(glyph == Kernel::Font::Bitmap() + (character - Kernel::Font::kFirstCharacter) * Kernel::Font::kGlyphBytes,
                    "glyph pointer isn't the bitmap slot for the character");
            }
        } });
        cases.push_back({ "font rejects non-printable bytes", [] {
            for (const std::uint8_t character : { std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{0x1F}, std::uint8_t{0x7F}, std::uint8_t{0x80}, std::uint8_t{0xFF} }) {
                Require(Kernel::Font::Glyph(character) == nullptr, "non-printable byte returned a glyph");
            }
        } });
        cases.push_back({ "font space is blank and letters have ink", [] {
            const std::uint8_t* const space = Kernel::Font::Glyph(' ');
            const std::uint8_t* const letter = Kernel::Font::Glyph('A');
            Require(space != nullptr && letter != nullptr, "glyphs are missing");
            bool spaceBlank = true;
            for (std::uint32_t line = 0; line < Kernel::Font::kGlyphBytes; ++line) {
                if (space[line] != 0) spaceBlank = false;
            }
            Require(spaceBlank, "space glyph isn't blank");
            bool letterInk = false;
            for (std::uint32_t line = 0; line < Kernel::Font::kGlyphBytes; ++line) {
                if (letter[line] != 0) letterInk = true;
            }
            Require(letterInk, "letter glyph has no ink");
        } });
        cases.push_back({ "font geometry constants", [] {
            Require(Kernel::Font::kGlyphWidth == 8 && Kernel::Font::kGlyphHeight == 16, "glyph size isn't 8x16");
            Require(Kernel::Font::kFirstCharacter == 0x20 && Kernel::Font::kLastCharacter == 0x7E, "glyph range isn't printable ASCII");
            Require(Kernel::Font::kGlyphCount == 95, "glyph count isn't 95");
            Require(Kernel::Font::kGlyphBytes == 16 && Kernel::Font::kBitmapBytes == 1520, "glyph bitmap size is wrong");
            Require(Kernel::ScreenTarget::kGlyphWidth == Kernel::Font::kGlyphWidth, "screen and font glyph widths differ");
            Require(Kernel::ScreenTarget::kGlyphHeight == Kernel::Font::kGlyphHeight, "screen and font glyph heights differ");
        } });
        // ---- VGA 文本 ----
        cases.push_back({ "VGA text install reports geometry", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            InstallVga(target, buffer);
            Require(target.Ready(), "VGA target isn't ready");
            Require(target.Columns() == kTextColumns && target.Rows() == kTextRows, "VGA geometry is wrong");
        } });
        cases.push_back({ "VGA clear fills spaces with the default attribute", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            InstallVga(target, buffer);
            for (std::uint32_t row = 0; row < kTextRows; ++row) {
                for (std::uint32_t column = 0; column < kTextColumns; ++column) {
                    const Cell cell = TextCell(buffer, column, row);
                    Require(cell.character == ' ', "cleared cell isn't a space");
                    Require(cell.attribute == Kernel::ScreenTarget::kDefaultAttribute, "cleared cell attribute is wrong");
                }
            }
            Require(buffer.GuardsIntact(), "clear wrote outside the text buffer");
        } });
        cases.push_back({ "VGA write puts character and attribute", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            InstallVga(target, buffer);
            target.SetAttribute(0x1E);
            target.Write("AB");
            const Cell first = TextCell(buffer, 0, 0);
            const Cell second = TextCell(buffer, 1, 0);
            Require(first.character == 'A' && first.attribute == 0x1E, "first cell is wrong");
            Require(second.character == 'B' && second.attribute == 0x1E, "second cell is wrong");
            Require(buffer.GuardsIntact(), "write went outside the text buffer");
        } });
        cases.push_back({ "VGA CR and LF move the cursor", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            InstallVga(target, buffer);
            target.Write("AB\rC\nD");
            Require(TextCell(buffer, 0, 0).character == 'C', "carriage return didn't return to the first column");
            Require(TextCell(buffer, 0, 1).character == 'D', "line feed didn't move to the next row");
        } });
        cases.push_back({ "VGA column wrap starts a new line", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            InstallVga(target, buffer);
            std::string line(kTextColumns, 'X');
            line.push_back('Y');
            target.Write(line.c_str());
            Require(TextCell(buffer, 0, 0).character == 'X', "first row isn't filled");
            Require(TextCell(buffer, kTextColumns - 1, 0).character == 'X', "last column of the first row isn't filled");
            Require(TextCell(buffer, 0, 1).character == 'Y', "wrapped character isn't on the next row");
        } });
        cases.push_back({ "VGA last row scrolls", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            InstallVga(target, buffer);
            std::string fill(static_cast<std::size_t>(kTextColumns) * kTextRows, 'X');
            target.Write(fill.c_str());
            target.Write("Y");
            Require(TextCell(buffer, 0, 0).character == 'X', "top row isn't the previous second row after scrolling");
            Require(TextCell(buffer, 0, kTextRows - 1).character == 'Y', "scrolled last row doesn't start with the new character");
            Require(TextCell(buffer, 1, kTextRows - 1).character == ' ', "scrolled last row isn't cleared");
            Require(buffer.GuardsIntact(), "scroll wrote outside the text buffer");
        } });
        cases.push_back({ "VGA kind None keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::None, buffer.Address(), kTextColumns, kTextRows, kTextStride, kTextBytes, Boot::ScreenFormat::VgaText);
            Require(!target.Ready(), "target with no screen kind is ready");
            Require(target.Columns() == 0 && target.Rows() == 0, "unavailable target reports a geometry");
            Require(buffer.Untouched(), "unavailable target wrote to the buffer");
        } });
        cases.push_back({ "VGA zero address keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::VgaText, 0, kTextColumns, kTextRows, kTextStride, kTextBytes, Boot::ScreenFormat::VgaText);
            Require(!target.Ready(), "target with a zero address is ready");
            Require(buffer.Untouched(), "unavailable target wrote to the buffer");
        } });
        cases.push_back({ "VGA address above the window keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::VgaText, 0x100000000ull, kTextColumns, kTextRows, kTextStride, kTextBytes, Boot::ScreenFormat::VgaText);
            Require(!target.Ready(), "target above the identity window is ready");
        } });
        cases.push_back({ "VGA format mismatch keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::VgaText, buffer.Address(), kTextColumns, kTextRows, kTextStride, kTextBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "text target accepted a framebuffer pixel format");
        } });
        cases.push_back({ "VGA zero width or height keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget first;
            first.Install(Boot::ScreenKind::VgaText, buffer.Address(), 0, kTextRows, kTextStride, kTextBytes, Boot::ScreenFormat::VgaText);
            Require(!first.Ready(), "target with zero width is ready");
            Kernel::ScreenTarget second;
            second.Install(Boot::ScreenKind::VgaText, buffer.Address(), kTextColumns, 0, kTextStride, kTextBytes, Boot::ScreenFormat::VgaText);
            Require(!second.Ready(), "target with zero height is ready");
        } });
        cases.push_back({ "VGA width or height above the limit keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget first;
            first.Install(Boot::ScreenKind::VgaText, buffer.Address(), 133, kTextRows, 266, 4000, Boot::ScreenFormat::VgaText);
            Require(!first.Ready(), "target above the column limit is ready");
            Kernel::ScreenTarget second;
            second.Install(Boot::ScreenKind::VgaText, buffer.Address(), kTextColumns, 65, kTextStride, 4000, Boot::ScreenFormat::VgaText);
            Require(!second.Ready(), "target above the row limit is ready");
        } });
        cases.push_back({ "VGA stride too small keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::VgaText, buffer.Address(), kTextColumns, kTextRows, kTextStride - 2, kTextBytes, Boot::ScreenFormat::VgaText);
            Require(!target.Ready(), "target with a short stride is ready");
        } });
        cases.push_back({ "VGA bytes too small keeps the target unavailable", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::VgaText, buffer.Address(), kTextColumns, kTextRows, kTextStride, kTextBytes - 1, Boot::ScreenFormat::VgaText);
            Require(!target.Ready(), "target whose buffer can't hold the last row is ready");
        } });
        cases.push_back({ "VGA non-ASCII writes the raw byte", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            InstallVga(target, buffer);
            target.Write("\x80");
            const Cell cell = TextCell(buffer, 0, 0);
            Require(cell.character == 0x80, "text mode didn't write the raw byte");
            Require(cell.attribute == Kernel::ScreenTarget::kDefaultAttribute, "raw byte got a wrong attribute");
        } });
        // ---- 帧缓冲 ----
        cases.push_back({ "framebuffer install reports character geometry", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            InstallFramebuffer(target, buffer);
            Require(target.Ready(), "framebuffer target isn't ready");
            Require(target.Columns() == kFrameWidth / 8 && target.Rows() == kFrameHeight / 16, "framebuffer geometry is wrong");
        } });
        cases.push_back({ "framebuffer clear fills the background", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            InstallFramebuffer(target, buffer);
            const std::uint32_t background = Bgrx(kBlack);
            for (std::uint32_t offset = 0; offset < kFrameBytes; offset += 4) {
                Require(buffer.Pixel(offset) == background, "cleared pixel isn't the background colour");
            }
            Require(buffer.GuardsIntact(), "clear wrote outside the framebuffer");
        } });
        cases.push_back({ "framebuffer glyph draws foreground and background", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            InstallFramebuffer(target, buffer);
            target.SetAttribute(0x4E);
            target.Write("A");
            const std::uint32_t background = Bgrx(kRed);
            const std::uint32_t foreground = Bgrx(kYellow);
            const std::uint8_t* const glyph = Kernel::Font::Glyph('A');
            const std::size_t cell = FrameCellOffset(0, 0);
            for (std::uint32_t line = 0; line < 16; ++line) {
                for (std::uint32_t column = 0; column < 8; ++column) {
                    const std::uint32_t expected = GlyphBit(glyph, line, column) ? foreground : background;
                    const std::uint32_t actual = buffer.Pixel(cell + line * kFrameStride + column * 4);
                    Require(actual == expected, "framebuffer glyph pixel is wrong");
                }
            }
            Require(buffer.GuardsIntact(), "glyph wrote outside the framebuffer");
        } });
        cases.push_back({ "framebuffer RGBX pixel order", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes, Boot::ScreenFormat::Rgbx8888);
            Require(target.Ready(), "RGBX framebuffer target isn't ready");
            target.SetAttribute(0x4E);
            target.ClearScreen();
            Require(buffer.Pixel(0) == Rgbx(kRed), "RGBX background pixel order is wrong");
            target.Write("A");
            const std::uint8_t* const glyph = Kernel::Font::Glyph('A');
            const std::uint32_t foreground = Rgbx(kYellow);
            for (std::uint32_t line = 0; line < 16; ++line) {
                for (std::uint32_t column = 0; column < 8; ++column) {
                    if (!GlyphBit(glyph, line, column)) continue;
                    Require(buffer.Pixel(line * kFrameStride + column * 4) == foreground, "RGBX foreground pixel order is wrong");
                }
            }
        } });
        cases.push_back({ "framebuffer stride padding stays untouched", [] {
            constexpr std::uint32_t kWidth = 16;
            constexpr std::uint32_t kHeight = 32;
            constexpr std::uint32_t kStride = 128;
            constexpr std::uint32_t kBytes = kStride * kHeight;
            GuardedBuffer buffer(kBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kWidth, kHeight, kStride, kBytes, Boot::ScreenFormat::Bgrx8888);
            Require(target.Ready(), "framebuffer target with padded stride isn't ready");
            Require(target.Columns() == 2 && target.Rows() == 2, "padded framebuffer geometry is wrong");
            for (std::uint32_t row = 0; row < kHeight; ++row) {
                for (std::uint32_t offset = kWidth * 4; offset < kStride; ++offset) {
                    Require(buffer.Byte(row * kStride + offset) == kUnwritten, "framebuffer wrote into the stride padding");
                }
                Require(buffer.Pixel(row * kStride) == Bgrx(kBlack), "padded framebuffer pixel is wrong");
            }
            Require(buffer.GuardsIntact(), "padded framebuffer wrote outside the buffer");
        } });
        cases.push_back({ "framebuffer scroll moves rows and clears the tail", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            InstallFramebuffer(target, buffer);
            target.SetAttribute(0x0F);
            target.Write("AAAAAAAA");
            target.Write("BBBBBBBB");
            target.Write("C");
            const std::uint8_t* const letter = Kernel::Font::Glyph('B');
            const std::uint32_t background = Bgrx(kBlack);
            const std::uint32_t foreground = Bgrx(0xFFFFFF);
            for (std::uint32_t line = 0; line < 16; ++line) {
                for (std::uint32_t column = 0; column < 8; ++column) {
                    const std::uint32_t expected = GlyphBit(letter, line, column) ? foreground : background;
                    Require(buffer.Pixel(line * kFrameStride + column * 4) == expected, "scrolled framebuffer row is wrong");
                }
            }
            for (std::uint32_t line = 16; line < kFrameHeight; ++line) {
                for (std::uint32_t column = 0; column < kFrameWidth; ++column) {
                    const std::uint32_t actual = buffer.Pixel(line * kFrameStride + column * 4);
                    if (line >= 16 && line < 32 && column < 8) continue;
                    Require(actual == background, "scrolled framebuffer tail isn't the background");
                }
            }
            Require(buffer.GuardsIntact(), "framebuffer scroll wrote outside the buffer");
        } });
        cases.push_back({ "framebuffer non-ASCII draws blank", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            InstallFramebuffer(target, buffer);
            target.Write("\xFF");
            const std::uint32_t background = Bgrx(kBlack);
            const std::size_t cell = FrameCellOffset(0, 0);
            for (std::uint32_t line = 0; line < 16; ++line) {
                for (std::uint32_t column = 0; column < 8; ++column) {
                    Require(buffer.Pixel(cell + line * kFrameStride + column * 4) == background, "non-ASCII character drew ink");
                }
            }
        } });
        cases.push_back({ "framebuffer last column wraps", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            InstallFramebuffer(target, buffer);
            target.Write("AAAAAAAAA");
            const std::size_t secondRow = FrameCellOffset(0, 1);
            const std::uint8_t* const glyph = Kernel::Font::Glyph('A');
            const std::uint32_t foreground = Bgrx(0xFFFFFF);
            bool wrappedInk = false;
            for (std::uint32_t line = 0; line < 16; ++line) {
                for (std::uint32_t column = 0; column < 8; ++column) {
                    if (!GlyphBit(glyph, line, column)) continue;
                    if (buffer.Pixel(secondRow + line * kFrameStride + column * 4) == foreground) wrappedInk = true;
                }
            }
            Require(wrappedInk, "wrapped framebuffer character didn't land on the next row");
            Require(buffer.GuardsIntact(), "framebuffer wrap wrote outside the buffer");
        } });
        cases.push_back({ "framebuffer kind None keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::None, buffer.Address(), kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "target with no screen kind is ready");
            Require(buffer.Untouched(), "unavailable target wrote to the framebuffer");
        } });
        cases.push_back({ "framebuffer zero address keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, 0, kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "framebuffer target with a zero address is ready");
            Require(buffer.Untouched(), "unavailable target wrote to the framebuffer");
        } });
        cases.push_back({ "framebuffer unaligned address keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address() | 1, kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "framebuffer target with an unaligned address is ready");
        } });
        cases.push_back({ "framebuffer address above the window keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, 0x100000000ull, kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "framebuffer target above the identity window is ready");
        } });
        cases.push_back({ "framebuffer text format keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes, Boot::ScreenFormat::VgaText);
            Require(!target.Ready(), "framebuffer target accepted the text pixel format");
        } });
        cases.push_back({ "framebuffer geometry below one glyph keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget first;
            first.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), 7, kFrameHeight, 28, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!first.Ready(), "framebuffer narrower than a glyph is ready");
            Kernel::ScreenTarget second;
            second.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kFrameWidth, 15, kFrameStride, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!second.Ready(), "framebuffer shorter than a glyph is ready");
        } });
        cases.push_back({ "framebuffer geometry above the limit keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), 8193, kFrameHeight, 8193 * 4, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "framebuffer above the pixel limit is ready");
        } });
        cases.push_back({ "framebuffer unaligned stride keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kFrameWidth, kFrameHeight, kFrameStride + 1, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "framebuffer target with an unaligned stride is ready");
        } });
        cases.push_back({ "framebuffer short stride keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kFrameWidth, kFrameHeight, kFrameStride - 4, kFrameBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "framebuffer target with a short stride is ready");
        } });
        cases.push_back({ "framebuffer bytes too small keeps the target unavailable", [] {
            GuardedBuffer buffer(kFrameBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::Framebuffer, buffer.Address(), kFrameWidth, kFrameHeight, kFrameStride, kFrameBytes - 4, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "framebuffer whose buffer can't hold the last row is ready");
        } });
        cases.push_back({ "unavailable target discards writes without touching memory", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            target.Install(Boot::ScreenKind::VgaText, buffer.Address(), kTextColumns, kTextRows, kTextStride, kTextBytes, Boot::ScreenFormat::Bgrx8888);
            Require(!target.Ready(), "target with mismatched format is ready");
            target.Write("KERNEL");
            target.SetAttribute(0x1E);
            target.ClearScreen();
            Require(buffer.Untouched(), "unavailable target wrote to the buffer");
            Require(buffer.GuardsIntact(), "unavailable target wrote outside the buffer");
        } });
        cases.push_back({ "uninstalled target discards writes", [] {
            GuardedBuffer buffer(kTextBytes);
            Kernel::ScreenTarget target;
            Require(!target.Ready(), "fresh target is ready");
            target.Write("KERNEL");
            target.SetAttribute(0x1E);
            target.ClearScreen();
            Require(buffer.Untouched(), "fresh target wrote to the buffer");
            Require(target.Columns() == 0 && target.Rows() == 0, "fresh target reports a geometry");
        } });
        return cases;
    }
}

// 入口：跑全部用例并汇总；任一失败返回 1
int main() {
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckScreen: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckScreen: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckScreen: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
