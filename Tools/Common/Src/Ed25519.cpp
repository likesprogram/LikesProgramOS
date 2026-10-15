/* Ed25519.cpp
    Ed25519 签名与验签实现（宿主侧）：域运算、点运算、标量归约与签名流程

    域元素用 16 个 16 位 limb（int64 存储，允许正负与溢出位），运算后由进位链收拢；
    标量归约按逐位模 L 进行；签名与派生按 RFC 8032 第 5.1 节
    与目标侧实现各自独立编写，用同一套 RFC 8032 向量互验
*/

#include <Ed25519.h>

#include <cstring>
#include <stdexcept>

#include <Sha512.h>

namespace hostbuild {
    namespace {
        // 域元素：16 个 16 位 limb，小端；limb 允许超出 16 位，由进位链收拢
        using Field = std::array<int64_t, 16>;
        // 扩展坐标点：X、Y、Z、T，满足 T = XY/Z
        using Point = std::array<Field, 4>;

        // 域常量：曲线参数 d、2d、基点坐标与 sqrt(-1)
        constexpr int64_t kCurveD[16] = {
            0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
            0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
        };
        constexpr int64_t kCurveD2[16] = {
            0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
            0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
        };
        constexpr int64_t kBaseX[16] = {
            0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
            0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
        };
        constexpr int64_t kBaseY[16] = {
            0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
            0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
        };
        constexpr int64_t kSqrtM1[16] = {
            0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
            0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
        };
        // 阶 L 的小端 32 字节
        constexpr uint8_t kOrderL[32] = {
            0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
        };

        // 域素数 p = 2^255 - 19 的小端 32 字节：编码规范性的比较基准
        constexpr uint8_t kPrimeEncoding[32] = {
            0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
        };

        // 加法：out = a + b，不归约
        void FieldAdd(Field& out, const Field& a, const Field& b) {
            for (unsigned i = 0; i < 16; ++i) out[i] = a[i] + b[i];
        }

        // 减法：out = a - b，结果可含负 limb
        void FieldSub(Field& out, const Field& a, const Field& b) {
            for (unsigned i = 0; i < 16; ++i) out[i] = a[i] - b[i];
        }

        // 进位链：收拢到 16 位范围；最高 limb 的进位按 2^256 ≡ 38 折回最低位
        void FieldCarry(Field& value) {
            for (unsigned i = 0; i < 16; ++i) {
                value[i] += 1 << 16;
                const int64_t carry = value[i] >> 16;
                if (i < 15) value[i + 1] += carry - 1;
                else value[0] += 38 * (carry - 1);
                value[i] -= carry << 16;
            }
        }

        // 乘法：out = a * b（模 p）
        void FieldMul(Field& out, const Field& a, const Field& b) {
            int64_t product[31] = {};
            for (unsigned i = 0; i < 16; ++i)
                for (unsigned j = 0; j < 16; ++j) product[i + j] += a[i] * b[j];
            for (unsigned i = 0; i < 15; ++i) product[i] += 38 * product[i + 16];
            for (unsigned i = 0; i < 16; ++i) out[i] = product[i];
            FieldCarry(out);
            FieldCarry(out);
        }

        // 平方：out = a^2（模 p）
        void FieldSquare(Field& out, const Field& a) {
            FieldMul(out, a, a);
        }

        // 常数时间条件交换：swap 非零时交换 p 与 q
        void FieldSwap(Field& p, Field& q, int64_t swap) {
            const int64_t mask = ~(swap - 1);
            for (unsigned i = 0; i < 16; ++i) {
                const int64_t move = mask & (p[i] ^ q[i]);
                p[i] ^= move;
                q[i] ^= move;
            }
        }

