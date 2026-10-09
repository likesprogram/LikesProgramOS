/* Tags.hpp
    内核诊断行前缀的标签：方括号内的标签文本，与引导期标签同一套写法与等级语义

    内核侧不装配 VGA 文本目标，标签没有显示属性可设，流式通道收到的是纯文本；
    等级语义见 Docs/Specs/输出通道与Print.md：OK 已完成、INFO 状态说明、... 续行、
    DEBUG 诊断值、WARN 异常但继续、ERROR 非致命错误、FATAL 致命错误且随后停机
*/

#pragma once
#include <stdint.h>

namespace Kernel {
    namespace Tags {
        // 诊断行前缀的标签：模块名与等级名同属一套写法
        enum class Tag : uint8_t {
            Continue,   // 延续上一行信息
            Kernel,     // 模块名：内核
            Info,       // 状态说明与即将进行的动作
            Debug,      // 诊断值
            Ok,         // 步骤已推进
            Warn,       // 异常但继续
            Error,      // 非致命错误
            Fatal,      // 致命错误，随后停机
        };

        // 标签的显示文本，不含方括号与两侧空格
        constexpr const char* TagName(Tag tag) {
            switch (tag) {
                case Tag::Continue: return "...";
                case Tag::Kernel: return "KERNEL";
                case Tag::Info: return "INFO";
                case Tag::Debug: return "DEBUG";
                case Tag::Ok: return "OK";
                case Tag::Warn: return "WARN";
                case Tag::Error: return "ERROR";
                case Tag::Fatal: return "FATAL";
            }
            return "?";   // 枚举扩展后未同步本表时的兜底，漏项正常由 -Wswitch 在构建期拦下
        }

        // 写入 "[ 标签 ] " 前缀
        void WriteTag(Tag tag);

        // 写入 "[ 标签 ] 文本\r\n" 一整行；文本为空指针时只写标签
        void WriteLine(Tag tag, const char* text);
    }
}
