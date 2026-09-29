/* BaleenImage.cpp
    Baleen 镜像的宿主侧实现：类别判定、BuildId 与摘要填充、组装门禁

    Digest 字段在摘要计算中按 32 个零代入，从不改写镜像里的其他字节
*/

#include <BaleenImage.h>

#include <HostIo.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace makeiso {
    namespace {
        // 两种镜像的格式标记，含终止零，与目标侧 ImageHeader.hpp 一致
        constexpr uint8_t kStubMagic[8] = { 'B', 'L', 'N', 'S', 'T', 'U', 'B', 0 };
        constexpr uint8_t kCoreMagic[8] = { 'B', 'L', 'N', 'C', 'O', 'R', 'E', 0 };
        // 占位内容标识：MakePayloads 生成的开发占位载荷
        constexpr std::string_view kStubPlaceholder = "PLACEHOLDER-STUB";
        constexpr std::string_view kCorePlaceholder = "PLACEHOLDER-CORE-IMAGE";

        // 条件不成立即抛出异常
        void Require(bool condition, const std::string& message) {
            if (!condition) throw std::runtime_error(message);
        }

        // 按小端读取 16 位
        uint16_t Get16(const uint8_t* field) {
            return static_cast<uint16_t>(field[0] | (field[1] << 8));
        }

        // 按小端读取 32 位
        uint32_t Get32(const uint8_t* field) {
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
            return value;
        }

        // 镜像是否容得下完整性头
        bool HoldsHeader(std::span<const uint8_t> image) {
            return image.size() >= ImageHeaderLayout::kOffset + ImageHeaderLayout::kBytes;
        }

        // 头里的格式标记是否等于给定标记
        bool HasMagic(std::span<const uint8_t> image, const uint8_t (&magic)[8]) {
            return HoldsHeader(image) && std::equal(std::begin(magic), std::end(magic), image.begin() + ImageHeaderLayout::kOffset);
        }

        // 内容里是否含占位标识
        bool HasMarker(std::span<const uint8_t> image, std::string_view marker) {
            const std::string_view bytes(reinterpret_cast<const char*>(image.data()), image.size());
            return bytes.find(marker) != std::string_view::npos;
        }

        // 每类镜像的校验约束
        struct Limits {
            uint32_t maxImageBytes;   // 文件字节数上限
            uint32_t maxMemoryBytes;  // 静态内存跨度上限
            uint32_t loadTop;         // 静态内存的装入起点
            uint32_t windowEnd;       // 窗口上端，不含；0 表示不检查窗口
        };

        // 取类别的约束；Stub 文件在两种入口间共用，按更严的 CD 上限 0x8000 检查
        Limits LimitsOf(ImageKind kind) {
            if (kind == ImageKind::Stub) return Limits{0x8000, 0x8200, 0x7E00, 0x10000};
            return Limits{0x400000, 0x400000, 0, 0};
        }
    }

    const char* ImageKindName(ImageKind kind) {
        switch (kind) {
            case ImageKind::Stub: return "Stub";
            case ImageKind::Core: return "Core";
            case ImageKind::Unknown: break;
        }
        return "未知";
    }

    ImageClass ClassifyImage(std::span<const uint8_t> image) {
        ImageClass result;
        if (HasMagic(image, kStubMagic)) {
            result.kind = ImageKind::Stub;
            result.header = true;
            result.placeholder = HasMarker(image, kStubPlaceholder);
            return result;
        }
        if (HasMagic(image, kCoreMagic)) {
            result.kind = ImageKind::Core;
            result.header = true;
            result.placeholder = HasMarker(image, kCorePlaceholder);
            return result;
        }
        // 没有可识别头：带占位标识的按对应类别的开发占位件处理，其余拒绝
        if (HasMarker(image, kStubPlaceholder)) {
            result.kind = ImageKind::Stub;
            result.placeholder = true;
            result.reason = "没有 BaleenStub 完整性头的格式标记";
            return result;
        }
        if (HasMarker(image, kCorePlaceholder)) {
            result.kind = ImageKind::Core;
            result.placeholder = true;
            result.reason = "没有 BaleenCore 完整性头的格式标记";
            return result;
        }
        result.reason = "既没有可识别的完整性头格式标记，也没有 MakePayloads 的占位标识";
        return result;
    }

    std::array<uint8_t, kSha256Bytes> ComputeImageDigest(std::span<const uint8_t> image) {
        Require(image.size() >= ImageHeaderLayout::kDigestFileEnd, "镜像短于摘要覆盖区间");
        const uint8_t zeros[kSha256Bytes] = {};
        Sha256 sha;
        sha.Begin();
        sha.Update(image.subspan(0, ImageHeaderLayout::kDigestFileOffset));
        sha.Update(std::span<const uint8_t>(zeros, sizeof(zeros)));
        sha.Update(image.subspan(ImageHeaderLayout::kDigestFileEnd));
        return sha.Finish();
    }

    void FillImageIdentity(std::vector<uint8_t>& image) {
        Require(HoldsHeader(image), "镜像放不下完整性头");
        uint8_t* const header = image.data() + ImageHeaderLayout::kOffset;
        // BuildId 是这一份内容的构建标识：把它与 Digest 一起归零后再算，不会出现自引用
        std::fill_n(header + ImageHeaderLayout::kFieldBuildId, ImageHeaderLayout::kBuildIdBytes, 0);
        std::fill_n(header + ImageHeaderLayout::kFieldDigest, kSha256Bytes, 0);
        const std::array<uint8_t, kSha256Bytes> buildId = ComputeImageDigest(image);
        std::copy(buildId.begin(), buildId.end(), header + ImageHeaderLayout::kFieldBuildId);
        // 摘要覆盖 BuildId，计算时只把 Digest 本身归零
        const std::array<uint8_t, kSha256Bytes> digest = ComputeImageDigest(image);
        std::copy(digest.begin(), digest.end(), header + ImageHeaderLayout::kFieldDigest);
    }

    void VerifyImage(std::span<const uint8_t> image, ImageKind kind) {
        Require(kind != ImageKind::Unknown, "镜像类别未知，无法校验");
        Require(HoldsHeader(image), "镜像放不下完整性头");
        const Limits limits = LimitsOf(kind);
        const uint8_t* const header = image.data() + ImageHeaderLayout::kOffset;
        const uint8_t(&magic)[8] = kind == ImageKind::Stub ? kStubMagic : kCoreMagic;
        Require(std::equal(std::begin(magic), std::end(magic), header), "镜像头的格式标记不匹配");
        Require(Get16(header + ImageHeaderLayout::kFieldVersion) == ImageHeaderLayout::kVersion
                && Get16(header + ImageHeaderLayout::kFieldHeaderBytes) == ImageHeaderLayout::kBytes, "镜像头的版本或头长不受支持");
        Require(Get32(header + ImageHeaderLayout::kFieldFlags) == ImageHeaderLayout::kFlags, "镜像头的标志位不受支持");
        Require(Get16(header + ImageHeaderLayout::kFieldDigestAlgorithm) == ImageHeaderLayout::kDigestSha256, "镜像头的摘要算法不受支持");
        Require(Get16(header + ImageHeaderLayout::kFieldBuildIdBytes) == ImageHeaderLayout::kBuildIdBytes, "镜像头的 BuildId 长度不受支持");
        for (uint32_t i = 0; i < ImageHeaderLayout::kReservedBytes; ++i) {
            Require(header[ImageHeaderLayout::kFieldReserved + i] == 0, "镜像头的保留字节不是全零");
        }

        // 长度必须与实际文件、类别上限一致；静态内存跨度必须覆盖文件并落在类别约束内
        const uint64_t imageBytes = Get32(header + ImageHeaderLayout::kFieldImageBytes);
        const uint64_t memoryBytes = Get32(header + ImageHeaderLayout::kFieldMemoryBytes);
        const uint64_t entry = Get32(header + ImageHeaderLayout::kFieldEntryOffset);
        Require(imageBytes == image.size(), "镜像头的 ImageBytes 与实际文件长度不一致");
        Require(imageBytes <= limits.maxImageBytes, "镜像文件超出该类别的长度上限");
        Require(memoryBytes >= imageBytes && memoryBytes <= limits.maxMemoryBytes, "镜像头的静态内存跨度越界");
        Require(entry >= ImageHeaderLayout::kEntryOffset && entry < imageBytes, "镜像头的入口偏移落在文件之外");
        Require(image[0] == ImageHeaderLayout::kNearJumpOpcode && Get16(image.data() + 1) == entry - ImageHeaderLayout::kPrefixBytes, "镜像入口前缀与头里的入口偏移不一致");
        if (limits.windowEnd != 0) Require(limits.loadTop + memoryBytes <= limits.windowEnd, "Stub 的静态内存跨度越出低 64KiB 窗口");

        const std::array<uint8_t, kSha256Bytes> digest = ComputeImageDigest(image);
        Require(std::equal(digest.begin(), digest.end(), header + ImageHeaderLayout::kFieldDigest), "镜像摘要不匹配");
    }

    std::string ImageBuildIdText(std::span<const uint8_t> image) {
        Require(HoldsHeader(image), "镜像放不下完整性头");
        const uint8_t* const buildId = image.data() + ImageHeaderLayout::kOffset + ImageHeaderLayout::kFieldBuildId;
        std::string text;
        for (uint32_t i = 0; i < 4; ++i) {
            constexpr char kDigits[] = "0123456789ABCDEF";
            text += kDigits[buildId[i] >> 4];
            text += kDigits[buildId[i] & 0xF];
        }
        return text;
    }

    PayloadInfo CheckPayload(const std::string& path, ImageKind expected, bool allowUnchecked, const std::string& uncheckedHint) {
        const std::vector<uint8_t> image = ReadFile(path);
        const ImageClass cls = ClassifyImage(image);
        const std::string label = ImageKindName(expected);
        PayloadInfo info;
        info.kind = cls.kind;
        info.placeholder = cls.placeholder;

        // 带头载荷：类别不符即拒绝，其余做完整校验并回报 BuildId
        if (cls.header) {
            if (cls.kind != expected) throw std::runtime_error(label + " 载荷装的是 " + ImageKindName(cls.kind) + " 镜像：" + path);
            VerifyImage(image, cls.kind);
            info.buildId = ImageBuildIdText(image);
            if (cls.placeholder) std::cerr << "  " << label << " 载荷是开发占位内容（头与摘要已校验）：" << path << '\n';
            return info;
        }
        // 没有头：占位内容放行并提示；其余载荷按调用方要求免除校验，默认拒绝
        if (cls.kind == expected) {
            std::cerr << "  " << label << " 载荷是开发占位内容，没有完整性头，未校验：" << path << '\n';
            return info;
        }
        if (allowUnchecked) {
            std::cerr << "  " << label << " 载荷按调用方要求免除完整性头与摘要校验（" << cls.reason << "）：" << path << '\n';
            return info;
        }
        throw std::runtime_error(label + " 载荷无法识别（" + cls.reason + "）：" + path + "；用 PackImage 打包，或确认占位件标记，或显式 " + uncheckedHint);
    }

    std::string PayloadSummary(const PayloadInfo& info) {
        if (!info.buildId.empty()) {
            return "头与摘要已校验，BuildId " + info.buildId + (info.placeholder ? "（开发占位内容）" : "");
        }
        if (info.placeholder) return "开发占位内容，没有完整性头，未校验";
        return "已按调用方要求免除完整性头与摘要校验";
    }
}
