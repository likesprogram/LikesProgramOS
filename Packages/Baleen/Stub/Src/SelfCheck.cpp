/* SelfCheck.cpp
    Stub 自身完整性自检：公共镜像头的字段校验，再补链接布局事实与整幅摘要

    头字段、入口前缀与摘要的公共校验在 Baleen/Common 的 ImageHeader.cpp；本文件补 Stub 的环境事实：
    头声明的长度必须与链接脚本给出的镜像、未初始化区边界一致，静态内存必须留在低 64KiB 窗口内
    长度与链接布局核对之前不取摘要；校验失败只回报原因，打印与停机由 _Stub_Main 负责
*/

#include <stddef.h>
#include <stdint.h>

#include <ImageHeader.hpp>
#include <SelfCheck.hpp>

// 链接脚本给出的镜像与未初始化区边界；镜像起点即 IPL 装入点，也是这里的物理地址
extern "C" const uint8_t __image_start[];
extern "C" const uint8_t __image_end[];
extern "C" const uint8_t __bss_end[];

namespace Baleen {
    namespace Stub {
        namespace {
            // 低 64KiB 窗口上端，不含；取值与 Contract.inc 的 LOAD_CEIL 一致
            constexpr uintptr_t kWindowEnd = 0x10000;
        }

        const char* CheckSelf(SelfInfo& info) {
            const uint8_t* const image = __image_start;
            ImageFacts facts;
            if (const char* reason = CheckImageHeader(image, kStubImageMagic, facts)) return reason;

            // 长度必须与链接布局一致：头字段是打包器按链接产物填的，对不上说明镜像不是这一份
            const uint32_t linkedImageBytes = static_cast<uint32_t>(__image_end - __image_start);
            const uint32_t linkedMemoryBytes = static_cast<uint32_t>(__bss_end - __image_start);
            if (facts.imageBytes != linkedImageBytes) return "The stub image size doesn't match the linked layout";
            if (facts.memoryBytes != linkedMemoryBytes) return "The stub memory size doesn't match the linked layout";
            if (reinterpret_cast<uintptr_t>(image) + facts.memoryBytes > kWindowEnd) return "The stub static memory leaves the low 64KiB window";

            // 长度已经与链接布局核对过，这时摘要才有意义
            if (const char* reason = CheckImageDigest(image, facts.imageBytes)) return reason;

            info.imageBytes = facts.imageBytes;
            info.memoryBytes = facts.memoryBytes;
            info.buildId = facts.buildId;
            return nullptr;
        }
    }
}