        // 规范编码：三轮进位后两次条件减去 p，输出 32 字节小端
        void FieldPack(uint8_t out[32], const Field& value) {
            Field reduced = value;
            Field candidate{};
            FieldCarry(reduced);
            FieldCarry(reduced);
            FieldCarry(reduced);
            for (unsigned pass = 0; pass < 2; ++pass) {
                candidate[0] = reduced[0] - 0xffed;
                for (unsigned i = 1; i < 15; ++i) {
                    candidate[i] = reduced[i] - 0xffff - ((candidate[i - 1] >> 16) & 1);
                    candidate[i - 1] &= 0xffff;
                }
                candidate[15] = reduced[15] - 0x7fff - ((candidate[14] >> 16) & 1);
                const int64_t borrow = (candidate[15] >> 16) & 1;
                candidate[14] &= 0xffff;
                FieldSwap(reduced, candidate, 1 - borrow);
            }
            for (unsigned i = 0; i < 16; ++i) {
                out[2 * i] = static_cast<uint8_t>(reduced[i] & 0xff);
                out[2 * i + 1] = static_cast<uint8_t>(reduced[i] >> 8);
            }
        }

        // 编码去掉符号位后的 255 位值是否小于 p：非规范编码按解码失败处理
        bool EncodingCanonical(const uint8_t encoded[32]) {
            uint8_t copy[32];
            for (unsigned i = 0; i < 32; ++i) copy[i] = encoded[i];
            copy[31] &= 0x7f;
            for (int i = 31; i >= 0; --i) {
                if (copy[i] != kPrimeEncoding[i]) return copy[i] < kPrimeEncoding[i];
            }
            return false;   // 恰好等于 p：非规范
        }

        // 解包：取 32 字节小端的低 255 位；非规范编码（y >= p）返回 false
        // RFC 8032 第 5.1.3 节要求 y >= p 的编码解码失败，这里不静默归约
        bool FieldUnpack(Field& out, const uint8_t encoded[32]) {
            if (!EncodingCanonical(encoded)) return false;
            for (unsigned i = 0; i < 16; ++i) out[i] = encoded[2 * i] + (static_cast<int64_t>(encoded[2 * i + 1]) << 8);
            out[15] &= 0x7fff;
            return true;
        }

        // 域元素是否为零：按规范编码判断
        bool FieldIsZero(const Field& a) {
            uint8_t encoded[32];
            FieldPack(encoded, a);
            uint8_t any = 0;
            for (unsigned i = 0; i < 32; ++i) any |= encoded[i];
            return any == 0;
        }

        // 两个域元素按规范编码是否不等
        bool FieldNotEqual(const Field& a, const Field& b) {
            uint8_t left[32], right[32];
            FieldPack(left, a);
            FieldPack(right, b);
            uint8_t diff = 0;
            for (unsigned i = 0; i < 32; ++i) diff |= left[i] ^ right[i];
            return diff != 0;
        }

        // 域元素规范编码的最低位
        uint8_t FieldParity(const Field& a) {
            uint8_t encoded[32];
            FieldPack(encoded, a);
            return encoded[0] & 1;
        }

        // 求逆：out = z^(p-2)
        void FieldInvert(Field& out, const Field& z) {
            Field power = z;
            for (int i = 253; i >= 0; --i) {
                FieldSquare(power, power);
                if (i != 2 && i != 4) FieldMul(power, power, z);
            }
            out = power;
        }

        // out = z^((p-5)/8)
        void FieldPow2523(Field& out, const Field& z) {
            Field power = z;
            for (int i = 250; i >= 0; --i) {
                FieldSquare(power, power);
                if (i != 1) FieldMul(power, power, z);
            }
            out = power;
        }

        // 把常量表转成域元素
        Field Constant(const int64_t (&value)[16]) {
            Field out{};
            for (unsigned i = 0; i < 16; ++i) out[i] = value[i];
            return out;
        }

