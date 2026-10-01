/* BaleenImage.h
    Baleen 镜像的宿主侧视图：完整性头的字段常量、类别判定、身份填充、校验与组装门禁

    Stub 与 Core 的镜像头同构：布局、固定取值与摘要规则完全一致，只有格式标记不同
    （Stub 为 BLNSTUB、Core 为 BLNCORE）；同一套实现供 PackImage 与两个组装器共用
    字段与 Packages/Baleen/Common/Include/ImageHeader.hpp、Stub 的 StubHeader.inc 保持一致，
    改动须同时同步；摘要中 Digest 字段按 32 个零代入，见 Stub 说明第四节
*/

#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <Sha256.h>

namespace hostbuild {
    // 完整性头的固定布局与取值，与目标侧 ImageHeaderLayout 一致
    struct ImageHeaderLayout {
        static constexpr uint32_t kOffset = 0x10;              // 头相对文件起点的偏移
        static constexpr uint32_t kBytes = 0x80;               // 头固定长度
        static constexpr uint32_t kEntryOffset = 0x90;         // 早期入口的文件偏移
        static constexpr uint32_t kPrefixBytes = 3;            // 文件起点的 16 位近跳转前缀
        static constexpr uint8_t kNearJumpOpcode = 0xE9;       // 近跳转操作码
        static constexpr uint16_t kVersion = 1;                // 未发布的开发格式标记
        static constexpr uint32_t kFlags = 0;                  // 当前不定义可选标志
        static constexpr uint16_t kDigestSha256 = 1;           // 摘要算法编号
        static constexpr uint16_t kBuildIdBytes = 32;          // BuildId 长度
        static constexpr uint32_t kFieldMagic = 0x00;          // 头内字段偏移
        static constexpr uint32_t kFieldVersion = 0x08;        // 头内字段偏移
        static constexpr uint32_t kFieldHeaderBytes = 0x0A;
        static constexpr uint32_t kFieldFlags = 0x0C;
        static constexpr uint32_t kFieldImageBytes = 0x10;
        static constexpr uint32_t kFieldMemoryBytes = 0x14;
        static constexpr uint32_t kFieldEntryOffset = 0x18;
        static constexpr uint32_t kFieldDigestAlgorithm = 0x1C;
        static constexpr uint32_t kFieldBuildIdBytes = 0x1E;
        static constexpr uint32_t kFieldBuildId = 0x20;
        static constexpr uint32_t kFieldDigest = 0x40;
        static constexpr uint32_t kFieldReserved = 0x60;
        static constexpr uint32_t kReservedBytes = 32;         // 头内保留字段长度
        // 摘要覆盖范围内按零代入的区间，相对文件起点
        static constexpr uint32_t kDigestFileOffset = kOffset + kFieldDigest;          // 0x50
        static constexpr uint32_t kDigestFileEnd = kDigestFileOffset + kSha256Bytes;   // 0x70
    };

    // 镜像类别：带完整性头的 Stub 或 Core，或无法识别
    enum class ImageKind { Stub, Core, Unknown };

    // 类别的名字，供诊断与打印；Unknown 返回“未知”
    const char* ImageKindName(ImageKind kind);

    // 类别判定结果：头标记与占位标识各自独立，占位内容也可能带完整头
    struct ImageClass {
        ImageKind kind = ImageKind::Unknown;   // 按头标记或占位标识判定的类别
        bool header = false;                   // 是否带可识别的完整性头
        bool placeholder = false;              // 内容里是否含 MakePayloads 的占位标识
        std::string reason;                    // 无法识别时的原因
    };

    // 判定镜像类别：先看头里的格式标记，再看内容里的占位标识
    ImageClass ClassifyImage(std::span<const uint8_t> image);

    // 计算镜像摘要：Digest 字段按 32 个零代入，不改写镜像
    std::array<uint8_t, kSha256Bytes> ComputeImageDigest(std::span<const uint8_t> image);

    // 填 BuildId 与 Digest：BuildId 由两者均归零的内容派生，Digest 再覆盖 BuildId
    void FillImageIdentity(std::vector<uint8_t>& image);

    // 校验带头镜像的固定字段、长度、入口前缀与整幅摘要；任一项不通过即抛异常
    // 长度上限与静态内存落点按类别取：Stub 受低 64KiB 窗口约束，Core 按自己的上限
    void VerifyImage(std::span<const uint8_t> image, ImageKind kind);

    // 带头镜像的 BuildId 前 4 字节，按字节顺序格式化为 8 位大写十六进制，供构建输出打印使用
    std::string ImageBuildIdText(std::span<const uint8_t> image);

    // 组装用载荷门禁结果：占位内容没有可核对的来源，免除校验的载荷也没有
    struct PayloadInfo {
        ImageKind kind = ImageKind::Unknown;   // 载荷类别
        bool placeholder = false;              // 是否为开发占位内容
        std::string buildId;                   // 带头载荷的 BuildId 前 4 字节，8 位大写十六进制
    };

    // 组装用载荷门禁：读文件、按 expected 分类并做完整校验
    // 带头的载荷必做校验；占位内容放行并在 stderr 说明；无法识别或类别不符的载荷默认拒绝
    // allowUnchecked 为真时，无法识别的载荷也放行（供测试夹具与特殊用途显式免除校验），uncheckedHint 是提示用的开关名
    PayloadInfo CheckPayload(const std::string& path, ImageKind expected, bool allowUnchecked, const std::string& uncheckedHint);

    // 载荷校验结果的一行摘要，供组装器打印
    std::string PayloadSummary(const PayloadInfo& info);
}
