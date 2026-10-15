/* Sha512.cpp
    SHA-512 摘要算法实现：128 字节一块的消息调度与 80 轮压缩

    常量与轮函数按 FIPS 180-4 第 4.2.3 与 6.4.2 节，未做查表或汇编优化
*/

#include <Sha512.hpp>

namespace Baleen {
    namespace Digest {
        namespace {
            // 初始链接变量：前八个素数平方根小数部分的前 64 位
            constexpr uint64_t kInitial[8] = {
                0x6A09E667F3BCC908ull, 0xBB67AE8584CAA73Bull, 0x3C6EF372FE94F82Bull, 0xA54FF53A5F1D36F1ull,
                0x510E527FADE682D1ull, 0x9B05688C2B3E6C1Full, 0x1F83D9ABFB41BD6Bull, 0x5BE0CD19137E2179ull,
            };

            // 轮常量：前 80 个素数立方根小数部分的前 64 位
            constexpr uint64_t kRound[80] = {
                0x428A2F98D728AE22ull, 0x7137449123EF65CDull, 0xB5C0FBCFEC4D3B2Full, 0xE9B5DBA58189DBBCull,
                0x3956C25BF348B538ull, 0x59F111F1B605D019ull, 0x923F82A4AF194F9Bull, 0xAB1C5ED5DA6D8118ull,
                0xD807AA98A3030242ull, 0x12835B0145706FBEull, 0x243185BE4EE4B28Cull, 0x550C7DC3D5FFB4E2ull,
                0x72BE5D74F27B896Full, 0x80DEB1FE3B1696B1ull, 0x9BDC06A725C71235ull, 0xC19BF174CF692694ull,
                0xE49B69C19EF14AD2ull, 0xEFBE4786384F25E3ull, 0x0FC19DC68B8CD5B5ull, 0x240CA1CC77AC9C65ull,
                0x2DE92C6F592B0275ull, 0x4A7484AA6EA6E483ull, 0x5CB0A9DCBD41FBD4ull, 0x76F988DA831153B5ull,
                0x983E5152EE66DFABull, 0xA831C66D2DB43210ull, 0xB00327C898FB213Full, 0xBF597FC7BEEF0EE4ull,
                0xC6E00BF33DA88FC2ull, 0xD5A79147930AA725ull, 0x06CA6351E003826Full, 0x142929670A0E6E70ull,
                0x27B70A8546D22FFCull, 0x2E1B21385C26C926ull, 0x4D2C6DFC5AC42AEDull, 0x53380D139D95B3DFull,
                0x650A73548BAF63DEull, 0x766A0ABB3C77B2A8ull, 0x81C2C92E47EDAEE6ull, 0x92722C851482353Bull,
                0xA2BFE8A14CF10364ull, 0xA81A664BBC423001ull, 0xC24B8B70D0F89791ull, 0xC76C51A30654BE30ull,
                0xD192E819D6EF5218ull, 0xD69906245565A910ull, 0xF40E35855771202Aull, 0x106AA07032BBD1B8ull,
                0x19A4C116B8D2D0C8ull, 0x1E376C085141AB53ull, 0x2748774CDF8EEB99ull, 0x34B0BCB5E19B48A8ull,
                0x391C0CB3C5C95A63ull, 0x4ED8AA4AE3418ACBull, 0x5B9CCA4F7763E373ull, 0x682E6FF3D6B2B8A3ull,
                0x748F82EE5DEFB2FCull, 0x78A5636F43172F60ull, 0x84C87814A1F0AB72ull, 0x8CC702081A6439ECull,
                0x90BEFFFA23631E28ull, 0xA4506CEBDE82BDE9ull, 0xBEF9A3F7B2C67915ull, 0xC67178F2E372532Bull,
                0xCA273ECEEA26619Cull, 0xD186B8C721C0C207ull, 0xEADA7DD6CDE0EB1Eull, 0xF57D4F7FEE6ED178ull,
                0x06F067AA72176FBAull, 0x0A637DC5A2C898A6ull, 0x113F9804BEF90DAEull, 0x1B710B35131C471Bull,
                0x28DB77F523047D84ull, 0x32CAAB7B40C72493ull, 0x3C9EBE0A15C9BEBCull, 0x431D67C49C100D4Cull,
                0x4CC5D4BECB3E42B6ull, 0x597F299CFC657E2Aull, 0x5FCB6FAB3AD6FAECull, 0x6C44198C4A475817ull,
            };

