/* EfiTextTarget.hpp
    UEFI 侧控制台目标：把 Print 前端的文本经简单文本输出协议写到固件控制台

    固件控制台收 UTF-16，前端给的是窄字符：本目标按字节逐个扩展到 UTF-16 再下发，
    不做其它转义或翻译——换行由调用方按 "\r\n" 写好，与其它阶段一致
    文本超过内部缓冲时按缓冲容量分段下发，长路径不会被截断
    显示属性经固件的 SetAttribute 设置，属性编码与 VGA 一致（前景低 4 位、背景高 4 位），
    标签着色方式因此与 BIOS 路径相同；固件不接受属性时写入照常进行
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
            // 设置后续写入的显示属性；未绑定或固件没有该能力时忽略
            void SetAttribute(uint8_t attribute) override;

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
