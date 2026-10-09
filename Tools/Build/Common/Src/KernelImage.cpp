/* KernelImage.cpp
    内核镜像的宿主侧实现：格式标记判定、BuildId 与摘要填充、整幅校验

    字段偏移与固定取值引用目标侧 KernelImage.hpp；摘要计算中 Digest 字段按 32 个零代入，
    不改写镜像里的其他字节
*/

#include <KernelImage.h>

#include <KernelImage.hpp>

#include <algorithm>
#include <stdexcept>

namespace hostbuild {
    namespace {
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

        // 镜像是否容得下内核镜像数据
        bool HoldsData(std::span<const uint8_t> image) {
            return image.size() >= Kernel::ImageDataLayout::kBytes;
        }

        // 起点是否为本项目认识的内核镜像数据的格式标记
        bool HasMagic(std::span<const uint8_t> image) {
            return HoldsData(image) && std::equal(std::begin(Kernel::kImageMagic), std::end(Kernel::kImageMagic), image.begin());
        }
    }

    bool IsKernelImage(std::span<const uint8_t> image) {
        return HasMagic(image);
    }

    std::array<uint8_t, kSha256Bytes> ComputeKernelDigest(std::span<const uint8_t> image) {
        using Layout = Kernel::ImageDataLayout;
        Require(image.size() >= Layout::kDigestFileEnd, "镜像短于内核镜像数据的摘要覆盖区间");
        const uint8_t zeros[kSha256Bytes] = {};
        Sha256 sha;
        sha.Begin();
        sha.Update(image.subspan(0, Layout::kDigestFileOffset));
        sha.Update(std::span<const uint8_t>(zeros, sizeof(zeros)));
        sha.Update(image.subspan(Layout::kDigestFileEnd));
        return sha.Finish();
    }

    void FillKernelIdentity(std::vector<uint8_t>& image) {
        using Layout = Kernel::ImageDataLayout;
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        uint8_t* const data = image.data();
        // BuildId 是这一份内容的构建标识：把它与 Digest 一起归零后再算，不会出现自引用
        std::fill_n(data + Layout::kFieldBuildId, Layout::kBuildIdBytes, 0);
        std::fill_n(data + Layout::kFieldDigest, kSha256Bytes, 0);
        const std::array<uint8_t, kSha256Bytes> buildId = ComputeKernelDigest(image);
        std::copy(buildId.begin(), buildId.end(), data + Layout::kFieldBuildId);
        // 摘要覆盖 BuildId，计算时只把 Digest 本身归零
        const std::array<uint8_t, kSha256Bytes> digest = ComputeKernelDigest(image);
        std::copy(digest.begin(), digest.end(), data + Layout::kFieldDigest);
    }

    void VerifyKernelImage(std::span<const uint8_t> image) {
        using Layout = Kernel::ImageDataLayout;
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        Require(HasMagic(image), "内核镜像数据的格式标记不匹配");
        const uint8_t* const data = image.data();
        Require(Get16(data + Layout::kFieldVersion) == Layout::kVersion
                && Get16(data + Layout::kFieldDataBytes) == Layout::kBytes, "内核镜像数据的版本或数据长度不受支持");
        Require(Get32(data + Layout::kFieldFlags) == Layout::kFlags, "内核镜像数据的标志位不受支持");
        Require(Get16(data + Layout::kFieldDigestAlgorithm) == Layout::kDigestSha256, "内核镜像数据的摘要算法不受支持");
        Require(Get16(data + Layout::kFieldBuildIdBytes) == Layout::kBuildIdBytes, "内核镜像数据的 BuildId 长度不受支持");
        for (uint32_t i = 0; i < Layout::kReservedBytes; ++i) {
            Require(data[Layout::kFieldReserved + i] == 0, "内核镜像数据的保留字节不是全零");
        }

        // 内核事实：长度必须与实际文件一致，内存跨度覆盖文件，入口落在数据之后，装载地址对齐
        const uint64_t imageBytes = Get32(data + Layout::kFieldImageBytes);
        const uint64_t memoryBytes = Get32(data + Layout::kFieldMemoryBytes);
        const uint64_t entry = Get32(data + Layout::kFieldEntryOffset);
        const uint64_t load = Get32(data + Layout::kFieldLoadAddress);
        Require(imageBytes == image.size(), "内核镜像数据的 ImageBytes 与实际文件长度不一致");
        Require(memoryBytes >= imageBytes, "内核镜像数据的 MemoryBytes 没有覆盖文件");
        Require(entry >= Layout::kBytes && entry < imageBytes, "内核镜像数据的入口偏移落在数据之前或文件之外");
        Require(load >= Layout::kLoadMin && (load % Layout::kLoadAlign) == 0, "内核镜像数据的装载地址过低或未按 2MiB 对齐");
        Require(Get32(data + Layout::kFieldBootInfoVersion) != 0, "内核镜像数据没有声明 BootInfo 版本");

        const std::array<uint8_t, kSha256Bytes> digest = ComputeKernelDigest(image);
        Require(std::equal(digest.begin(), digest.end(), data + Layout::kFieldDigest), "内核镜像摘要不匹配");
    }

    std::string KernelBuildIdText(std::span<const uint8_t> image) {
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        const uint8_t* const buildId = image.data() + Kernel::ImageDataLayout::kFieldBuildId;
        std::string text;
        for (uint32_t i = 0; i < 4; ++i) {
            constexpr char kDigits[] = "0123456789ABCDEF";
            text += kDigits[buildId[i] >> 4];
            text += kDigits[buildId[i] & 0xF];
        }
        return text;
    }
}
