/* SelfCheck.cpp
    Stub 自身完整性自检：公共镜像头的字段校验，再补链接布局事实与整幅摘要

    头字段、入口前缀与摘要的公共校验在 Baleen/Common 的 ImageHeader.cpp；本文件补 Stub 的环境事实：
    头声明的长度必须与链接脚本给出的镜像、未初始化区边界一致，静态内存必须留在低 64KiB 窗口内，
    保护模式栈必须整段落在静态内存里
    长度与链接布局核对之前不取摘要；校验失败只回报带编号的原因，打印与停机由 _Stub_Main 负责
*/

#include <stddef.h>
#include <stdint.h>

#include <Diagnostics.hpp>
#include <ImageHeader.hpp>
#include <SelfCheck.hpp>

// 链接脚本给出的镜像与未初始化区边界；镜像起点即 IPL 装入点，也是这里的物理地址
extern "C" const uint8_t __image_start[];
extern "C" const uint8_t __image_end[];
extern "C" const uint8_t __bss_end[];
// 保护模式栈的边界，由 Stub.asm 定义、链接脚本排到未初始化区最后
extern "C" const uint8_t _Stub_Stack_Bottom[];
extern "C" const uint8_t _Stub_Stack_Top[];

namespace Baleen {
    namespace Stub {
        namespace {
            // 低 64KiB 窗口上端，不含；取值与 Contract.inc 的 LOAD_CEIL 一致
            constexpr uintptr_t kWindowEnd = 0x10000;

            // —— 本阶段失败原因的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailImageBytes{"E-STUB-10", "The stub image size doesn't match the linked layout"};
            constexpr Diagnostic kFailMemoryBytes{"E-STUB-11", "The stub memory size doesn't match the linked layout"};
            constexpr Diagnostic kFailWindow{"E-STUB-12", "The stub static memory leaves the low 64KiB window"};
            constexpr Diagnostic kFailStackOutside{"E-STUB-13", "The stub stack isn't inside the static memory"};
            constexpr Diagnostic kFailStackLength{"E-STUB-14", "The stub stack length is zero"};
        }

        Diagnostic CheckSelf(SelfInfo& info) {
            const uint8_t* const image = __image_start;
            ImageFacts facts;
            if (const Diagnostic reason = CheckImageHeader(image, kStubImageMagic, facts, ImageDiagStage::Stub); reason.code != nullptr) return reason;

            // 长度必须与链接布局一致：头字段是打包器按链接产物填的，对不上说明镜像不是这一份
            const uint32_t linkedImageBytes = static_cast<uint32_t>(__image_end - __image_start);
            const uint32_t linkedMemoryBytes = static_cast<uint32_t>(__bss_end - __image_start);
            if (facts.imageBytes != linkedImageBytes) return kFailImageBytes;
            if (facts.memoryBytes != linkedMemoryBytes) return kFailMemoryBytes;
            if (reinterpret_cast<uintptr_t>(image) + facts.memoryBytes > kWindowEnd) return kFailWindow;
            // 保护模式栈必须在头声明的静态内存里：位置由链接脚本决定，栈本身要被清零、也要受窗口约束
            const uintptr_t stackBottom = reinterpret_cast<uintptr_t>(_Stub_Stack_Bottom);
            const uintptr_t stackTop = reinterpret_cast<uintptr_t>(_Stub_Stack_Top);
            if (stackBottom < reinterpret_cast<uintptr_t>(image) || stackTop > reinterpret_cast<uintptr_t>(image) + facts.memoryBytes)
                return kFailStackOutside;
            if (stackTop <= stackBottom) return kFailStackLength;

            // 长度已经与链接布局核对过，这时摘要才有意义
            if (const Diagnostic reason = CheckImageDigest(image, facts.imageBytes, ImageDiagStage::Stub); reason.code != nullptr) return reason;

            info.imageBytes = facts.imageBytes;
            info.memoryBytes = facts.memoryBytes;
            info.buildId = facts.buildId;
            return Diagnostic{};
        }
    }
}
