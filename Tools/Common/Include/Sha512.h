/* Sha512.h
    SHA-512 摘要算法的宿主侧实现：供 Ed25519 签名与内核集清单校验计算摘要

    与目标侧 Packages/Baleen/Common/Src/Sha512.cpp 是同一算法的两份实现，
    用 FIPS 180-4 测试向量互验；不共享源码，宿主工具不依赖目标包
*/

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hostbuild {
    // SHA-512 摘要长度，字节
    constexpr std::size_t kSha512Bytes = 64;

    // 流式摘要：Begin 重置、Update 累积、Finish 输出，可重复使用
    class Sha512 {
    public:
        // 重置为初始状态
        void Begin();
        // 累积一段字节
        void Update(std::span<const uint8_t> data);
        // 追加填充与 128 位位长度并输出 64 字节摘要
        std::array<uint8_t, kSha512Bytes> Finish();
    private:
        std::array<uint64_t, 8> m_state{};      // 八个链接变量
        uint64_t m_bytes = 0;                   // 已累积的输入字节数
        std::array<uint8_t, 128> m_block{};     // 未凑满一块的输入
        std::size_t m_blockBytes = 0;           // 输入块里已有的字节数
    };
}
