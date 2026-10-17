/* EfiTextTarget.hpp
    UEFI 侧控制台目标：把 Print 前端的文本经简单文本输出协议写到固件控制台

    固件控制台收 UTF-16，前端给的是窄字符：本目标按字节逐个扩展到 UTF-16 再下发，
    不做其它转义或翻译——换行由调用方按 "\r\n" 写好，与其它阶段一致
    文本超过内部缓冲时按缓冲容量分段下发，长路径不会被截断
    显示属性不实现：固件控制台没有 VGA 那样的属性字节，标签的等级靠文本自己表达
*/

#pragma once
#include <stdint.h>

#include <EfiTypes.hpp>
#include <Print.hpp>

namespace Baleen {
    namespace Uefi {
        // UEFI 文本控制台目标
        class EfiTextTarget final : public Print::OutTarget {
        public:
            // 绑定固件控制台协议；传空指针表示不可用，后续写入被丢弃
            void Bind(SimpleTextOutput* console);
            // 是否已绑定可用的控制台
            bool Ready() const override;
            // 写入以 '\0' 结尾的窄字符文本；未绑定时丢弃
            void Write(const char* text) override;

        private:
            // 一次下发的 UTF-16 缓冲：超长文本按它分段
            static constexpr uint32_t kLineChars = 256;
            // 固件控制台协议（不拥有，由固件保证在本次运行期间有效）
            SimpleTextOutput* m_console = nullptr;
            // 窄字符到 UTF-16 的转换缓冲
            char16_t m_line[kLineChars] = {};
        };
    }
}