        // 点加（扩展坐标）：p = p + q
        void PointAdd(Point& p, const Point& q) {
            Field a, b, c, d, t, e, f, g, h;
            FieldSub(a, p[1], p[0]);
            FieldSub(t, q[1], q[0]);
            FieldMul(a, a, t);
            FieldAdd(b, p[0], p[1]);
            FieldAdd(t, q[0], q[1]);
            FieldMul(b, b, t);
            FieldMul(c, p[3], q[3]);
            FieldMul(c, c, Constant(kCurveD2));
            FieldMul(d, p[2], q[2]);
            FieldAdd(d, d, d);
            FieldSub(e, b, a);
            FieldSub(f, d, c);
            FieldAdd(g, d, c);
            FieldAdd(h, b, a);

            FieldMul(p[0], e, f);
            FieldMul(p[1], h, g);
            FieldMul(p[2], g, f);
            FieldMul(p[3], e, h);
        }

        // 常数时间条件交换两个点
        void PointSwap(Point& p, Point& q, int64_t swap) {
            for (unsigned i = 0; i < 4; ++i) FieldSwap(p[i], q[i], swap);
        }

        // 标量乘：p = scalar * q；base 会被就地修改，传入的 q 保持不变
        void PointScalarMul(Point& p, const Point& q, const uint8_t scalar[32]) {
            p[0].fill(0);
            p[1].fill(0);
            p[1][0] = 1;
            p[2].fill(0);
            p[2][0] = 1;
            p[3].fill(0);
            Point base = q;
            for (int bit = 255; bit >= 0; --bit) {
                const int64_t select = (scalar[bit / 8] >> (bit & 7)) & 1;
                PointSwap(p, base, select);
                PointAdd(base, p);
                PointAdd(p, p);
                PointSwap(p, base, select);
            }
        }

        // 标量乘基点：p = scalar * B
        void PointScalarBase(Point& p, const uint8_t scalar[32]) {
            Point base{};
            for (unsigned i = 0; i < 16; ++i) base[0][i] = kBaseX[i];
            for (unsigned i = 0; i < 16; ++i) base[1][i] = kBaseY[i];
            base[2].fill(0);
            base[2][0] = 1;
            FieldMul(base[3], base[0], base[1]);
            PointScalarMul(p, base, scalar);
        }

        // 点编码：归一化后输出 y，最高位放 x 的符号
        void PointPack(uint8_t out[32], const Point& p) {
            Field x, y, zInverse;
            FieldInvert(zInverse, p[2]);
            FieldMul(x, p[0], zInverse);
            FieldMul(y, p[1], zInverse);
            FieldPack(out, y);
            out[31] ^= static_cast<uint8_t>(FieldParity(x) << 7);
        }

        // 解压公钥并取负：out = -A；编码不对应曲线上的点或不是规范编码时失败返回 false
        bool PointUnpackNeg(Point& out, const uint8_t encoded[32]) {
            Field t, check, num, den, den2, den4, den6;
            out[2].fill(0);
            out[2][0] = 1;
            if (!FieldUnpack(out[1], encoded)) return false;
            FieldSquare(num, out[1]);
            FieldMul(den, num, Constant(kCurveD));
            FieldSub(num, num, out[2]);
            FieldAdd(den, out[2], den);

            FieldSquare(den2, den);
            FieldSquare(den4, den2);
            FieldMul(den6, den4, den2);
            FieldMul(t, den6, num);
            FieldMul(t, t, den);

            FieldPow2523(t, t);
            FieldMul(t, t, num);
            FieldMul(t, t, den);
            FieldMul(t, t, den);
            FieldMul(out[0], t, den);

            FieldSquare(check, out[0]);
            FieldMul(check, check, den);
            if (FieldNotEqual(check, num)) FieldMul(out[0], out[0], Constant(kSqrtM1));

            FieldSquare(check, out[0]);
            FieldMul(check, check, den);
            if (FieldNotEqual(check, num)) return false;

            // x = 0 且符号位为 1：RFC 8032 第 5.1.3 节要求解码失败
            if ((encoded[31] >> 7) != 0 && FieldIsZero(out[0])) return false;

            if (FieldParity(out[0]) == static_cast<uint8_t>(encoded[31] >> 7)) {
                Field zero{};
                FieldSub(out[0], zero, out[0]);
            }

            FieldMul(out[3], out[0], out[1]);
            return true;
        }

