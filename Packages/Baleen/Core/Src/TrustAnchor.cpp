/* TrustAnchor.cpp
    信任锚查找：表内容由构建期生成的 TrustAnchors.cpp 提供

    生成物由 Tools/Bin/MakeKeys 从开发期或发布密钥导出，定义 kTrustAnchors 与 kTrustAnchorCount；
    本文件只做按 KeyId 的查找，不改表内容
*/

#include <TrustAnchor.hpp>

namespace Baleen {
    namespace Crypto {
        // 表内容：构建期生成物提供，带外部链接
        extern const TrustAnchor kTrustAnchors[];
        // 表内条目数：构建期生成物提供
        extern const uint32_t kTrustAnchorCount;

        const uint8_t* FindTrustAnchor(uint8_t keyId) {
            // 表长度不超过容量由生成物的 static_assert 保证，这里不截断、不静默丢条目
            for (uint32_t i = 0; i < kTrustAnchorCount; ++i) {
                if (kTrustAnchors[i].keyId == keyId) return kTrustAnchors[i].publicKey;
            }
            return nullptr;
        }
    }
}
