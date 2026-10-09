/* KernelImage.h
    内核镜像的宿主侧视图：内核镜像数据的填充与校验，供 PackImage 使用

    布局与固定取值引用目标侧 Packages/Common/Include/KernelImage.hpp 的同一份常量，
    不在这里另写一份；摘要规则与 Baleen 镜像一致：Digest 字段在覆盖范围内按 32 个零代入
    字节表见 Docs/Specs/Baleen/内核镜像数据.md
*/

#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <Sha256.h>

namespace hostbuild {
    // 文件是否为本项目的内核镜像：起点是内核镜像数据且格式标记相符
    bool IsKernelImage(std::span<const uint8_t> image);

    // 计算内核镜像摘要：Digest 字段按 32 个零代入，不改写镜像
    std::array<uint8_t, kSha256Bytes> ComputeKernelDigest(std::span<const uint8_t> image);

    // 填 BuildId 与 Digest：BuildId 由两者均归零的内容派生，Digest 再覆盖 BuildId
    void FillKernelIdentity(std::vector<uint8_t>& image);

    // 校验内核镜像数据与整幅摘要；任一项不通过即抛异常
    void VerifyKernelImage(std::span<const uint8_t> image);

    // BuildId 前 4 字节，按字节顺序格式化为 8 位大写十六进制，供构建输出打印使用
    std::string KernelBuildIdText(std::span<const uint8_t> image);
}
