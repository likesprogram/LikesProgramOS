/* ImageHeader.cpp
    Baleen 镜像完整性头的公共校验实现：固定字段、入口前缀与整幅摘要

    只回报带编号的原因，不打印也不停机；摘要按覆盖区间把 Digest 字段视作 32 个零
    同一原因在检测阶段各有编号（E-STUB-40.. / E-CORE-40..）：文本两边只写一份，
    编号表按阶段各排一张，序号一致，改动时两张表一起改
*/

#include <ImageHeader.hpp>

namespace Baleen {
    namespace {
        // —— 原因文本：两个阶段的编号表引用同一批常量，避免两边措辞漂移 ——
        constexpr const char* kMagicText = "The image header magic doesn't match";
        constexpr const char* kVersionText = "The image header version or header length isn't supported";
        constexpr const char* kFlagsText = "The image header flags aren't supported";
        constexpr const char* kDigestAlgorithmText = "The image header digest algorithm isn't supported";
        constexpr const char* kBuildIdText = "The image header build id length isn't supported";
        constexpr const char* kReservedText = "The image header reserved bytes aren't zero";
        constexpr const char* kEntryPointText = "The image size doesn't cover the entry point";
        constexpr const char* kMemoryText = "The image memory size doesn't cover the image";
        constexpr const char* kEntryRangeText = "The image entry offset is outside the image";
        constexpr const char* kEntryPrefixText = "The image entry prefix doesn't match the entry offset";
        constexpr const char* kDigestCoverageText = "The image size doesn't cover the digest field";
        constexpr const char* kDigestMismatchText = "The image digest doesn't match";

        // 原因下标：两张编号表与检查顺序都按它排
        enum class Reason : uint32_t {
            Magic = 0,           // 格式标记
            Version,             // 版本或头长
            Flags,               // 可选标志
            DigestAlgorithm,     // 摘要算法
            BuildId,             // BuildId 长度
            Reserved,            // 保留区
            EntryPoint,          // 长度覆盖入口
            Memory,              // 内存跨度覆盖映像
            EntryRange,          // 入口落在映像内
            EntryPrefix,         // 入口前缀与偏移一致
            DigestCoverage,      // 长度覆盖摘要字段
            DigestMismatch,      // 摘要比对
            Count,               // 项数
        };

        // Stub 阶段的编号：40 起，序号与原因下标一一对应
        constexpr Diagnostic kStubReasons[] = {
            {"E-STUB-40", kMagicText},
            {"E-STUB-41", kVersionText},
            {"E-STUB-42", kFlagsText},
            {"E-STUB-43", kDigestAlgorithmText},
            {"E-STUB-44", kBuildIdText},
            {"E-STUB-45", kReservedText},
            {"E-STUB-46", kEntryPointText},
            {"E-STUB-47", kMemoryText},
            {"E-STUB-48", kEntryRangeText},
            {"E-STUB-49", kEntryPrefixText},
            {"E-STUB-50", kDigestCoverageText},
            {"E-STUB-51", kDigestMismatchText},
        };

        // BaleenCore 阶段的编号：序号与 Stub 阶段一致
        constexpr Diagnostic kCoreReasons[] = {
            {"E-CORE-40", kMagicText},
            {"E-CORE-41", kVersionText},
            {"E-CORE-42", kFlagsText},
            {"E-CORE-43", kDigestAlgorithmText},
            {"E-CORE-44", kBuildIdText},
            {"E-CORE-45", kReservedText},
            {"E-CORE-46", kEntryPointText},
            {"E-CORE-47", kMemoryText},
            {"E-CORE-48", kEntryRangeText},
            {"E-CORE-49", kEntryPrefixText},
            {"E-CORE-50", kDigestCoverageText},
            {"E-CORE-51", kDigestMismatchText},
        };

        static_assert(sizeof(kStubReasons) / sizeof(kStubReasons[0]) == static_cast<uint32_t>(Reason::Count), "Stub 阶段的原因表与原因下标数量不符");
        static_assert(sizeof(kCoreReasons) / sizeof(kCoreReasons[0]) == static_cast<uint32_t>(Reason::Count), "BaleenCore 阶段的原因表与原因下标数量不符");

