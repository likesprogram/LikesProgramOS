/* Tags.cpp
    内核诊断标签的实现：纯文本的 "[ 标签 ] " 前缀与整行写入

    与引导期的差别只有一处：内核侧没有支持显示属性的目标，前缀不着色
*/

#include <Tags.hpp>

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
}
