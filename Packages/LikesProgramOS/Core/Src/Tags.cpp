/* Tags.cpp
    内核诊断标签与带编号诊断行的实现：纯文本的 "[ 标签 ] " 前缀、整行写入与 E-KRN 行写法

    与引导期的差别只有一处：内核侧没有支持显示属性的目标，前缀不着色
    带编号行按 Baleen 的同一写法组行：标签 + 编号 + 文本 + 可选 detail，见 Diagnostics.hpp
*/

#include <Tags.hpp>

#include <Diagnostics.hpp>

#include <Print.hpp>

namespace Kernel {
    namespace Tags {
        void WriteTag(Tag tag) {
            Print::Write("[ ");
            Print::Write(TagName(tag));
            Print::Write(" ] ");
        }

        void WriteLine(Tag tag, const char* text) {
            WriteTag(tag);
            if (text != nullptr) Print::Write(text);
            Print::Write("\r\n");
        }
    }

    namespace Diagnostics {
        void WriteDiagnosticPrefix(Tags::Tag tag, const Diagnostic& diagnostic) {
            Tags::WriteTag(tag);
            Print::Write(diagnostic.code != nullptr ? diagnostic.code : "E-UNKNOWN");
            if (diagnostic.text != nullptr && diagnostic.text[0] != '\0') {
                Print::Write(" ");
                Print::Write(diagnostic.text);
            }
        }

        void WriteDiagnostic(Tags::Tag tag, const Diagnostic& diagnostic, const char* detail) {
            WriteDiagnosticPrefix(tag, diagnostic);
            if (detail != nullptr && detail[0] != '\0') {
                Print::Write(" (");
                Print::Write(detail);
                Print::Write(")");
            }
            Print::Write("\r\n");
        }
    }
}