        // 按阶段取编号条目
        Diagnostic Entry(ImageDiagStage stage, Reason reason) {
            const uint32_t index = static_cast<uint32_t>(reason);
            return stage == ImageDiagStage::Stub ? kStubReasons[index] : kCoreReasons[index];
        }

        // 两段字节是否完全相同
        bool Same(const uint8_t* left, const uint8_t* right, uint32_t bytes) {
            for (uint32_t i = 0; i < bytes; ++i) {
                if (left[i] != right[i]) return false;
            }
            return true;
        }
    }

    uint16_t ReadU16(const uint8_t* field) {
        return static_cast<uint16_t>(field[0] | (field[1] << 8));
    }

    Diagnostic CheckImageHeader(const uint8_t* image, const uint8_t magic[8], ImageFacts& facts, ImageDiagStage stage) {
        const ImageHeader* const header = reinterpret_cast<const ImageHeader*>(image + ImageHeaderLayout::kOffset);
        // 格式标记先行：不是本阶段认识的镜像时，后面的字段没有解释意义
        if (!Same(header->magic, magic, sizeof(header->magic))) return Entry(stage, Reason::Magic);
        if (header->version != ImageHeaderLayout::kVersion || header->headerBytes != ImageHeaderLayout::kBytes) return Entry(stage, Reason::Version);
        if (header->flags != ImageHeaderLayout::kFlags) return Entry(stage, Reason::Flags);
        if (header->digestAlgorithm != ImageHeaderLayout::kDigestSha256) return Entry(stage, Reason::DigestAlgorithm);
        if (header->buildIdBytes != ImageHeaderLayout::kBuildIdBytes) return Entry(stage, Reason::BuildId);
        for (uint32_t i = 0; i < ImageHeaderLayout::kReservedBytes; ++i) {
            if (header->reserved[i] != 0) return Entry(stage, Reason::Reserved);
        }

        // 长度与入口：入口必须落在文件的已初始化区域，且与文件起点的近跳转前缀一致
        if (header->imageBytes <= ImageHeaderLayout::kEntryOffset) return Entry(stage, Reason::EntryPoint);
        if (header->memoryBytes < header->imageBytes) return Entry(stage, Reason::Memory);
        const uint32_t entry = header->entryOffset;
        if (entry < ImageHeaderLayout::kEntryOffset || entry >= header->imageBytes) return Entry(stage, Reason::EntryRange);
        if (image[0] != ImageHeaderLayout::kNearJumpOpcode || ReadU16(image + 1) != entry - ImageHeaderLayout::kPrefixBytes) return Entry(stage, Reason::EntryPrefix);

        facts.imageBytes = header->imageBytes;
        facts.memoryBytes = header->memoryBytes;
        facts.entryOffset = entry;
        // BuildId 是不透明字节串：前 4 字节按字节顺序装入这个仅用于诊断的值，
        // 使运行期打印与构建日志里的 BuildId 文本写法一致，不引入小端整数解释
        uint32_t buildId = 0;
        for (uint32_t i = 0; i < 4; ++i) buildId = (buildId << 8) | header->buildId[i];
        facts.buildId = buildId;
        return Diagnostic{};
    }

    Diagnostic CheckImageDigest(const uint8_t* image, uint32_t imageBytes, ImageDiagStage stage) {
        if (imageBytes < ImageHeaderLayout::kDigestFileEnd) return Entry(stage, Reason::DigestCoverage);
        const ImageHeader* const header = reinterpret_cast<const ImageHeader*>(image + ImageHeaderLayout::kOffset);
        const uint8_t zeros[Digest::kSha256Bytes] = {};
        Digest::Sha256 sha;
        sha.Begin();
        sha.Update(image, ImageHeaderLayout::kDigestFileOffset);
        sha.Update(zeros, sizeof(zeros));
        sha.Update(image + ImageHeaderLayout::kDigestFileEnd, imageBytes - ImageHeaderLayout::kDigestFileEnd);
        uint8_t digest[Digest::kSha256Bytes];
        sha.Finish(digest);
        if (!Same(digest, header->digest, sizeof(digest))) return Entry(stage, Reason::DigestMismatch);
        return Diagnostic{};
    }
}
