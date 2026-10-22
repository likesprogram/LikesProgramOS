/* KernelImage.h
    内核镜像的宿主侧视图：结构校验、内容摘要与身份写入，供 PackImage 与 MakeManifest 使用

    布局与固定取值引用目标侧 Packages/Common/Include/KernelImage.hpp 的同一份常量，
    不在这里另写一份；字节表与摘要规则见 Docs/Specs/Baleen/内核镜像数据.md
    BuildId 是构建级身份（内核集身份），由 MakeManifest 按 Docs/Specs/内核集清单与签名.md
    的派生规则算出后经 AssignKernelIdentity 写入；本文件只提供派生所需的输入 C_os 与写入动作
    镜像数据里的 ModuleMajor 是发布线：清单与 .osm 的主版本必须与它相等
*/

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Sha256.h>

namespace hostbuild {
    // BuildId 长度，字节；与内核镜像数据的 BuildId 字段、清单的 BuildId 字段同长
    constexpr std::size_t kBuildIdBytes = 32;

    // 内核镜像的身份状态
    enum class KernelIdentityState {
        Prototype,  // BuildId 与 Digest 都是全零：尚未分配身份的原型，只做结构校验
        Assigned,   // BuildId 非零且 Digest 与镜像内容一致：已分配身份
    };

    // 文件是否为本项目的内核镜像：起点是内核镜像数据且格式标记相符
    bool IsKernelImage(std::span<const uint8_t> image);

    // BuildId 派生规则的输入 C_os：BuildId 与 Digest 两段都按 32 个零代入后的内容摘要
    std::array<uint8_t, kSha256Bytes> ComputeKernelContentId(std::span<const uint8_t> image);

    // 计算内核镜像摘要：Digest 字段按 32 个零代入，不改写镜像
    std::array<uint8_t, kSha256Bytes> ComputeKernelDigest(std::span<const uint8_t> image);

    // 把给定的构建级 BuildId 与对应摘要写进镜像：BuildId 全零或长度不符时报错
    void AssignKernelIdentity(std::vector<uint8_t>& image, std::span<const uint8_t> buildId);

    // 校验内核镜像数据与身份：结构、长度、入口、装载地址与保留字段；原型只校验结构并返回
    // Prototype，已分配身份时复算摘要并核对 Digest；expectedBuildId 非空时还要求与镜像里的
    // BuildId 逐字节相等
    KernelIdentityState VerifyKernelImage(std::span<const uint8_t> image, std::span<const uint8_t> expectedBuildId = {});

    // BuildId 的文本形态：64 个小写十六进制字符
    std::string BuildIdText(std::span<const uint8_t> buildId);

    // 解析 BuildId 文本：要求 64 个十六进制字符，输出 32 字节
    std::array<uint8_t, kBuildIdBytes> ParseBuildIdText(std::string_view text);

    // 镜像里 BuildId 字段的文本形态，供构建输出打印
    std::string KernelBuildIdText(std::span<const uint8_t> image);

    // 镜像数据里的模块主版本（发布线）；结构不合法或为零时报错
    uint16_t KernelModuleMajor(std::span<const uint8_t> image);
}
