/* SelfCheck.hpp
    Stub 自身完整性自检：头字段校验、长度与链接布局一致性、静态内存与保护模式栈的位置、镜像摘要比对

    自检针对刚装入、尚未改写的文件字节，摘要计算把 Digest 字段视作 32 个零
    本文件只检查并回报带编号的原因，不打印也不停机，诊断由 _Stub_Main 统一输出
*/

#pragma once
#include <stdint.h>

#include <Diagnostics.hpp>

namespace Baleen {
    namespace Stub {
        // 自检通过后带出的镜像事实，供诊断行使用
        struct SelfInfo {
            uint32_t imageBytes;    // 头声明的文件字节数
            uint32_t memoryBytes;   // 头声明的静态内存跨度，含 BSS
            uint32_t buildId;       // BuildId 的前 4 字节，仅用于诊断显示
        };

        // 校验自身头字段与镜像摘要：成功返回空指针，失败返回带编号的原因
        Diagnostic CheckSelf(SelfInfo& info);
    }
}
