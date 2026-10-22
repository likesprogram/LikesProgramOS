/* KernelImage.cpp
    内核镜像的宿主侧实现：格式标记判定、内容摘要与摘要计算、身份写入与整幅校验

    字段偏移与固定取值引用目标侧 KernelImage.hpp；摘要计算中 Digest 字段按 32 个零代入，
    派生 BuildId 的 C_os 则把 BuildId 与 Digest 两段一起按零代入，都不改写镜像里的其他字节
*/

#include <KernelImage.h>

#include <KernelImage.hpp>

#include <algorithm>
#include <stdexcept>

namespace hostbuild {
    static_assert(kBuildIdBytes == Kernel::ImageDataLayout::kBuildIdBytes, "BuildId 长度必须与内核镜像数据一致");

    namespace {
        // 全零字节块：派生 C_os 时代入 BuildId 与 Digest 两段
        constexpr std::array<uint8_t, Kernel::ImageDataLayout::kBuildIdBytes + kSha256Bytes> kZeroIdentityFields{};

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

        // 一段字节是否全零
        bool IsZero(std::span<const uint8_t> bytes) {
            return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
        }

        // 一个十六进制字符的取值；不是十六进制返回 -1
        int HexValue(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // 镜像是否容得下内核镜像数据
        bool HoldsData(std::span<const uint8_t> image) {
            return image.size() >= Kernel::ImageDataLayout::kBytes;
        }

        // 起点是否为本项目认识的内核镜像数据的格式标记
        bool HasMagic(std::span<const uint8_t> image) {
            return HoldsData(image) && std::equal(std::begin(Kernel::kImageMagic), std::end(Kernel::kImageMagic), image.begin());
        }

        // 镜像里的 BuildId 字段
        std::span<const uint8_t> BuildIdField(std::span<const uint8_t> image) {
            return std::span<const uint8_t>(image.data() + Kernel::ImageDataLayout::kFieldBuildId, Kernel::ImageDataLayout::kBuildIdBytes);
        }

        // 镜像里的 Digest 字段
        std::span<const uint8_t> DigestField(std::span<const uint8_t> image) {
            return std::span<const uint8_t>(image.data() + Kernel::ImageDataLayout::kFieldDigest, kSha256Bytes);
        }
    }

    bool IsKernelImage(std::span<const uint8_t> image) {
        return HasMagic(image);
    }

    std::array<uint8_t, kSha256Bytes> ComputeKernelContentId(std::span<const uint8_t> image) {
        using Layout = Kernel::ImageDataLayout;
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        Sha256 sha;
        sha.Begin();
        sha.Update(image.subspan(0, Layout::kFieldBuildId));
        sha.Update(std::span<const uint8_t>(kZeroIdentityFields.data(), kZeroIdentityFields.size()));
        sha.Update(image.subspan(Layout::kDigestFileEnd));
        return sha.Finish();
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

    void AssignKernelIdentity(std::vector<uint8_t>& image, std::span<const uint8_t> buildId) {
        using Layout = Kernel::ImageDataLayout;
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        Require(buildId.size() == Layout::kBuildIdBytes, "BuildId 长度必须是 32 字节");
        Require(!IsZero(buildId), "BuildId 全零，禁止写入内核镜像");
        uint8_t* const data = image.data();
        std::copy(buildId.begin(), buildId.end(), data + Layout::kFieldBuildId);
        // 摘要只把 Digest 本身归零：已填入的 BuildId 在覆盖范围内
        std::fill_n(data + Layout::kFieldDigest, kSha256Bytes, 0);
        const std::array<uint8_t, kSha256Bytes> digest = ComputeKernelDigest(image);
        std::copy(digest.begin(), digest.end(), data + Layout::kFieldDigest);
    }

    KernelIdentityState VerifyKernelImage(std::span<const uint8_t> image, std::span<const uint8_t> expectedBuildId) {
        using Layout = Kernel::ImageDataLayout;
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        Require(HasMagic(image), "内核镜像数据的格式标记不匹配");
        const uint8_t* const data = image.data();
        Require(Get16(data + Layout::kFieldVersion) == Layout::kVersion
                && Get16(data + Layout::kFieldDataBytes) == Layout::kBytes, "内核镜像数据的版本或数据长度不受支持");
        Require(Get32(data + Layout::kFieldFlags) == Layout::kFlags, "内核镜像数据的标志位不受支持");
        Require(Get16(data + Layout::kFieldDigestAlgorithm) == Layout::kDigestSha256, "内核镜像数据的摘要算法不受支持");
        Require(Get16(data + Layout::kFieldBuildIdBytes) == Layout::kBuildIdBytes, "内核镜像数据的 BuildId 长度不受支持");
        Require(Get16(data + Layout::kFieldModuleMajor) != 0, "内核镜像数据的模块主版本为零");
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

        // 身份两段必须同时为零（未分配的原型）或同时非零（已分配）；期望值先核对，原型对不上非零期望
        const std::span<const uint8_t> buildId = BuildIdField(image);
        const std::span<const uint8_t> digest = DigestField(image);
        const bool buildIdZero = IsZero(buildId);
        Require(buildIdZero == IsZero(digest), "内核镜像的 BuildId 与 Digest 必须同时为零或同时非零");
        Require(expectedBuildId.empty() || expectedBuildId.size() == Layout::kBuildIdBytes, "期望的 BuildId 长度必须是 32 字节");
        if (!expectedBuildId.empty()) {
            Require(std::equal(expectedBuildId.begin(), expectedBuildId.end(), buildId.begin()), "内核镜像的 BuildId 与期望值不一致");
        }
        // 原型没有可核对的内容摘要：结构合法即通过，身份如何分配由清单负责
        if (buildIdZero) return KernelIdentityState::Prototype;
        const std::array<uint8_t, kSha256Bytes> actual = ComputeKernelDigest(image);
        Require(std::equal(actual.begin(), actual.end(), digest.begin()), "内核镜像摘要不匹配");
        return KernelIdentityState::Assigned;
    }

    std::string BuildIdText(std::span<const uint8_t> buildId) {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(buildId.size() * 2);
        for (uint8_t byte : buildId) {
            text.push_back(kDigits[byte >> 4]);
            text.push_back(kDigits[byte & 0x0F]);
        }
        return text;
    }

    std::array<uint8_t, kBuildIdBytes> ParseBuildIdText(std::string_view text) {
        if (text.size() != kBuildIdBytes * 2) throw std::runtime_error("BuildId 文本必须是 64 个十六进制字符：" + std::string(text));
        std::array<uint8_t, kBuildIdBytes> buildId{};
        for (std::size_t i = 0; i < buildId.size(); ++i) {
            const int high = HexValue(text[2 * i]);
            const int low = HexValue(text[2 * i + 1]);
            if (high < 0 || low < 0) throw std::runtime_error("BuildId 文本含非十六进制字符：" + std::string(text));
            buildId[i] = static_cast<uint8_t>((high << 4) | low);
        }
        return buildId;
    }

    std::string KernelBuildIdText(std::span<const uint8_t> image) {
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        return BuildIdText(BuildIdField(image));
    }

    uint16_t KernelModuleMajor(std::span<const uint8_t> image) {
        Require(HoldsData(image), "镜像放不下内核镜像数据");
        return Get16(image.data() + Kernel::ImageDataLayout::kFieldModuleMajor);
    }
}
