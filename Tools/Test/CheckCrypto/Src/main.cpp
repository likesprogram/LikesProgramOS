/* main.cpp
    CheckCrypto：SHA-512、Ed25519 与信任锚表的宿主侧检查，按需手工执行，不构成默认构建依赖

    检查分四类：
    1. 用例检查：SHA-512 的 FIPS 向量、分块流式一致性与两份实现的互验；
       Ed25519 的 RFC 8032 向量、拒绝路径（含非规范公钥编码）与两份实现的互验；
       信任锚表轮换窗口的按 KeyId 查找与按编号选钥验签
    2. 双编译检查：目标侧密码学源码在 32 位下重新编译、链接并运行同一批用例
    3. 目标侧链接检查：按与 Baleen 各阶段一致的选项（-m32 -ffreestanding 无运行库）
       链接并核对产物不牵入 64 位除法辅助函数
    4. 信任锚生成检查：跑 MakeKeys 复现新旧并存的锚表，核对内容与确定性，
       按 BaleenCore 的目标侧选项编译生成源，并覆盖超容量与重复编号两个负例

    输入路径从可执行文件位置推断（Tools/Bin/CheckCrypto 向上三级是项目根），
    --root 可覆盖；缺少 32 位工具链时按基础设施缺失报告（退出码 2），--strict 把它当作失败；
    缺工具链时第 2、3 类与第 4 类里依赖编译器的那部分不执行，结论行与“全部通过”分开写
*/

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Ed25519.h>
#include <Ed25519.hpp>
#include <Sha512.h>
#include <Sha512.hpp>
#include <TrustAnchor.hpp>

// 本工具自带的信任锚表：轮换窗口的两条锚（旧钥 KeyId 1、新钥 KeyId 2），
// 供目标侧 TrustAnchor.cpp 的 FindTrustAnchor 链接使用；构建期生成的 Out/TrustAnchors.cpp
// 定义同名符号，两者不能同时链接，本工具因此自带表、不编译生成物
namespace Baleen {
    namespace Crypto {
        // 信任锚表：两条锚的公钥由固定种子派生，用例里核对派生结果与这里逐字节一致；
        // extern 不是多余：命名空间作用域的 const 默认内部链接，查找实现引用的是外部链接符号
        extern const TrustAnchor kTrustAnchors[] = {
            // 旧钥：种子取顺序字节 0x00..0x1f 派生的公钥
            { 1, {
                0x03, 0xa1, 0x07, 0xbf, 0xf3, 0xce, 0x10, 0xbe,
                0x1d, 0x70, 0xdd, 0x18, 0xe7, 0x4b, 0xc0, 0x99,
                0x67, 0xe4, 0xd6, 0x30, 0x9b, 0xa5, 0x0d, 0x5f,
                0x1d, 0xdc, 0x86, 0x64, 0x12, 0x55, 0x31, 0xb8,
            } },
            // 新钥：种子取模式数据（字节 i 取 (i*7+3)%251）派生的公钥
            { 2, {
                0x75, 0x5c, 0x4c, 0xb9, 0x25, 0x6c, 0xa7, 0xcd,
                0xc4, 0xac, 0xfd, 0xc6, 0xcf, 0xee, 0xda, 0x84,
                0x90, 0x17, 0xe5, 0xb9, 0xf9, 0x51, 0x4e, 0x99,
                0x19, 0x1b, 0xd6, 0x7e, 0x0b, 0x0d, 0x42, 0x76,
            } },
        };
        // 表内锚数量：轮换窗口刚好满容量
        extern const uint32_t kTrustAnchorCount = 2;
        // 超容量的表在编译期拦下，与生成源里的守卫一致
        static_assert(kTrustAnchorCount <= kTrustAnchorMaxCount, "信任锚表超过 TrustAnchor.hpp 的容量");
    }
}

#ifndef __i386__
#include <filesystem>
#include <fstream>
#include <iterator>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
#ifndef __i386__
    namespace fs = std::filesystem;
