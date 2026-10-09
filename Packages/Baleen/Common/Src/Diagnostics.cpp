/* Diagnostics.cpp
    Diagnostics.hpp 的实现：带编号诊断行的写法
*/

#include <Diagnostics.hpp>

#include <Print.hpp>

namespace Baleen {
    void WriteDiagnostic(PrintTargets::Tag tag, const Diagnostic& diagnostic, const char* detail) {
        PrintTargets::WriteTag(tag);
        Print::Write(diagnostic.code != nullptr ? diagnostic.code : "E-UNKNOWN");
        if (diagnostic.text != nullptr && diagnostic.text[0] != '\0') {
            Print::Write(" ");
            Print::Write(diagnostic.text);
        }
        if (detail != nullptr && detail[0] != '\0') {
            Print::Write(" (");
            Print::Write(detail);
            Print::Write(")");
        }
        Print::Write("\r\n");
    }
}