            // 循环右移；bits 取 1..63
            inline uint64_t RotateRight(uint64_t value, uint32_t bits) {
                return (value >> bits) | (value << (64 - bits));
            }

            // 按大端序写入 64 位
            void Put64Big(uint8_t* p, uint64_t value) {
                p[0] = static_cast<uint8_t>(value >> 56);
                p[1] = static_cast<uint8_t>(value >> 48);
                p[2] = static_cast<uint8_t>(value >> 40);
                p[3] = static_cast<uint8_t>(value >> 32);
                p[4] = static_cast<uint8_t>(value >> 24);
                p[5] = static_cast<uint8_t>(value >> 16);
                p[6] = static_cast<uint8_t>(value >> 8);
                p[7] = static_cast<uint8_t>(value);
            }

            // 处理一个 128 字节输入块，就地更新八个链接变量
            void ProcessBlock(uint64_t* state, const uint8_t* block) {
                // 消息调度：前 16 个字直接取输入，其余按递推式展开
                uint64_t w[80];
                for (uint32_t i = 0; i < 16; ++i) {
                    uint64_t word = 0;
                    for (uint32_t j = 0; j < 8; ++j) word = (word << 8) | block[8 * i + j];
                    w[i] = word;
                }
                for (uint32_t i = 16; i < 80; ++i) {
                    const uint64_t s0 = RotateRight(w[i - 15], 1) ^ RotateRight(w[i - 15], 8) ^ (w[i - 15] >> 7);
                    const uint64_t s1 = RotateRight(w[i - 2], 19) ^ RotateRight(w[i - 2], 61) ^ (w[i - 2] >> 6);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }

                uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
                uint64_t e = state[4], f = state[5], g = state[6], h = state[7];
                for (uint32_t i = 0; i < 80; ++i) {
                    const uint64_t s1 = RotateRight(e, 14) ^ RotateRight(e, 18) ^ RotateRight(e, 41);
                    const uint64_t choose = (e & f) ^ (~e & g);
                    const uint64_t t1 = h + s1 + choose + kRound[i] + w[i];
                    const uint64_t s0 = RotateRight(a, 28) ^ RotateRight(a, 34) ^ RotateRight(a, 39);
                    const uint64_t majority = (a & b) ^ (a & c) ^ (b & c);
                    const uint64_t t2 = s0 + majority;
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

        void Sha512::Begin() {
            for (uint32_t i = 0; i < 8; ++i) m_state[i] = kInitial[i];
            m_bytes = 0;
            m_blockBytes = 0;
        }

        void Sha512::Update(const uint8_t* data, uint32_t bytes) {
            if (data == nullptr || bytes == 0) return;
            m_bytes += bytes;
            while (bytes > 0) {
                const uint32_t space = sizeof(m_block) - m_blockBytes;
                const uint32_t take = bytes < space ? bytes : space;
                for (uint32_t i = 0; i < take; ++i) m_block[m_blockBytes + i] = data[i];
                m_blockBytes += take;
                data += take;
                bytes -= take;
                if (m_blockBytes == sizeof(m_block)) {
                    ProcessBlock(m_state, m_block);
                    m_blockBytes = 0;
                }
            }
        }

        void Sha512::Finish(uint8_t digest[kSha512Bytes]) {
            // 位长度 128 位：高 64 位是字节数右移 61，低 64 位是字节数左移 3
            const uint64_t bytes = m_bytes;
            uint8_t length[16];
            Put64Big(length, bytes >> 61);
            Put64Big(length + 8, bytes << 3);
            // 追加 0x80 后补零到 112 字节为止，再写位长度；m_bytes 的累加已无意义
            const uint8_t marker = 0x80;
            Update(&marker, 1);
            const uint8_t zeros[128] = {};
            while (m_blockBytes != 112) {
                const uint32_t space = 112 > m_blockBytes ? 112 - m_blockBytes : 112 + sizeof(m_block) - m_blockBytes;
                Update(zeros, space < sizeof(zeros) ? space : static_cast<uint32_t>(sizeof(zeros)));
            }
            Update(length, sizeof(length));
            for (uint32_t i = 0; i < 8; ++i) Put64Big(digest + 8 * i, m_state[i]);
        }
    }
}