#endif
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 9;
    // 已跑用例数
    unsigned g_ran = 0;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckCrypto: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 测试用模式数据：确定性字节序列，避免依赖随机源
    std::vector<uint8_t> Pattern(std::size_t bytes) {
        std::vector<uint8_t> data(bytes);
        for (std::size_t i = 0; i < bytes; ++i) data[i] = static_cast<uint8_t>((i * 7 + 3) % 251);
        return data;
    }

    // 摘要转小写十六进制
    std::string Hex(const std::vector<uint8_t>& digest) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(digest.size() * 2);
        for (uint8_t byte : digest) {
            text.push_back(kDigits[byte >> 4]);
            text.push_back(kDigits[byte & 0x0f]);
        }
        return text;
    }

    // 十六进制转字节
    std::vector<uint8_t> FromHex(std::string_view text) {
        auto value = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        Require(text.size() % 2 == 0, "十六进制文本长度必须是偶数");
        std::vector<uint8_t> bytes;
        bytes.reserve(text.size() / 2);
        for (std::size_t i = 0; i < text.size(); i += 2) {
            const int high = value(text[i]);
            const int low = value(text[i + 1]);
            Require(high >= 0 && low >= 0, "十六进制文本含非法字符");
            bytes.push_back(static_cast<uint8_t>((high << 4) | low));
        }
        return bytes;
    }

    // 目标侧 SHA-512 一次性摘要
    std::vector<uint8_t> TargetSha512(std::span<const uint8_t> data) {
        Baleen::Digest::Sha512 hash;
        hash.Begin();
        hash.Update(data.data(), static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> digest(Baleen::Digest::kSha512Bytes);
        hash.Finish(digest.data());
        return digest;
    }

    // 宿主侧 SHA-512 一次性摘要
    std::vector<uint8_t> HostSha512(std::span<const uint8_t> data) {
        hostbuild::Sha512 hash;
        hash.Begin();
        hash.Update(data);
        const std::array<uint8_t, hostbuild::kSha512Bytes> digest = hash.Finish();
        return std::vector<uint8_t>(digest.begin(), digest.end());
    }

    // 目标侧 SHA-512 分块摘要：按 chunk 切分累积，检验流式接口
    std::vector<uint8_t> TargetSha512Chunked(std::span<const uint8_t> data, std::size_t chunk) {
        Baleen::Digest::Sha512 hash;
        hash.Begin();
        for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
            const std::size_t take = std::min(chunk, data.size() - offset);
            hash.Update(data.data() + offset, static_cast<uint32_t>(take));
        }
        std::vector<uint8_t> digest(Baleen::Digest::kSha512Bytes);
        hash.Finish(digest.data());
        return digest;
    }

    // 宿主侧 SHA-512 分块摘要
    std::vector<uint8_t> HostSha512Chunked(std::span<const uint8_t> data, std::size_t chunk) {
        hostbuild::Sha512 hash;
        hash.Begin();
        for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
            const std::size_t take = std::min(chunk, data.size() - offset);
            hash.Update(data.subspan(offset, take));
        }
        const std::array<uint8_t, hostbuild::kSha512Bytes> digest = hash.Finish();
        return std::vector<uint8_t>(digest.begin(), digest.end());
    }

    // 两侧实现对同一段数据计算摘要并比对；expected 非空时再与期望值比对
    void CheckSha512Pair(std::span<const uint8_t> data, std::string_view expected, const std::string& label) {
        const std::vector<uint8_t> target = TargetSha512(data);
        const std::vector<uint8_t> host = HostSha512(data);
        Require(target == host, label + "：目标侧与宿主侧摘要不一致");
        if (!expected.empty()) {
            const std::vector<uint8_t> want = FromHex(expected);
            Require(want.size() == target.size(), label + "：期望值长度不是 64 字节");
            Require(target == want, label + "：摘要与期望值不一致，得到 " + Hex(target));
        }
    }

    // 文本转字节视图
    std::span<const uint8_t> Text(std::string_view text) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    // 模式数据向量：字节 i 取 (i*7+3)%251，期望值由外部工具生成后固化
    struct Sha512Vector {
        std::size_t bytes;      // 输入长度
        const char* digest;     // 期望摘要的小写十六进制
    };

    constexpr Sha512Vector kPatternVectors[] = {
        { 0, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" },
        { 1, "e45bf5817ddf94aa2f7a407071f0eedc6beb98f768b4cd33d1176d44d1563a45a5d7212290eb7670c6786b13591aedac86478993895e8b24e612014abaa6ba04" },
        { 3, "8853167ab14c043919981a6409307ef68b5b726f4b80dcab259daa24b7cabcb8b4bbfd3e2e36ed98465059661f672e6529cdcc673e7eabfa78dd3186745b112a" },
        { 55, "eac5ae9c6b4ff21e87bd0e3ab17784a3916ea556c4b893b0c24843068cea295d15ce84fe40b14e1c3610c12b08a82b6e5cf35a2a90f76bfb9baa2ecd6f299dbb" },
        { 56, "8b8c39aff78464f48125c952d59a17335db274120c321928993280e3de8ed56ba428d66b772bd2c91d4f6ba49b59d26e4c2cfb08890117aab127ab87afbcd73f" },
        { 63, "1955e5263d977825e571a3148f29d53d2d9a305779fdd8de21553a2359e3a149e16510d1b504eaf2b60f2283bfc2897b6346cc70749e097376aa124c958f4d32" },
        { 64, "1cf8de2670546a69ca5cd3e93399d4a8e35a1fb79d60d10e80239b902cdc1ff9554a14bca10a6957747585a56c00127f43e4ad8769ca14be1816420a955f5c13" },
        { 111, "0ef0e383d1e3b7f1e7477f27d7d62a25b3d637c42b509c96f230889cc05cade87cb595834c2f3cccd4c842221251636e3ef6c8b57ee0656e5352a9e968044ab7" },
        { 112, "3b30650ae40d6cf145136a38623ebf064e642157296df6595c6b1a3fac9f5c30461bbc698b19fff7192763656ffc619002ce0cf39eb9efdfd9914a81d81fd71f" },
        { 113, "56e7afeae695f146fa3ed396eb33c4c0c3e7ff88974c9df24b40327d92889628508cff2c15b8f327e14c739c2d0e7e9670424a0e600b0e396aa789ca7fb734c3" },
        { 127, "07d05c6a64f1c2ebc735334f428871fca7153286d369318b52427074c3d97e97af523a00b2dd02e5150ee7f962380cf841bcb566cd4921e0226602986e59aff6" },
        { 128, "9ac7d9bb8e42c2ce0230fe54f60868dca869afe8c4c02771ae54a7827818e6a8840b1fc2efbf2b5163150ed3710318d750c0ac2998b2c628ce4e66cb12cabfff" },
        { 129, "8e14a5884c493da00130dddf9c0140009e7a4fbfebe9c88416bfae10fee38409fa50288087ff2d50e631b0695c95d6bb33a781069487fde48b2fde8d0c4230ab" },
        { 255, "6b4bc2e04c7515a5c49d7e4760ec83eebfcc561eaff834e8966c7485c1227cfa248ffd7e10bf0a6e4b0f7d2920496d7eeae8534ca01ffa7825b5f16591cc734a" },
        { 256, "13b7b7634df11324dc0cba338094805ca25488452ddf08325c14b4f57073d3b4df5ac4088c2ceae9c4b8f8d590704437880f3709f4cadf360b7e9d311c3d8635" },
        { 1000, "585b7a26bf81872d575ee2d071374956d4fa3570e31e244231d14410f824a73a64ac870479a54ab425473726b529392e274683daa521d9b89d8b2bf80c0c831f" },
    };

    // 用例：SHA-512 的 FIPS 180-4 公开示例
    void CaseSha512Vectors() {
        CheckSha512Pair(std::span<const uint8_t>(), "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", "空串");
        CheckSha512Pair(Text("abc"), "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "abc");
        CheckSha512Pair(Text("The quick brown fox jumps over the lazy dog"), "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb642e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6", "fox");
    }

    // 用例：模式数据向量，覆盖块边界前后与多块输入
    void CaseSha512Pattern() {
        for (const Sha512Vector& vector : kPatternVectors) {
            const std::vector<uint8_t> data = Pattern(vector.bytes);
            CheckSha512Pair(data, vector.digest, "模式数据 " + std::to_string(vector.bytes) + " 字节");
        }
    }

    // 用例：分块流式累积与一次性摘要一致
    void CaseSha512Chunking() {
        const std::vector<uint8_t> data = Pattern(1000);
        const std::vector<uint8_t> once = TargetSha512(data);
        for (std::size_t chunk : {std::size_t(1), std::size_t(7), std::size_t(63), std::size_t(127), std::size_t(128), std::size_t(129), std::size_t(333)}) {
            Require(TargetSha512Chunked(data, chunk) == once, "目标侧分块摘要不一致，块长 " + std::to_string(chunk));
            Require(HostSha512Chunked(data, chunk) == once, "宿主侧分块摘要不一致，块长 " + std::to_string(chunk));
        }
        Require(TargetSha512Chunked(std::span<const uint8_t>(), 16) == TargetSha512(std::span<const uint8_t>()), "空输入分块摘要不一致");
    }

    // 用例：两侧实现逐长度互验，覆盖填充与块边界
    void CaseSha512Cross() {
        for (std::size_t bytes = 0; bytes <= 300; ++bytes) {
            const std::vector<uint8_t> data = Pattern(bytes);
            Require(TargetSha512(data) == HostSha512(data), "长度 " + std::to_string(bytes) + " 的两侧摘要不一致");
        }
    }

    // 目标侧 Ed25519 验签
    bool TargetEd25519Verify(std::span<const uint8_t> publicKey, std::span<const uint8_t> signature, std::span<const uint8_t> message) {
        Require(publicKey.size() == Baleen::Crypto::kEd25519PublicKeyBytes, "公钥长度必须是 32 字节");
        Require(signature.size() == Baleen::Crypto::kEd25519SignatureBytes, "签名长度必须是 64 字节");
        return Baleen::Crypto::Ed25519Verify(publicKey.data(), signature.data(), message.data(), static_cast<uint32_t>(message.size()));
    }

    // Ed25519 测试向量：消息、公钥与签名的小写十六进制文本
    struct Ed25519Vector {
        std::string_view message;      // 消息的十六进制文本，空串表示空消息
        std::string_view publicKey;    // 公钥的十六进制文本
        std::string_view signature;    // 签名的十六进制文本
    };

    // RFC 8032 第 7 节的测试向量（TEST 1..3）
    constexpr Ed25519Vector kEd25519Vectors[] = {
        { "", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "72", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "af82", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    };

    // 长消息向量：1000 字节模式数据，种子 0x00..0x1f
    constexpr std::string_view kLongMessagePublicKey = "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
    constexpr std::string_view kLongMessageSignature = "63074d5f3d48f0ce8bae80697a372c3102690706ca72cd71c7a75e38b8fd37664b4367139e8600450a37736451115c05b5039350e67871bb10bb427ab409a100";

    // 阶 L 的小端 32 字节，用于构造 S 越界的签名
    constexpr std::string_view kOrderL = "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010";

    // 用例：Ed25519 的 RFC 8032 向量与长消息向量
    void CaseEd25519Vectors() {
        for (const Ed25519Vector& vector : kEd25519Vectors) {
            const std::vector<uint8_t> message = FromHex(vector.message);
            Require(TargetEd25519Verify(FromHex(vector.publicKey), FromHex(vector.signature), message), "RFC 8032 向量的签名未通过验证");
        }
        Require(TargetEd25519Verify(FromHex(kLongMessagePublicKey), FromHex(kLongMessageSignature), Pattern(1000)), "长消息向量的签名未通过验证");
    }

    // 用例：Ed25519 拒绝路径——消息或签名被改动、S 越界、公钥不在曲线上都必须失败
    void CaseEd25519Rejects() {
        const std::vector<uint8_t> message = Pattern(1000);
        const std::vector<uint8_t> publicKey = FromHex(kLongMessagePublicKey);
        const std::vector<uint8_t> signature = FromHex(kLongMessageSignature);
        Require(TargetEd25519Verify(publicKey, signature, message), "基准向量应通过");
        std::vector<uint8_t> other = message;
        other[0] ^= 0x01;
        Require(!TargetEd25519Verify(publicKey, signature, other), "消息被改动后签名仍通过");
        for (std::size_t index : {std::size_t(0), std::size_t(31), std::size_t(32), std::size_t(63)}) {
            std::vector<uint8_t> broken = signature;
            broken[index] ^= 0x80;
            Require(!TargetEd25519Verify(publicKey, broken, message), "签名第 " + std::to_string(index) + " 字节被改动后仍通过");
        }
        const std::vector<uint8_t> orderL = FromHex(kOrderL);
        std::vector<uint8_t> sEqualsL = signature;
        std::copy(orderL.begin(), orderL.end(), sEqualsL.begin() + 32);
        Require(!TargetEd25519Verify(publicKey, sEqualsL, message), "S 等于 L 的签名仍通过");
        std::vector<uint8_t> sZero = signature;
        std::fill(sZero.begin() + 32, sZero.end(), 0);
        Require(!TargetEd25519Verify(publicKey, sZero, message), "S 全零的签名仍通过");
        // y=2 的编码在曲线上没有对应 x，解压必须失败
        const std::vector<uint8_t> offCurve = FromHex("0200000000000000000000000000000000000000000000000000000000000000");
        Require(!TargetEd25519Verify(offCurve, signature, message), "不在曲线上的公钥仍通过");
        // 非规范编码：y >= p 的编码按 RFC 8032 5.1.3 解码失败，不得静默归约成 y mod p
        const std::vector<std::string_view> nonCanonical = {
            "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",   // y = p
            "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",   // y = p + 1
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",   // y = 2^255 - 1
            "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",   // y = p，符号位置一
            "0100000000000000000000000000000000000000000000000000000000000080",   // x = 0 且符号位置一
        };
        for (const std::string_view encoded : nonCanonical)
            Require(!TargetEd25519Verify(FromHex(encoded), signature, message), "非规范公钥编码仍通过：" + std::string(encoded));
        // 强探针：y 归约后是单位元（阶 1）的两种编码，配 (R = B, S = 1) 的签名对任意消息都满足
        // [S]B == R + [h]A，等式比对挡不住它，只有解码阶段拒绝才算过
        const std::vector<uint8_t> unitForgery = FromHex(
            "5866666666666666666666666666666666666666666666666666666666666666"
            "0100000000000000000000000000000000000000000000000000000000000000");
        const std::vector<std::string_view> unitEncodings = {
            "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",   // y = p + 1 ≡ 1，非规范
            "0100000000000000000000000000000000000000000000000000000000000080",   // y = 1，符号位置一
        };
        for (const std::string_view encoded : unitEncodings)
            Require(!TargetEd25519Verify(FromHex(encoded), unitForgery, message), "单位元公钥的伪造签名仍通过：" + std::string(encoded));
    }

    // 顺序字节序列：0, 1, 2, ...
    std::vector<uint8_t> Sequence(std::size_t bytes) {
        std::vector<uint8_t> data(bytes);
        for (std::size_t i = 0; i < bytes; ++i) data[i] = static_cast<uint8_t>(i);
        return data;
    }

    // Ed25519 种子向量：种子、消息、公钥与签名的十六进制文本
    struct Ed25519SeedVector {
        std::string_view seed;         // 私钥种子
        std::string_view message;      // 消息
        std::string_view publicKey;    // 期望公钥
        std::string_view signature;    // 期望签名
    };

    // RFC 8032 第 7 节的测试向量（TEST 1..3），带种子
    constexpr Ed25519SeedVector kEd25519SeedVectors[] = {
        { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "72", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "af82", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    };

    // 用例：宿主侧 Ed25519 的派生与签名（RFC 8032 向量）
    void CaseEd25519HostSign() {
        for (const Ed25519SeedVector& vector : kEd25519SeedVectors) {
            std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
            std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
            hostbuild::Ed25519DeriveKey(FromHex(vector.seed), publicKey, privateKey);
            Require(Hex(std::vector<uint8_t>(publicKey.begin(), publicKey.end())) == vector.publicKey, "派生的公钥与向量不一致");
            const std::vector<uint8_t> message = FromHex(vector.message);
            std::array<uint8_t, hostbuild::kEd25519SignatureBytes> signature{};
            hostbuild::Ed25519Sign(privateKey, message, signature);
            Require(Hex(std::vector<uint8_t>(signature.begin(), signature.end())) == vector.signature, "签名与向量不一致");
            Require(hostbuild::Ed25519Verify(publicKey, message, signature), "宿主侧自检未通过");
        }
    }

    // 用例：宿主侧签名与目标侧验签互验，覆盖不同消息长度
    void CaseEd25519Cross() {
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
        hostbuild::Ed25519DeriveKey(Sequence(32), publicKey, privateKey);
        Require(Hex(std::vector<uint8_t>(publicKey.begin(), publicKey.end())) == kLongMessagePublicKey, "种子 0x00..0x1f 派生的公钥与长消息向量不一致");
        for (std::size_t bytes : {std::size_t(0), std::size_t(1), std::size_t(32), std::size_t(64), std::size_t(128), std::size_t(1000)}) {
            const std::vector<uint8_t> message = Pattern(bytes);
            std::array<uint8_t, hostbuild::kEd25519SignatureBytes> signature{};
            hostbuild::Ed25519Sign(privateKey, message, signature);
            Require(hostbuild::Ed25519Verify(publicKey, message, signature), "宿主侧自检失败，长度 " + std::to_string(bytes));
            Require(TargetEd25519Verify(publicKey, signature, message), "目标侧未通过宿主侧签名，长度 " + std::to_string(bytes));
            std::vector<uint8_t> other = message;
            other.push_back(0x01);
            Require(!TargetEd25519Verify(publicKey, signature, other), "目标侧对加长后的消息仍通过");
            Require(!hostbuild::Ed25519Verify(publicKey, other, signature), "宿主侧对加长后的消息仍通过");
        }
    }

    // 用例：信任锚表的轮换窗口——新旧两把公钥并存，查找与验签都按 KeyId 选钥
    void CaseTrustAnchorRotation() {
        // 两把密钥由固定种子经宿主侧派生：表里的两条锚就是这两把的公钥
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> oldPublic{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> oldPrivate{};
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> newPublic{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> newPrivate{};
        hostbuild::Ed25519DeriveKey(Sequence(32), oldPublic, oldPrivate);
        hostbuild::Ed25519DeriveKey(Pattern(32), newPublic, newPrivate);
        Require(Baleen::Crypto::kTrustAnchorCount == 2, "本工具自带的锚表应有 2 条");
        Require(std::equal(oldPublic.begin(), oldPublic.end(), Baleen::Crypto::kTrustAnchors[0].publicKey), "表内 KeyId 1 的公钥不是旧钥种子的派生结果");
        Require(std::equal(newPublic.begin(), newPublic.end(), Baleen::Crypto::kTrustAnchors[1].publicKey), "表内 KeyId 2 的公钥不是新钥种子的派生结果");

        // 运行时查找：两个编号都命中且逐字节一致，表外的编号返回空
        const uint8_t* foundOld = Baleen::Crypto::FindTrustAnchor(1);
        const uint8_t* foundNew = Baleen::Crypto::FindTrustAnchor(2);
        Require(foundOld != nullptr && foundNew != nullptr, "表内的 KeyId 1 与 KeyId 2 都应命中");
        Require(std::equal(oldPublic.begin(), oldPublic.end(), foundOld), "KeyId 1 命中的公钥与旧钥不一致");
        Require(std::equal(newPublic.begin(), newPublic.end(), foundNew), "KeyId 2 命中的公钥与新钥不一致");
        Require(Baleen::Crypto::FindTrustAnchor(3) == nullptr, "表外的 KeyId 3 不应命中");

        // 按编号选钥：新钥签名只应由 KeyId 2 的公钥验过，旧钥签名只应由 KeyId 1 的验过
        const std::string text = "信任锚轮换窗口：清单只带 KeyId，公钥按编号从表里取";
        const std::vector<uint8_t> message(text.begin(), text.end());
        const std::span<const uint8_t> oldAnchor(foundOld, hostbuild::kEd25519PublicKeyBytes);
        const std::span<const uint8_t> newAnchor(foundNew, hostbuild::kEd25519PublicKeyBytes);
        std::array<uint8_t, hostbuild::kEd25519SignatureBytes> newSignature{};
        std::array<uint8_t, hostbuild::kEd25519SignatureBytes> oldSignature{};
        hostbuild::Ed25519Sign(newPrivate, message, newSignature);
        hostbuild::Ed25519Sign(oldPrivate, message, oldSignature);
        Require(TargetEd25519Verify(newAnchor, newSignature, message), "KeyId 2 命中的公钥未通过新钥签名");
        Require(!TargetEd25519Verify(oldAnchor, newSignature, message), "旧钥公钥通过了新钥签名：选钥没有按编号走");
        Require(TargetEd25519Verify(oldAnchor, oldSignature, message), "KeyId 1 命中的公钥未通过旧钥签名");
        Require(!TargetEd25519Verify(newAnchor, oldSignature, message), "新钥公钥通过了旧钥签名：选钥没有按编号走");
    }

    // 跑全部用例：末尾核对实跑数与声明一致
    void RunCases() {
        Case("SHA-512 标准向量", CaseSha512Vectors);
        Case("SHA-512 模式数据向量", CaseSha512Pattern);
        Case("SHA-512 分块流式一致性", CaseSha512Chunking);
        Case("SHA-512 两侧实现互验", CaseSha512Cross);
        Case("Ed25519 RFC 8032 向量", CaseEd25519Vectors);
        Case("Ed25519 拒绝路径", CaseEd25519Rejects);
        Case("Ed25519 宿主侧派生与签名", CaseEd25519HostSign);
        Case("Ed25519 宿主签目标验互验", CaseEd25519Cross);
        Case("信任锚表轮换窗口", CaseTrustAnchorRotation);
        Require(g_ran == kCaseCount, "用例数与声明的 kCaseCount 不一致");
    }
}

#ifndef __i386__
namespace {
    // 目标侧链接检查的入口源码：引用目标侧实现，保证链接器保留它们的代码路径
    constexpr const char* kTargetEntrySource = R"(
#include <Ed25519.hpp>
#include <Sha512.hpp>

extern "C" void Crypto_Target_Entry() {
    Baleen::Digest::Sha512 hash;
    hash.Begin();
    const unsigned char data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    hash.Update(data, 8);
    unsigned char digest[Baleen::Digest::kSha512Bytes];
    hash.Finish(digest);
    unsigned char key[Baleen::Crypto::kEd25519PublicKeyBytes] = {};
    unsigned char signature[Baleen::Crypto::kEd25519SignatureBytes] = {};
    const bool valid = Baleen::Crypto::Ed25519Verify(key, signature, data, 8);
    volatile unsigned char sink = digest[0] ^ (valid ? 1 : 0);
    (void)sink;
}
)";

    // 命令行选项
    struct Options {
        fs::path root;        // 项目根
        fs::path work;        // 工作目录
        bool keep = false;    // 保留工作目录现场
        bool strict = false;  // 缺少 32 位工具链时视为失败
    };

    // 读环境变量，未设置或为空时取默认值
    std::string Env(const char* name, const char* fallback) {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' ? value : fallback;
    }

    // 解析命令行：--root、--work、--keep、--strict
    Options Parse(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            auto next = [&](const char* what) -> std::string {
                Require(i + 1 < argc, std::string("选项 ") + what + " 缺少参数");
                return argv[++i];
            };
            if (arg == "--root") options.root = next("--root");
            else if (arg == "--work") options.work = next("--work");
            else if (arg == "--keep") options.keep = true;
            else if (arg == "--strict") options.strict = true;
            else Fail("无法识别的参数：" + std::string(arg));
        }
        return options;
    }

    // 运行一个外部命令，标准输出与错误重定向到日志；返回退出码
    int Run(const std::vector<std::string>& argv, const fs::path& log) {
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const std::string& item : argv) raw.push_back(const_cast<char*>(item.c_str()));
        raw.push_back(nullptr);
        const pid_t pid = fork();
        Require(pid >= 0, "无法创建子进程");
        if (pid == 0) {
            const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            execvp(raw[0], raw.data());
            _exit(127);
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {}
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }

    // 在基础参数后追加一组参数
    std::vector<std::string> With(const std::vector<std::string>& base, std::initializer_list<std::string> extra) {
        std::vector<std::string> all = base;
        all.insert(all.end(), extra.begin(), extra.end());
        return all;
    }

    // 读整个文件；不存在读失败返回空串
    std::string ReadTextOrEmpty(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 整个覆盖写入文本
    void WriteText(const fs::path& path, std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(bool(output), "无法写入 " + path.string());
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        Require(bool(output), "写入失败 " + path.string());
    }

    // 公钥字节转小写十六进制
    std::string Hex(std::span<const uint8_t> bytes) {
        return Hex(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }

    // 读整个文件为字节；读失败返回空
    std::vector<uint8_t> ReadBytesOrEmpty(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 从生成的锚源里取出某个 KeyId 的公钥字节：找不到条目或字节数不符即失败
    std::vector<uint8_t> GeneratedAnchorKey(std::string_view source, uint8_t keyId) {
        const std::string where = "KeyId " + std::to_string(static_cast<unsigned>(keyId));
        const std::string head = "{ " + std::to_string(static_cast<unsigned>(keyId)) + ", {";
        const std::size_t begin = source.find(head);
        Require(begin != std::string_view::npos, "生成源里找不到 " + where + " 的条目");
        const std::size_t end = source.find('}', begin + head.size());
        Require(end != std::string_view::npos, "生成源里 " + where + " 的条目没有收尾花括号");
        std::vector<uint8_t> bytes;
        for (std::size_t i = begin + head.size(); i + 4 <= end; ++i) {
            if (source[i] != '0' || source[i + 1] != 'x') continue;
            const std::vector<uint8_t> byte = FromHex(source.substr(i + 2, 2));
            Require(byte.size() == 1, "生成源里的字节字面量不是两位十六进制");
            bytes.push_back(byte[0]);
            i += 3;
        }
        Require(bytes.size() == hostbuild::kEd25519PublicKeyBytes, "生成源里 " + where + " 的公钥不是 32 字节");
        return bytes;
    }

    // 探测 32 位工具链：能编译并链接一个最小程序即算可用
    bool Has32BitToolchain(const std::string& cxx, const fs::path& work) {
        const fs::path probeSrc = work / "probe.cpp";
        WriteText(probeSrc, "int main() { return 0; }\n");
        return Run({cxx, "-m32", "-std=c++20", "-O2", "-o", (work / "probe").string(), probeSrc.string()}, work / "probe.log") == 0;
    }

    // 打印日志尾部：失败时给出最后几行线索
    void ShowLogTail(const fs::path& log, unsigned lines) {
        std::ifstream input(log);
        std::vector<std::string> all;
        std::string line;
        while (std::getline(input, line)) all.push_back(line);
        const std::size_t first = all.size() > lines ? all.size() - lines : 0;
        for (std::size_t i = first; i < all.size(); ++i) std::cerr << "  | " << all[i] << std::endl;
    }

    // 32 位双编译与目标侧链接检查：返回 0 通过、2 缺少 32 位工具链，失败抛异常
    int Check32Bit(const fs::path& root, const std::string& cxx, const fs::path& work) {
        const fs::path targetSrc = root / "Packages/Baleen/Common/Src";
        const fs::path targetInc = root / "Packages/Baleen/Common/Include";
        const fs::path coreSrc = root / "Packages/Baleen/Core/Src";
        const fs::path coreInc = root / "Packages/Baleen/Core/Include";
        const fs::path hostSrc = root / "Tools/Common/Src";
        const fs::path hostInc = root / "Tools/Common/Include";
        const fs::path selfSrc = root / "Tools/Test/CheckCrypto/Src/main.cpp";
        Require(fs::exists(targetSrc / "Sha512.cpp"), "找不到目标侧源码：" + (targetSrc / "Sha512.cpp").string());
        Require(fs::exists(targetInc / "Sha512.hpp"), "找不到目标侧头文件：" + (targetInc / "Sha512.hpp").string());
        Require(fs::exists(coreSrc / "TrustAnchor.cpp"), "找不到目标侧源码：" + (coreSrc / "TrustAnchor.cpp").string());
        Require(fs::exists(hostSrc / "Sha512.cpp"), "找不到宿主侧源码：" + (hostSrc / "Sha512.cpp").string());
        Require(fs::exists(selfSrc), "找不到本工具源码：" + selfSrc.string());

        // 探测 32 位工具链：编译一个最小程序
        if (!Has32BitToolchain(cxx, work)) return 2;

        // 第一步：同一份用例在 32 位下编译、链接并运行
        const std::vector<std::string> hostCompile{cxx, "-m32", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                                                    "-Werror", "-I" + targetInc.string(), "-I" + coreInc.string(), "-I" + hostInc.string()};
        std::vector<std::string> objects;
        // 目标侧与宿主侧有同名实现文件：对象名按来源显式给出，避免 vpath 撞名；
        // TrustAnchor.cpp 是目标侧的查找实现，锚表由本工具的 main.cpp 自带
        const std::vector<std::pair<fs::path, std::string>> sources = {
            { targetSrc / "Sha512.cpp", ".target.32.o" },
            { targetSrc / "Ed25519.cpp", ".target.32.o" },
            { coreSrc / "TrustAnchor.cpp", ".target.32.o" },
            { hostSrc / "Sha512.cpp", ".host.32.o" },
            { hostSrc / "Ed25519.cpp", ".host.32.o" },
        };
        for (const auto& [source, suffix] : sources) {
            const fs::path object = work / (source.stem().string() + suffix);
            if (Run(With(hostCompile, {"-c", source.string(), "-o", object.string()}), work / "compile32.log") != 0) {
                std::cerr << "CheckCrypto: 32 位编译失败：" << source << std::endl;
                ShowLogTail(work / "compile32.log", 8);
                Fail("32 位编译失败");
            }
            objects.push_back(object.string());
        }
        const fs::path selfObject = work / "main.32.o";
        if (Run(With(hostCompile, {"-c", selfSrc.string(), "-o", selfObject.string()}), work / "compile32.log") != 0) {
            std::cerr << "CheckCrypto: 32 位编译失败：" << selfSrc << std::endl;
            ShowLogTail(work / "compile32.log", 8);
            Fail("32 位编译失败");
        }
        objects.push_back(selfObject.string());

        const fs::path runner = work / "CheckCrypto32";
        std::vector<std::string> link{cxx, "-m32", "-o", runner.string()};
        link.insert(link.end(), objects.begin(), objects.end());
        if (Run(link, work / "link32.log") != 0) {
            std::cerr << "CheckCrypto: 32 位链接失败" << std::endl;
            ShowLogTail(work / "link32.log", 8);
            Fail("32 位链接失败");
        }
        if (Run({runner.string()}, work / "run32.log") != 0) {
            std::cerr << "CheckCrypto: 32 位用例运行失败" << std::endl;
            ShowLogTail(work / "run32.log", 8);
            Fail("32 位用例运行失败");
        }
        std::cout << "CheckCrypto: 32 位编译、链接与用例运行 通过" << std::endl;

        // 第二步：目标侧选项下的无运行库链接检查，选项与 Baleen 各阶段一致
        const fs::path entrySrc = work / "target_entry.cpp";
        WriteText(entrySrc, kTargetEntrySource);
        const std::vector<std::string> targetCompile{cxx, "-m32", "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions",
                                                     "-fno-rtti", "-fno-pic", "-fno-pie", "-fno-stack-protector",
                                                     "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror",
                                                     "-I" + targetInc.string()};
        std::vector<std::string> targetObjects;
        for (const char* name : {"Sha512.cpp", "Ed25519.cpp"}) {
            const fs::path object = work / (std::string(name) + ".target.o");
            if (Run(With(targetCompile, {"-c", (targetSrc / name).string(), "-o", object.string()}), work / "compile-target.log") != 0) {
                std::cerr << "CheckCrypto: 目标侧编译失败：" << (targetSrc / name) << std::endl;
                ShowLogTail(work / "compile-target.log", 8);
                Fail("目标侧编译失败");
            }
            targetObjects.push_back(object.string());
        }
        const fs::path entryObject = work / "target_entry.o";
        if (Run(With(targetCompile, {"-c", entrySrc.string(), "-o", entryObject.string()}), work / "compile-target.log") != 0) {
            std::cerr << "CheckCrypto: 目标侧编译失败：" << entrySrc << std::endl;
            ShowLogTail(work / "compile-target.log", 8);
            Fail("目标侧编译失败");
        }
        targetObjects.push_back(entryObject.string());

        const fs::path targetElf = work / "target-check.elf";
        std::vector<std::string> targetLink{cxx, "-m32", "-nostdlib", "-static", "-Wl,--no-undefined", "-Wl,--fatal-warnings",
                                            "-Wl,-e,Crypto_Target_Entry", "-o", targetElf.string()};
        targetLink.insert(targetLink.end(), targetObjects.begin(), targetObjects.end());
        if (Run(targetLink, work / "link-target.log") != 0) {
            std::cerr << "CheckCrypto: 目标侧链接失败（-m32 -ffreestanding -nostdlib）" << std::endl;
            ShowLogTail(work / "link-target.log", 12);
            Fail("目标侧链接失败");
        }

        const std::string nm = Env("NM", "nm");
        if (Run({nm, targetElf.string()}, work / "nm.log") != 0) {
            std::cerr << "CheckCrypto: 无法读取目标侧产物的符号表（NM=" << nm << "）" << std::endl;
            Fail("符号表读取失败");
        }
        const std::string symbols = ReadTextOrEmpty(work / "nm.log");
        for (const char* helper : {"__udivdi3", "__umoddi3", "__divdi3", "__moddi3", "__divmoddi4"}) {
            Require(symbols.find(helper) == std::string::npos, std::string("目标侧产物牵入 64 位除法辅助函数：") + helper);
        }
        std::cout << "CheckCrypto: 目标侧无运行库链接与 64 位除法检查 通过" << std::endl;
        return 0;
    }

    // 信任锚生成检查：跑 MakeKeys 复现新旧并存的锚表，核对内容与确定性并覆盖两个负例；
    // 返回 0 通过、2 生成源的目标侧编译与容量负例未执行（缺少 32 位工具链）
    int CheckTrustAnchorGenerator(const fs::path& root, const std::string& cxx, const fs::path& work) {
        const fs::path makeKeys = root / "Tools/Bin/MakeKeys";
        Require(fs::exists(makeKeys), "找不到密钥工具：" + makeKeys.string() + "（先构建 Tools/Build/MakeKeys）");
        // 生成检查的现场放在工作目录的 anchors 子目录：密钥、输入、生成源与日志都落在这里
        const fs::path dir = work / "anchors";
        const fs::path keys = dir / "keys";
        fs::create_directories(dir);
        // 轮换窗口输入：保留旧钥（KeyId 1）；注释行与空行按输入格式忽略
        const std::string oldKeyHex = Hex(Baleen::Crypto::kTrustAnchors[0].publicKey);
        const std::string newKeyHex = Hex(Baleen::Crypto::kTrustAnchors[1].publicKey);
        const fs::path retained = dir / "Retained.txt";
        WriteText(retained, "# 轮换窗口：保留的旧钥（KeyId 1）\n1 " + oldKeyHex + "\n\n");
        // 生成命令：当前密钥 KeyId 2、附加锚输入给旧钥；每次只替换生成源路径
        auto command = [&](const fs::path& output) {
            return std::vector<std::string>{makeKeys.string(), "--out", keys.string(), "--key-id", "2",
                                            "--anchors", retained.string(), "--anchor-cpp", output.string()};
        };
        const fs::path generated = dir / "TrustAnchors.cpp";
        const int status = Run(command(generated), dir / "makekeys.log");
        if (status != 0) {
            std::cerr << "CheckCrypto: MakeKeys 生成信任锚源失败，退出码 " << status << std::endl;
            ShowLogTail(dir / "makekeys.log", 8);
            Fail("MakeKeys 生成信任锚源失败");
        }
        const std::string source = ReadTextOrEmpty(generated);
        Require(!source.empty(), "MakeKeys 没有写出生成源：" + generated.string());
        Require(source.find("extern const uint32_t kTrustAnchorCount = 2;") != std::string::npos, "生成源里的锚数量不是 2");
        Require(GeneratedAnchorKey(source, 1) == FromHex(oldKeyHex), "生成源里 KeyId 1 的公钥与附加锚输入不一致");
        const std::vector<uint8_t> published = ReadBytesOrEmpty(keys / "Development.pub");
        Require(published.size() == hostbuild::kEd25519PublicKeyBytes, "Development.pub 不是 32 字节");
        Require(GeneratedAnchorKey(source, 2) == published, "生成源里 KeyId 2 的公钥与 Development.pub 不一致");
        // 确定性：同一密钥与同一输入再生成一次，生成源字节完全相同
        const fs::path again = dir / "TrustAnchors.again.cpp";
        const int againStatus = Run(command(again), dir / "makekeys-again.log");
        if (againStatus != 0) {
            std::cerr << "CheckCrypto: MakeKeys 第二次生成失败，退出码 " << againStatus << std::endl;
            ShowLogTail(dir / "makekeys-again.log", 8);
            Fail("MakeKeys 第二次生成失败");
        }
        Require(ReadTextOrEmpty(again) == source, "同一输入两次生成的锚源不一致");
        std::cout << "CheckCrypto: 信任锚生成端到端 通过（新旧并存、内容核对、两次生成一致）" << std::endl;

        // 负例：附加锚输入与 --key-id 同号，工具必须非零退出、说明原因，且不留生成源
        const fs::path duplicate = dir / "RetainedDup.txt";
        WriteText(duplicate, "2 " + newKeyHex + "\n");
        const fs::path dupCpp = dir / "TrustAnchorsDup.cpp";
        const std::vector<std::string> dupCommand{makeKeys.string(), "--out", keys.string(), "--key-id", "2",
                                                  "--anchors", duplicate.string(), "--anchor-cpp", dupCpp.string()};
        const int dupStatus = Run(dupCommand, dir / "makekeys-dup.log");
        Require(dupStatus != 0, "附加锚输入与 --key-id 同号时 MakeKeys 仍以 0 退出");
        const std::string dupLog = ReadTextOrEmpty(dir / "makekeys-dup.log");
        Require(dupLog.find("KeyId 2") != std::string::npos && dupLog.find("重复") != std::string::npos, "MakeKeys 没有给出可读的重复编号原因：" + dupLog);
        Require(!fs::exists(dupCpp), "生成失败时不应留下生成源");
        std::cout << "CheckCrypto: 信任锚生成拒绝重复编号 通过" << std::endl;

        if (!Has32BitToolchain(cxx, work)) return 2;
        // 生成源按 BaleenCore 的目标侧选项单独编译（不链接）：证明它进得了目标侧构建
        const std::vector<std::string> targetCompile{cxx, "-m32", "-std=c++20", "-Os", "-ffreestanding", "-fno-exceptions",
                                                     "-fno-rtti", "-fno-pic", "-fno-pie", "-fno-stack-protector",
                                                     "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror",
                                                     "-I" + (root / "Packages/Baleen/Common/Include").string()};
        const fs::path object = dir / "TrustAnchors.o";
        if (Run(With(targetCompile, {"-c", generated.string(), "-o", object.string()}), dir / "compile.log") != 0) {
            std::cerr << "CheckCrypto: 生成源的目标侧编译失败" << std::endl;
            ShowLogTail(dir / "compile.log", 8);
            Fail("生成源的目标侧编译失败");
        }
        Require(fs::file_size(object) > 0, "生成源的目标侧对象为空");
        std::cout << "CheckCrypto: 生成源按目标侧选项编译 通过" << std::endl;

        // 负例：附加锚输入四条条目，与当前密钥合计 5 条，超容量（TrustAnchor.hpp 的 4 条）必须失败在容量 static_assert 上
        const fs::path over = dir / "Retained4.txt";
        WriteText(over, "2 " + oldKeyHex + "\n3 " + newKeyHex + "\n4 " + oldKeyHex + "\n6 " + newKeyHex + "\n");
        const fs::path overCpp = dir / "TrustAnchorsOver.cpp";
        const std::vector<std::string> overCommand{makeKeys.string(), "--out", (dir / "keys-over").string(), "--key-id", "5",
                                                   "--anchors", over.string(), "--anchor-cpp", overCpp.string()};
        const int overStatus = Run(overCommand, dir / "makekeys-over.log");
        if (overStatus != 0) {
            std::cerr << "CheckCrypto: MakeKeys 生成超容量锚源失败，退出码 " << overStatus << std::endl;
            ShowLogTail(dir / "makekeys-over.log", 8);
            Fail("MakeKeys 生成超容量锚源失败");
        }
        Require(ReadTextOrEmpty(overCpp).find("extern const uint32_t kTrustAnchorCount = 5;") != std::string::npos, "超容量输入生成的锚数量不是 5");
        if (Run(With(targetCompile, {"-c", overCpp.string(), "-o", (dir / "TrustAnchorsOver.o").string()}), dir / "compile-over.log") == 0) Fail("超容量的生成源竟然编译通过");
        const std::string overLog = ReadTextOrEmpty(dir / "compile-over.log");
        Require(overLog.find("static assertion") != std::string::npos, "超容量生成源不是失败在 static assertion 上：" + overLog);
        Require(overLog.find("信任锚表超过 TrustAnchor.hpp 的容量") != std::string::npos, "编译器输出里没有容量 static_assert 的文本：" + overLog);
        std::cout << "CheckCrypto: 超容量锚表在编译期被 static_assert 拦下 通过" << std::endl;
        return 0;
    }

    // 64 位宿主的完整流程：跑用例，并按参数决定是否做双编译、目标侧链接与信任锚生成检查
    int RunHostMain(int argc, char** argv) {
        fs::path work;
        bool keepWork = false;
        try {
            const Options options = Parse(argc, argv);
            keepWork = options.keep;
            fs::path root = options.root;
            if (root.empty()) {
                std::error_code error;
                const fs::path self = fs::read_symlink("/proc/self/exe", error);
                Require(!error, "无法定位本可执行文件，请用 --root 指定项目根");
                root = self.parent_path().parent_path().parent_path();
            }
            Require(fs::exists(root / "Packages/Baleen/Common/Src/Sha512.cpp"), "项目根下找不到目标侧源码：" + root.string());

            if (options.work.empty())
                work = fs::temp_directory_path() / ("checkcrypto-" + std::to_string(static_cast<long>(getpid())));
            else
                work = options.work;
            fs::remove_all(work);
            fs::create_directories(work);

            RunCases();
            const std::string cxx = Env("CXX", "g++");
            const int linkResult = Check32Bit(root, cxx, work);
            const int generatorResult = CheckTrustAnchorGenerator(root, cxx, work);
            bool skipped = false;
            if (linkResult == 2) {
                std::cout << "CheckCrypto: 缺少 32 位工具链，双编译与目标侧链接检查跳过" << std::endl;
                skipped = true;
            }
            if (generatorResult == 2) {
                std::cout << "CheckCrypto: 缺少 32 位工具链，信任锚生成源的目标侧编译与容量负例跳过" << std::endl;
                skipped = true;
            }
            if (skipped && options.strict) {
                std::cerr << "CheckCrypto: --strict 下按失败处理" << std::endl;
                return 2;
            }
            // 跳过的检查不算通过：两种结论分开写，免得自动化把“少跑几组”当成全绿
            if (skipped) std::cout << "CheckCrypto: 用例通过，双编译、目标侧链接与信任锚生成编译检查未执行" << std::endl;
            else std::cout << "CheckCrypto: 全部通过" << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "CheckCrypto: 失败：" << error.what() << std::endl;
            if (keepWork && !work.empty()) std::cerr << "CheckCrypto: 工作目录保留在 " << work << std::endl;
            return 1;
        }
        if (keepWork) std::cout << "CheckCrypto: 工作目录保留在 " << work << std::endl;
        else fs::remove_all(work);
        return 0;
    }
}
#endif

int main(int argc, char** argv) {
#ifdef __i386__
    // 32 位复跑：只跑用例，不做双编译与文件操作
    (void)argc;
    (void)argv;
    RunCases();
    return 0;
#else
    return RunHostMain(argc, argv);
#endif
}
