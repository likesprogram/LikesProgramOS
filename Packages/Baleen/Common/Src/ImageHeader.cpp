/* ImageHeader.cpp
    Baleen 镜像完整性头的公共校验实现：固定字段、入口前缀与整幅摘要

    只回报原因文本，不打印也不停机；摘要按覆盖区间把 Digest 字段视作 32 个零
*/

#include <ImageHeader.hpp>

namespace Baleen {
    namespace {
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

    uint32_t ReadU32(const uint8_t* field) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
        return value;
    }

    const char* CheckImageHeader(const uint8_t* image, const uint8_t magic[8], ImageFacts& facts) {
        const ImageHeader* const header = reinterpret_cast<const ImageHeader*>(image + ImageHeaderLayout::kOffset);
        // 格式标记先行：不是本阶段认识的镜像时，后面的字段没有解释意义
        if (!Same(header->magic, magic, sizeof(header->magic))) return "The image header magic doesn't match";
        if (header->version != ImageHeaderLayout::kVersion || header->headerBytes != ImageHeaderLayout::kBytes) return "The image header version or header length isn't supported";
        if (header->flags != ImageHeaderLayout::kFlags) return "The image header flags aren't supported";
        if (header->digestAlgorithm != ImageHeaderLayout::kDigestSha256) return "The image header digest algorithm isn't supported";
        if (header->buildIdBytes != ImageHeaderLayout::kBuildIdBytes) return "The image header build id length isn't supported";
        for (uint32_t i = 0; i < ImageHeaderLayout::kReservedBytes; ++i) {
            if (header->reserved[i] != 0) return "The image header reserved bytes aren't zero";
        }

        // 长度与入口：入口必须落在文件的已初始化区域，且与文件起点的近跳转前缀一致
        if (header->imageBytes <= ImageHeaderLayout::kEntryOffset) return "The image size doesn't cover the entry point";
        if (header->memoryBytes < header->imageBytes) return "The image memory size doesn't cover the image";
        const uint32_t entry = header->entryOffset;
        if (entry < ImageHeaderLayout::kEntryOffset || entry >= header->imageBytes) return "The image entry offset is outside the image";
        if (image[0] != ImageHeaderLayout::kNearJumpOpcode || ReadU16(image + 1) != entry - ImageHeaderLayout::kPrefixBytes) return "The image entry prefix doesn't match the entry offset";

        facts.imageBytes = header->imageBytes;
        facts.memoryBytes = header->memoryBytes;
        facts.entryOffset = entry;
        facts.buildId = ReadU32(header->buildId);
        return nullptr;
    }

    const char* CheckImageDigest(const uint8_t* image, uint32_t imageBytes) {
        if (imageBytes < ImageHeaderLayout::kDigestFileEnd) return "The image size doesn't cover the digest field";
        const ImageHeader* const header = reinterpret_cast<const ImageHeader*>(image + ImageHeaderLayout::kOffset);
        const uint8_t zeros[Digest::kSha256Bytes] = {};
        Digest::Sha256 sha;
        sha.Begin();
        sha.Update(image, ImageHeaderLayout::kDigestFileOffset);
        sha.Update(zeros, sizeof(zeros));
        sha.Update(image + ImageHeaderLayout::kDigestFileEnd, imageBytes - ImageHeaderLayout::kDigestFileEnd);
        uint8_t digest[Digest::kSha256Bytes];
        sha.Finish(digest);
        if (!Same(digest, header->digest, sizeof(digest))) return "The image digest doesn't match";
        return nullptr;
    }
}
