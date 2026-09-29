/* PrintTarget.hpp
    Baleen BIOS 路径引导期控制台：组合 VGA 文本、调试口与串口并安装为当前 Print 目标，
    另给出诊断行前缀的标签写法：方括号内的标签在屏幕上有颜色，流式通道只收到纯文本
*/

#pragma once
#include <stdint.h>

#include <Print.hpp>
#include <Print/DebugPortTarget.hpp>
#include <Print/SerialTarget.hpp>
#include <Print/VgaTextTarget.hpp>

namespace Baleen {
    namespace PrintTargets {
        // 诊断行前缀的标签：方括号内的内容，模块名与等级名同属一套写法
        enum class Tag : uint8_t {
            Stub,     // 模块名：Stub 阶段
            Info,     // 状态说明，与模块名同为白字黑底
            Debug,    // 诊断值
            Ok,       // 步骤已推进
            Warn,     // 异常但继续
            Error,    // 非致命错误
            Fatal,    // 致命错误，随后停机
        };

        // 标签的显示属性字节：模块名与 INFO 用缺省的白字黑底，其余各有颜色
        // 属性只在支持显示属性的目标上生效，取值按 VGA 文本模式的属性字节
        constexpr uint8_t TagAttribute(Tag tag) {
            switch (tag) {
                case Tag::Stub:
                case Tag::Info: return Devices::Vga::kDefaultAttribute;
                case Tag::Debug: return 0x07;   // 亮灰字黑底
                case Tag::Ok: return 0x0A;      // 亮绿字黑底
                case Tag::Warn: return 0x0E;    // 亮黄字黑底
                case Tag::Error: return 0x0C;   // 亮红字黑底
                case Tag::Fatal: return 0xCF;   // 白字亮红底
            }
            return Devices::Vga::kDefaultAttribute;
        }

        // 标签的显示文本，不含方括号与两侧空格
        constexpr const char* TagName(Tag tag) {
            switch (tag) {
                case Tag::Stub: return "STUB";
                case Tag::Info: return "INFO";
                case Tag::Debug: return "DEBUG";
                case Tag::Ok: return "OK";
                case Tag::Warn: return "WARN";
                case Tag::Error: return "ERROR";
                case Tag::Fatal: return "FATAL";
            }
            return "?";   // 枚举扩展后未同步本表时的兜底，漏项正常由 -Wswitch 在构建期拦下
        }

        // 写入 "[ 标签 ] " 前缀：标签按等级着色，写完立即恢复缺省属性，着色范围只限方括号内
        void WriteTag(Tag tag);

        // 写入 "[ 标签 ] 文本\r\n" 一整行；文本为空指针时只写标签
        void WriteLine(Tag tag, const char* text);

        // BIOS 路径引导期控制台：显存直写给人看，0xE9 与串口供自动化与无头机器抓取
        // 不走 BIOS 电传是有意的：CSM 下电传是逐字符固件调用，真机上慢到肉眼可见，
        // 而 IPL 交权后仍处于固件给出的文本模式，显存直写快几个数量级
        class BiosConsole {
        public:
            // 初始化绑定的 VGA 设备，组合三路目标并安装为当前 Print 目标；可重复调用
            void Install(Devices::Vga& vga);
        private:
            Print::VgaTextTarget m_text;          // VGA 文本目标
            Print::DebugPortTarget m_debug;       // 0xE9 调试口目标
            Print::SerialTarget m_serial;         // COM1 串口目标
            Print::MultiplexTarget m_multiplex;   // 扇出目标
        };
    }
}
