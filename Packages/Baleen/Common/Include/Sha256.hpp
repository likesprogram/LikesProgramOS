/* Sha256.hpp
    SHA-256 摘要算法：供引导阶段在无运行库环境下计算自身完整性摘要

    实现按 FIPS 180-4，输出 32 字节原始摘要；同一份源码同时给 Stub 与后续 Core 使用
    宿主侧的打包与校验工具另有一份实现，两侧用标准测试向量与真实镜像互验
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    namespace Digest {
        // SHA-256 摘要长度，字节
        constexpr uint32_t kSha256Bytes = 32;

        // SHA-256 流式摘要：Begin 重置、Update 累积、Finish 输出
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
            uint32_t m_state[8];      // 八个链接变量
            uint32_t m_bytes = 0;     // 已累积的输入字节数
            uint8_t m_block[64];      // 未凑满一块的输入
            uint32_t m_blockBytes = 0;   // 输入块里已有的字节数
        };
    }
}
