/* TrustAnchor.hpp
    引导器内嵌的信任锚：内核集清单验签用的公钥表条目

    形态与上限见 Docs/Specs/内核集清单与签名.md 第四节：条目 = KeyId（1 字节）+ Ed25519 公钥（32 字节），
    表最多 2 条、覆盖轮换窗口；表随引导器一起构建、只读，清单只带 KeyId，公钥不由清单提供
*/

#pragma once
#include <stdint.h>

#include <Ed25519.hpp>

namespace Baleen {
    namespace Crypto {
        // 信任锚表最大条目数：轮换窗口两把并存，另留吊销授权与轮换余量
        constexpr uint32_t kTrustAnchorMaxCount = 4;

        // 信任锚条目：编号与 Ed25519 公钥
        struct TrustAnchor {
            uint8_t keyId;                              // 密钥编号，取值 1..255
            uint8_t publicKey[kEd25519PublicKeyBytes];  // Ed25519 公钥
        };

        // 按 KeyId 查找公钥：找不到返回空指针，调用方按“密钥不存在”拒绝
        const uint8_t* FindTrustAnchor(uint8_t keyId);
    }
}
