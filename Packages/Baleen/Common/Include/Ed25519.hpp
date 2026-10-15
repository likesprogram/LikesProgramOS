/* Ed25519.hpp
    Ed25519 签名验证（RFC 8032）：供引导选择器在内核集装载前验证清单签名

    只包含验签：私钥操作在宿主侧工具里，引导器只内嵌公钥表，不带签名能力
    验证顺序按 RFC 8032 第 5.1.7 节：先检查 S < L，再按第 5.1.3 节解压公钥
    （非规范编码 y >= p 解码失败），最后验证 [S]B == R + [h]A
    解压不拒绝小阶点：公钥来自引导器内嵌的锚表，不由清单提供，边界见清单规格第四节
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    namespace Crypto {
        // Ed25519 公钥长度，字节
        constexpr uint32_t kEd25519PublicKeyBytes = 32;
        // Ed25519 签名长度，字节
        constexpr uint32_t kEd25519SignatureBytes = 64;

        // 验证签名：公钥 32 字节、签名 64 字节、消息任意长度
        // 返回 true 表示签名有效；S 越界、公钥编码非规范或不可解压、等式不成立都返回 false
        bool Ed25519Verify(const uint8_t publicKey[kEd25519PublicKeyBytes], const uint8_t signature[kEd25519SignatureBytes], const uint8_t* message, uint32_t messageBytes);
    }
}
