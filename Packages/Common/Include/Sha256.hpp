/* Sha256.hpp
    SHA-256 摘要算法的目标侧共享实现：Baleen 两阶段与内核共用同一份源码

    实现按 FIPS 180-4，输出 32 字节原始摘要；纯整数运算，不依赖运行库、异常或平台设施
    算法实现在 Packages/Common/Src/Sha256.cpp 一处，本头给出两个视图：

      Common::Digest::Sha256  64 位字节计数，内核与后续目标代码使用；按 FIPS 的 64 位
                              位长度字段，可处理的消息上限是 2^61 字节（2 EiB），
                              超过该上限 FIPS 未定义，本实现不检查也不回绕
      Baleen::Digest::Sha256  Baleen 阶段的兼容视图：32 位计数，类布局与迁移前的旧头逐字一致，
                              供 Baleen/Common 的 ImageHeader 与 Stub 路径使用；旧副本已从
                              Packages/Baleen/Common 删除，本头是 Baleen 侧的唯一来源；
                              Baleen 镜像远小于 512MiB 回绕上限，行为与迁移前一致，
                              新代码应使用 Common::Digest::Sha256

    宿主侧 Tools/Common/Src/Sha256.cpp 是独立实现，两侧用 FIPS 向量与真实镜像互验，
    不共享源码：宿主工具不依赖目标包
*/

#pragma once
#include <stdint.h>

namespace Common {
    namespace Digest {
        // SHA-256 摘要长度，字节
        constexpr uint32_t kSha256Bytes = 32;

        // 64 位字节计数的流式摘要：内核与后续目标代码使用
        // 字节计数器 64 位，位长度字段按 FIPS 为 64 位，可表示的消息上限 2^61 字节
        class Sha256 {
        public:
            // 重置为初始状态，可重复使用
            void Begin();
            // 累积一段字节，data 为空或 bytes 为 0 时不做任何事
            void Update(const uint8_t* data, uint32_t bytes);
            // 追加填充与位长度并输出 32 字节摘要；此后须重新 Begin 才能再用
            void Finish(uint8_t digest[kSha256Bytes]);
        private:
            uint32_t m_state[8];         // 八个链接变量
            uint64_t m_bytes = 0;        // 已累积的输入字节数，64 位
            uint8_t m_block[64];         // 未凑满一块的输入
            uint32_t m_blockBytes = 0;   // 输入块里已有的字节数
        };
    }
}

namespace Baleen {
    namespace Digest {
        // SHA-256 摘要长度，字节
        constexpr uint32_t kSha256Bytes = 32;

        // Baleen 阶段兼容视图：布局与迁移前的 Baleen 头逐字一致
        // 计数器只有 32 位，输入达到 512MiB 时位数会回绕；引导阶段的镜像远小于该上限
        class Sha256 {
        public:
            // 重置为初始状态，可重复使用
            void Begin();
            // 累积一段字节，data 为空或 bytes 为 0 时不做任何事
            void Update(const uint8_t* data, uint32_t bytes);
            // 追加填充与位长度并输出 32 字节摘要；此后须重新 Begin 才能再用
            void Finish(uint8_t digest[kSha256Bytes]);
        private:
            uint32_t m_state[8];         // 八个链接变量
            uint32_t m_bytes = 0;        // 已累积的输入字节数，32 位
            uint8_t m_block[64];         // 未凑满一块的输入
            uint32_t m_blockBytes = 0;   // 输入块里已有的字节数
        };
    }
}

// 兼容视图的布局必须与迁移前的 Baleen 头一致：Baleen/Common 的未迁移对象按旧布局构造
static_assert(sizeof(Baleen::Digest::Sha256) == 104, "Baleen 兼容视图的布局必须与迁移前的 Baleen 头一致");
