/* Sha256.h
    SHA-256 摘要算法的宿主侧实现：供 Stub 镜像的打包填充与组装校验计算摘要

    与目标侧 Packages/Baleen/Common/Src/Sha256.cpp 是同一算法的两份实现，
    用 FIPS 180-4 测试向量与真实镜像的摘要互验；不共享源码，宿主工具不依赖目标包
*/

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hostbuild {
    // SHA-256 摘要长度，字节
    constexpr std::size_t kSha256Bytes = 32;

    // 流式摘要：Begin 重置、Update 累积、Finish 输出，可重复使用
    class Sha256 {
    public:
        // 重置为初始状态
        void Begin();
        // 累积一段字节
        void Update(std::span<const uint8_t> data);
        // 追加填充与位长度并输出 32 字节摘要
        std::array<uint8_t, kSha256Bytes> Finish();
    private:
        std::array<uint32_t, 8> m_state{};      // 八个链接变量
        uint64_t m_bytes = 0;                   // 已累积的输入字节数
        std::array<uint8_t, 64> m_block{};      // 未凑满一块的输入
        std::size_t m_blockBytes = 0;           // 输入块里已有的字节数
    };
}