        // r 是否小于 L；相等不算小于
        bool ScalarLessThanL(const uint8_t r[32]) {
            for (int i = 31; i >= 0; --i) {
                if (r[i] != kOrderL[i]) return r[i] < kOrderL[i];
            }
            return false;
        }

        // r -= L；调用前须保证 r >= L
        void ScalarSubtractL(uint8_t r[32]) {
            int borrow = 0;
            for (unsigned i = 0; i < 32; ++i) {
                const int diff = static_cast<int>(r[i]) - kOrderL[i] - borrow;
                r[i] = static_cast<uint8_t>(diff & 0xff);
                borrow = diff < 0 ? 1 : 0;
            }
        }

        // 逐位模 L 归约：把 bits 位输入从高到低折叠成 32 字节标量
        void ScalarReduce(uint8_t out[32], const uint8_t* data, unsigned bits) {
            uint8_t r[32] = {};
            for (unsigned index = bits; index-- > 0;) {
                uint8_t carry = static_cast<uint8_t>((data[index >> 3] >> (index & 7)) & 1);
                for (unsigned i = 0; i < 32; ++i) {
                    const uint8_t next = static_cast<uint8_t>(r[i] >> 7);
                    r[i] = static_cast<uint8_t>((r[i] << 1) | carry);
                    carry = next;
                }
                if (!ScalarLessThanL(r)) ScalarSubtractL(r);
            }
            std::memcpy(out, r, 32);
        }

        // out = (a * b + c) mod L：16 位 limb 乘法得 512 位，加 c 后逐位归约
        void ScalarMulAddModL(const uint8_t a[32], const uint8_t b[32], const uint8_t c[32], uint8_t out[32]) {
            uint64_t product[32] = {};
            for (unsigned i = 0; i < 16; ++i) {
                const uint64_t ai = a[2 * i] | (static_cast<uint64_t>(a[2 * i + 1]) << 8);
                for (unsigned j = 0; j < 16; ++j) {
                    const uint64_t bj = b[2 * j] | (static_cast<uint64_t>(b[2 * j + 1]) << 8);
                    product[i + j] += ai * bj;
                }
            }
            uint8_t bytes[72] = {};
            uint64_t carry = 0;
            unsigned index = 0;
            for (unsigned i = 0; i < 32; ++i) {
                const uint64_t value = product[i] + carry;
                bytes[index++] = static_cast<uint8_t>(value & 0xff);
                bytes[index++] = static_cast<uint8_t>((value >> 8) & 0xff);
                carry = value >> 16;
            }
            while (carry != 0 && index < sizeof(bytes)) {
                bytes[index++] = static_cast<uint8_t>(carry & 0xff);
                carry >>= 8;
            }
            // 加上 c（32 字节）
            unsigned addCarry = 0;
            for (unsigned i = 0; i < 32; ++i) {
                const unsigned sum = bytes[i] + c[i] + addCarry;
                bytes[i] = static_cast<uint8_t>(sum & 0xff);
                addCarry = sum >> 8;
            }
            for (unsigned i = 32; addCarry != 0 && i < sizeof(bytes); ++i) {
                const unsigned sum = bytes[i] + addCarry;
                bytes[i] = static_cast<uint8_t>(sum & 0xff);
                addCarry = sum >> 8;
            }
            ScalarReduce(out, bytes, static_cast<unsigned>(sizeof(bytes)) * 8);
        }

        // 标量乘基点并编码成 32 字节
        void ScalarBaseEncode(const uint8_t scalar[32], uint8_t out[32]) {
            Point p;
            PointScalarBase(p, scalar);
            PointPack(out, p);
        }
    }

