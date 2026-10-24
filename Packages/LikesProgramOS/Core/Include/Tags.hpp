/* Tags.hpp
    内核诊断行前缀的标签：方括号内的标签文本，与引导期标签同一套写法与等级语义

    屏幕目标存在时标签按与引导期同一张属性表着色，写完立即恢复缺省属性；串口与调试口这类
    流式通道忽略属性，只收到纯文本
    等级语义：OK 已完成、INFO 状态说明、... 续行、
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

        // 缺省显示属性：白字黑底，与引导期的缺省属性一致
        constexpr uint8_t kDefaultAttribute = 0x0F;

        // 标签的显示属性字节：与引导期同一张属性表，取值按 VGA 文本模式的属性字节
        constexpr uint8_t TagAttribute(Tag tag) {
            switch (tag) {
                case Tag::Kernel:
                case Tag::Info: return kDefaultAttribute;
                case Tag::Continue:
                case Tag::Debug: return 0x07;   // 亮灰字黑底
                case Tag::Ok: return 0x0A;      // 亮绿字黑底
                case Tag::Warn: return 0x0E;    // 亮黄字黑底
                case Tag::Error: return 0x0C;   // 亮红字黑底
                case Tag::Fatal: return 0xCF;   // 白字亮红底
            }
            return kDefaultAttribute;
        }

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

        // 两段文本是否完全相同：标签名是编译期常量，避免为比较引入运行库 strcmp
        constexpr bool SameText(const char* left, const char* right) {
            uint32_t index = 0;
            while (left[index] != '\0' && right[index] != '\0') {
                if (left[index] != right[index]) return false;
                ++index;
            }
            return left[index] == right[index];
        }

        // 按标签文本取显示属性：模块经 Core.Log 传的是字符串标签，与本文件的等级名共用同一张属性表
        // 未登记的标签（模块名，如 AHCI / VFS / EXT4）按缺省属性处理
        constexpr uint8_t TagAttributeByName(const char* name) {
            if (name == nullptr) return kDefaultAttribute;
            if (SameText(name, TagName(Tag::Continue))) return TagAttribute(Tag::Continue);
            if (SameText(name, TagName(Tag::Info))) return TagAttribute(Tag::Info);
            if (SameText(name, TagName(Tag::Debug))) return TagAttribute(Tag::Debug);
            if (SameText(name, TagName(Tag::Ok))) return TagAttribute(Tag::Ok);
            if (SameText(name, TagName(Tag::Warn))) return TagAttribute(Tag::Warn);
            if (SameText(name, TagName(Tag::Error))) return TagAttribute(Tag::Error);
            if (SameText(name, TagName(Tag::Fatal))) return TagAttribute(Tag::Fatal);
            return kDefaultAttribute;
        }

        // 自动换行后的续行前缀：内核自绘屏幕目标行满换行后先写它，再继续正文
        // 只写屏幕，串口与调试口不补前缀，日志仍是单行
        constexpr const char kContinuePrefix[] = "[ ... ] ";

        // 编译期核对续行前缀与 TagName(Tag::Continue) 的 "[ 名字 ] " 写法一致，改标签名不会漏改前缀
        constexpr bool ContinuePrefixMatches() {
            const char* name = TagName(Tag::Continue);
            uint32_t index = 0;
            if (kContinuePrefix[index++] != '[' || kContinuePrefix[index++] != ' ') return false;
            for (uint32_t i = 0; name[i] != '\0'; ++i) {
                if (kContinuePrefix[index++] != name[i]) return false;
            }
            return kContinuePrefix[index] == ' ' && kContinuePrefix[index + 1] == ']' && kContinuePrefix[index + 2] == ' '
                && kContinuePrefix[index + 3] == '\0';
        }
        static_assert(ContinuePrefixMatches(), "kContinuePrefix 必须与 TagName(Tag::Continue) 的 [ 名字 ] 写法一致");

        // 写入 "[ 标签 ] " 前缀：标签按等级着色，写完立即恢复缺省属性；流式通道忽略属性
        void WriteTag(Tag tag);

        // 写入 "[ 标签 ] 文本\r\n" 一整行；文本为空指针时只写标签
        void WriteLine(Tag tag, const char* text);
    }
}
