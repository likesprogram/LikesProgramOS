/* Sha512.hpp
    SHA-512 摘要算法：供 Ed25519 验签与内核集校验在无运行库环境下使用

    实现按 FIPS 180-4，输出 64 字节原始摘要；Ed25519 的内部摘要就是本算法，见 RFC 8032
    同一份源码给 BaleenCore 与后续 UEFI 侧使用；宿主侧工具另有一份实现，两侧用标准测试向量互验
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    namespace Digest {
        // SHA-512 摘要长度，字节
        constexpr uint32_t kSha512Bytes = 64;

        // SHA-512 流式摘要：Begin 重置、Update 累积、Finish 输出
        class Sha512 {
        public:
            // 重置为初始状态，可重复使用
            void Begin();
            // 累积一段字节，data 为空或 bytes 为 0 时不做任何事
            void Update(const uint8_t* data, uint32_t bytes);
            // 追加填充与 128 位位长度并输出 64 字节摘要；此后须重新 Begin 才能再用
            void Finish(uint8_t digest[kSha512Bytes]);
        private:
            uint64_t m_state[8];         // 八个链接变量
            uint64_t m_bytes = 0;        // 已累积的输入字节数，填入 128 位位长度的高位用
            uint8_t m_block[128];        // 未凑满一块的输入
            uint32_t m_blockBytes = 0;   // 输入块里已有的字节数
        };
    }
}
