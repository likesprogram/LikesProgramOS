/* Sha256.cpp
    SHA-256 摘要算法的宿主侧实现：消息调度与 64 轮压缩

    常量与轮函数按 FIPS 180-4 第 4.2.2 与 6.2.2 节，与目标侧实现同构
*/

#include <Sha256.h>

#include <algorithm>

namespace hostbuild {
    namespace {
        // 初始链接变量：前八个素数平方根小数部分的前 32 位
        constexpr uint32_t kInitial[8] = {
            0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
            0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
        };

        // 轮常量：前 64 个素数立方根小数部分的前 32 位
        constexpr uint32_t kRound[64] = {
            0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
            0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
            0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
            0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
            0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
            0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
            0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
            0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
        };

        // 循环右移；bits 取 1..31
        uint32_t RotateRight(uint32_t value, uint32_t bits) {
            return (value >> bits) | (value << (32 - bits));
        }

        // 按大端序写入 32 位
        void Put32Big(uint8_t* p, uint32_t value) {
            p[0] = static_cast<uint8_t>(value >> 24);
            p[1] = static_cast<uint8_t>(value >> 16);
            p[2] = static_cast<uint8_t>(value >> 8);
            p[3] = static_cast<uint8_t>(value);
        }

        // 处理一个 64 字节输入块，就地更新八个链接变量
        void ProcessBlock(uint32_t* state, const uint8_t* block) {
            // 消息调度：前 16 个字直接取输入，其余按递推式展开
            uint32_t w[64];
            for (uint32_t i = 0; i < 16; ++i) {
                w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) | (static_cast<uint32_t>(block[4 * i + 1]) << 16)
                    | (static_cast<uint32_t>(block[4 * i + 2]) << 8) | static_cast<uint32_t>(block[4 * i + 3]);
            }
            for (uint32_t i = 16; i < 64; ++i) {
                const uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
            for (uint32_t i = 0; i < 64; ++i) {
                const uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                const uint32_t choose = (e & f) ^ (~e & g);
                const uint32_t t1 = h + s1 + choose + kRound[i] + w[i];
                const uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
                const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = s0 + majority;
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
            state[5] += f;
            state[6] += g;
            state[7] += h;
        }
    }

    void Sha256::Begin() {
        std::copy(std::begin(kInitial), std::end(kInitial), m_state.begin());
        m_bytes = 0;
        m_blockBytes = 0;
    }

    void Sha256::Update(std::span<const uint8_t> data) {
        m_bytes += data.size();
        while (!data.empty()) {
            const std::size_t space = m_block.size() - m_blockBytes;
            const std::size_t take = std::min(space, data.size());
            std::copy_n(data.begin(), take, m_block.begin() + static_cast<std::ptrdiff_t>(m_blockBytes));
            m_blockBytes += take;
            data = data.subspan(take);
            if (m_blockBytes == m_block.size()) {
                ProcessBlock(m_state.data(), m_block.data());
                m_blockBytes = 0;
            }
        }
    }

    std::array<uint8_t, kSha256Bytes> Sha256::Finish() {
        // 位长度共 64 位，按大端写入
        uint8_t length[8];
        Put32Big(length, static_cast<uint32_t>(m_bytes >> 29));
        Put32Big(length + 4, static_cast<uint32_t>(m_bytes << 3));
        // 追加 0x80 后补零到 56 字节为止，再写位长度
        const uint8_t marker = 0x80;
        Update(std::span<const uint8_t>(&marker, 1));
        const uint8_t zeros[64] = {};
        while (m_blockBytes != 56) {
            const std::size_t space = 56 > m_blockBytes ? 56 - m_blockBytes : 56 + m_block.size() - m_blockBytes;
            Update(std::span<const uint8_t>(zeros, std::min(space, sizeof(zeros))));
        }
        Update(std::span<const uint8_t>(length, sizeof(length)));
        std::array<uint8_t, kSha256Bytes> digest{};
        for (std::size_t i = 0; i < m_state.size(); ++i) Put32Big(digest.data() + 4 * i, m_state[i]);
        return digest;
    }
}
