/* PrintTarget.cpp
    Baleen BIOS 路径引导期控制台的实现：显示模式判定、目标组合与安装，诊断行标签前缀的写入
*/

#include <PrintTarget.hpp>

#include <Diagnostics.hpp>
#include <Platform/PhysicalMemory.hpp>

namespace Baleen {
    namespace PrintTargets {
        namespace {
            // BIOS 数据区的当前显示模式与列数：0040:0049 与 0040:004A
            constexpr uintptr_t kBdaBase = 0x400;      // BIOS 数据区起点
            constexpr uintptr_t kBdaVideoMode = 0x49;  // 当前显示模式
            constexpr uintptr_t kBdaColumns = 0x4A;    // 当前文本列数

            // 当前显示设备是否处于 80×25 文本模式：模式 2（单色）、3（彩色）与 7（单色）是文本模式，
            // 列数由 BDA 佐证。固件或 CSM 在图形模式下交权时显存直写不会显示，装配方据此退化
            bool TextModeAvailable() {
                const volatile uint8_t* const bda = Platform::PhysicalBytePointer(kBdaBase);
                const uint8_t mode = bda[kBdaVideoMode];
                if (mode != 2 && mode != 3 && mode != 7) return false;
                return bda[kBdaColumns] == Devices::Vga::kColumns;
            }
        }

        void WriteTag(Tag tag) {
            Print::Write("[ ");
            // 设置标签颜色
            Print::SetAttribute(TagAttribute(tag));
            Print::Write(TagName(tag));
            // 恢复默认颜色
            Print::SetAttribute(Devices::Vga::kDefaultAttribute);
            Print::Write(" ] ");
        }

        void WriteLine(Tag tag, const char* text) {
            WriteTag(tag);
            if (text != nullptr) Print::Write(text);
            Print::Write("\r\n");
        }

        void BiosConsole::Install(Devices::Vga& vga, Print::TeletypeTarget::WriteFn teletype, const Diagnostic* fallback) {
            m_multiplex.Clear();
            const bool text = TextModeAvailable();
            if (text) {
                vga.Initialize();
                m_text.Initialize(vga);
                m_multiplex.Add(&m_text);
            } else if (teletype != nullptr) {
                m_teletype.Initialize(teletype);
                m_multiplex.Add(&m_teletype);
            }
            m_debug.Initialize();
            m_serial.Initialize();
            m_multiplex.Add(&m_debug);
            m_multiplex.Add(&m_serial);
            Print::SetTarget(&m_multiplex);
            // 退化不是静默的：流式通道已装好，再用带编号的 WARN 说明屏幕输出走了哪条路
            if (!text && fallback != nullptr) WriteDiagnostic(Tag::Warn, *fallback);
        }
    }
}