    void Ed25519DeriveKey(std::span<const uint8_t> seed, std::array<uint8_t, kEd25519PublicKeyBytes>& publicKey, std::array<uint8_t, kEd25519PrivateKeyBytes>& privateKey) {
        if (seed.size() != kEd25519SeedBytes) throw std::invalid_argument("Ed25519 种子长度必须是 32 字节");
        Sha512 hash;
        hash.Begin();
        hash.Update(seed);
        std::array<uint8_t, kSha512Bytes> digest = hash.Finish();
        uint8_t scalar[32];
        std::memcpy(scalar, digest.data(), 32);
        // 标量按 RFC 8032 截断：清低 3 位、清最高位、置次高位
        scalar[0] &= 248;
        scalar[31] &= 127;
        scalar[31] |= 64;
        ScalarBaseEncode(scalar, publicKey.data());
        std::memcpy(privateKey.data(), seed.data(), 32);
        std::memcpy(privateKey.data() + 32, publicKey.data(), 32);
    }

    void Ed25519Sign(std::span<const uint8_t> privateKey, std::span<const uint8_t> message, std::array<uint8_t, kEd25519SignatureBytes>& signature) {
        if (privateKey.size() != kEd25519PrivateKeyBytes) throw std::invalid_argument("Ed25519 私钥长度必须是 64 字节");
        const uint8_t* publicKey = privateKey.data() + 32;
        // d = SHA-512(seed)：前 32 字节截断成标量，后 32 字节是前缀
        Sha512 first;
        first.Begin();
        first.Update(privateKey.first(32));
        const std::array<uint8_t, kSha512Bytes> material = first.Finish();
        uint8_t scalar[32];
        std::memcpy(scalar, material.data(), 32);
        scalar[0] &= 248;
        scalar[31] &= 127;
        scalar[31] |= 64;
        const uint8_t* prefix = material.data() + 32;

        // r = SHA-512(prefix || M) mod L，R = r * B
        Sha512 second;
        second.Begin();
        second.Update(std::span<const uint8_t>(prefix, 32));
        second.Update(message);
        const std::array<uint8_t, kSha512Bytes> rDigest = second.Finish();
        uint8_t r[32];
        ScalarReduce(r, rDigest.data(), 512);
        ScalarBaseEncode(r, signature.data());

        // k = SHA-512(R || A || M) mod L
        Sha512 third;
        third.Begin();
        third.Update(std::span<const uint8_t>(signature.data(), 32));
        third.Update(std::span<const uint8_t>(publicKey, 32));
        third.Update(message);
        const std::array<uint8_t, kSha512Bytes> kDigest = third.Finish();
        uint8_t k[32];
        ScalarReduce(k, kDigest.data(), 512);

        // S = (r + k * a) mod L
        ScalarMulAddModL(k, scalar, r, signature.data() + 32);
    }

    bool Ed25519Verify(std::span<const uint8_t> publicKey, std::span<const uint8_t> message, std::span<const uint8_t> signature) {
        if (publicKey.size() != kEd25519PublicKeyBytes || signature.size() != kEd25519SignatureBytes) return false;
        if (!ScalarLessThanL(signature.data() + 32)) return false;
        Point publicPoint;
        if (!PointUnpackNeg(publicPoint, publicKey.data())) return false;

        Sha512 hash;
        hash.Begin();
        hash.Update(signature.first(32));
        hash.Update(publicKey);
        hash.Update(message);
        const std::array<uint8_t, kSha512Bytes> digest = hash.Finish();
        uint8_t h[32];
        ScalarReduce(h, digest.data(), 512);

        Point computed, scalarBase;
        PointScalarMul(computed, publicPoint, h);
        PointScalarBase(scalarBase, signature.data() + 32);
        PointAdd(computed, scalarBase);

        uint8_t encoded[32];
        PointPack(encoded, computed);
        uint8_t diff = 0;
        for (unsigned i = 0; i < 32; ++i) diff |= encoded[i] ^ signature[i];
        return diff == 0;
    }
}
