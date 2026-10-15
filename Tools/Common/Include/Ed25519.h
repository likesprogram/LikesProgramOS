/* Ed25519.h
    Ed25519 签名与验签的宿主侧实现：供密钥生成、清单签名与自检使用

    与目标侧 Packages/Baleen/Common/Src/Ed25519.cpp（只有验签）是同一算法的两份实现，
    用 RFC 8032 测试向量互验；不共享源码，宿主工具不依赖目标包
    两侧对公钥解码的严格程度一致：非规范编码（y >= p）按解码失败拒绝，不拒绝小阶点
*/

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hostbuild {
    // Ed25519 公钥长度，字节
    constexpr std::size_t kEd25519PublicKeyBytes = 32;
    // Ed25519 签名长度，字节
    constexpr std::size_t kEd25519SignatureBytes = 64;
    // Ed25519 种子长度，字节
    constexpr std::size_t kEd25519SeedBytes = 32;
    // Ed25519 私钥长度，字节：种子与公钥拼接
    constexpr std::size_t kEd25519PrivateKeyBytes = 64;

    // 从种子派生公钥与私钥：私钥是 seed || publicKey 的 64 字节展开
    void Ed25519DeriveKey(std::span<const uint8_t> seed, std::array<uint8_t, kEd25519PublicKeyBytes>& publicKey, std::array<uint8_t, kEd25519PrivateKeyBytes>& privateKey);

    // 对消息签名；消息可为空，签名输出 64 字节
    void Ed25519Sign(std::span<const uint8_t> privateKey, std::span<const uint8_t> message, std::array<uint8_t, kEd25519SignatureBytes>& signature);

    // 验证签名：S 越界、公钥编码非规范或不可解压、等式不成立都返回 false
    bool Ed25519Verify(std::span<const uint8_t> publicKey, std::span<const uint8_t> message, std::span<const uint8_t> signature);
}
