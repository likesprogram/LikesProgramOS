/* BaleenLayout.cpp
    Baleen 布局描述的宿主侧编码实现：Ext4 卷标识读取、模板解析与字段回填

    字节表与校验规则的单点定义是 Docs/Specs/Baleen/布局描述.md；本文件不改变格式，
    只按规格生成字节；保留字段一律写 0，校验和覆盖 [0x000, 0x140)
*/

#include <BaleenLayout.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <HostIo.h>

namespace hostbuild {
    namespace {
        // 头部字段的固定取值与字段偏移：引用目标侧同源的 LayoutDescriptor.hpp，不在这里另写一份
        constexpr uint32_t kMagic = Baleen::LayoutDescriptor::kMagic;                   // 'BLAY'，布局描述的格式标记
        constexpr uint16_t kVersion = Baleen::LayoutDescriptor::kVersion;
        constexpr uint16_t kHeaderBytes = Baleen::LayoutDescriptor::kHeaderBytes;
        constexpr uint32_t kLayoutBytes = Baleen::LayoutDescriptor::kLayoutBytes;
        constexpr std::size_t kFieldUuid = Baleen::LayoutDescriptor::kFieldVolumeUuid;
        constexpr std::size_t kFieldVolumeOffset = Baleen::LayoutDescriptor::kFieldVolumeOffset;
        constexpr std::size_t kFieldVolumeBytes = Baleen::LayoutDescriptor::kFieldVolumeBytes;
        constexpr std::size_t kFieldStateOffset = Baleen::LayoutDescriptor::kFieldStateOffset;
        constexpr std::size_t kFieldStateBytes = Baleen::LayoutDescriptor::kFieldStateBytes;
        constexpr std::size_t kFieldPathBytes = Baleen::LayoutDescriptor::kFieldKernelPathBytes;
        constexpr std::size_t kFieldPath = Baleen::LayoutDescriptor::kFieldKernelPath;
        constexpr std::size_t kFieldChecksum = Baleen::LayoutDescriptor::kFieldChecksum;
        constexpr std::size_t kPathMaxBytes = Baleen::LayoutDescriptor::kKernelPathMaxBytes;
        // Ext4 superblock 的位置与字段
        constexpr std::size_t kExt4SuperOffset = 1024;     // superblock 在卷内的字节偏移
        constexpr std::size_t kExt4SuperBytes = 1024;      // superblock 长度
        constexpr std::size_t kExt4MagicOffset = 0x38;     // superblock 内 s_magic 的偏移
        constexpr uint16_t kExt4Magic = 0xEF53;            // s_magic 的取值
        constexpr std::size_t kExt4UuidOffset = 0x68;      // superblock 内 s_uuid 的偏移
        constexpr std::size_t kExt4UuidBytes = 16;         // s_uuid 的长度

        // 按小端序写入 16 位
        void Put16Le(uint8_t* p, uint16_t v) {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>(v >> 8);
        }

        // 按小端序写入 32 位
        void Put32Le(uint8_t* p, uint32_t v) {
            for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }

        // 按小端序写入 64 位
        void Put64Le(uint8_t* p, uint64_t v) {
            for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }

        // 按小端序读取 16 位
        uint16_t Get16Le(const uint8_t* p) {
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        // 按小端序读取 32 位
        uint32_t Get32Le(const uint8_t* p) {
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (8 * i);
            return value;
        }

        // 校验内核路径的形式：非空、以 / 起头、不超过字段宽度
        void CheckKernelPath(const std::string& path) {
            if (path.empty()) throw std::runtime_error("布局描述的内核路径不能为空");
            if (path.size() > kPathMaxBytes) throw std::runtime_error("布局描述的内核路径超过 " + std::to_string(kPathMaxBytes) + " 字节");
            if (path[0] != '/') throw std::runtime_error("布局描述的内核路径必须是卷内绝对路径：" + path);
        }
    }

    std::array<uint8_t, 16> ReadExt4VolumeUuid(const std::string& path) {
        const std::vector<uint8_t> head = ReadHead(path, kExt4SuperOffset + kExt4SuperBytes);
        if (head.size() < kExt4SuperOffset + kExt4SuperBytes) throw std::runtime_error("系统卷镜像过小，读不到 Ext4 superblock：" + path);
        const uint8_t* const super = head.data() + kExt4SuperOffset;
        if (Get16Le(super + kExt4MagicOffset) != kExt4Magic) throw std::runtime_error("系统卷镜像的 Ext4 魔数不符：" + path);
        std::array<uint8_t, kExt4UuidBytes> uuid{};
        std::copy_n(super + kExt4UuidOffset, kExt4UuidBytes, uuid.begin());
        return uuid;
    }

    std::string ReadTemplateKernelPath(const std::string& path) {
        const std::vector<uint8_t> block = ReadFile(path);
        if (block.size() < kLayoutBytes) throw std::runtime_error("布局描述模板过小：" + path);
        if (Get32Le(block.data()) != kMagic) throw std::runtime_error("布局描述模板的格式标记不符：" + path);
        if (Get16Le(block.data() + 4) != kVersion) throw std::runtime_error("布局描述模板的版本不支持：" + path);
        if (Get16Le(block.data() + 6) != kHeaderBytes) throw std::runtime_error("布局描述模板的头部长度不符：" + path);
        if (Get32Le(block.data() + 8) != kLayoutBytes) throw std::runtime_error("布局描述模板的有效长度不符：" + path);
        const uint16_t pathBytes = Get16Le(block.data() + kFieldPathBytes);
        if (pathBytes == 0 || pathBytes > kPathMaxBytes) throw std::runtime_error("布局描述模板的内核路径长度非法：" + path);
        const std::string kernel(block.begin() + kFieldPath, block.begin() + kFieldPath + pathBytes);
        CheckKernelPath(kernel);
        return kernel;
    }

    std::vector<uint8_t> BuildBaleenLayout(const BaleenLayoutSpec& spec) {
        CheckKernelPath(spec.kernel_path);
        if (spec.volume_image.empty()) throw std::runtime_error("布局描述需要系统卷镜像以读取卷标识");
        if (spec.volume_bytes == 0) throw std::runtime_error("布局描述的系统卷长度必须非 0");
        if (spec.state_offset != 0 && spec.state_bytes == 0) throw std::runtime_error("布局描述声明了状态区域位置却没有长度");
        if (spec.state_bytes > 0xFFFFFFFFull) throw std::runtime_error("布局描述的引导状态区域长度超出 32 位");
        const std::array<uint8_t, kExt4UuidBytes> uuid = ReadExt4VolumeUuid(spec.volume_image);

        std::vector<uint8_t> block(BaleenLayout::kBlockBytes, 0);
        Put32Le(block.data(), kMagic);
        Put16Le(block.data() + 4, kVersion);
        Put16Le(block.data() + 6, kHeaderBytes);
        Put32Le(block.data() + 8, kLayoutBytes);
        // Flags 与保留字段保持 0
        std::copy(uuid.begin(), uuid.end(), block.begin() + static_cast<std::ptrdiff_t>(kFieldUuid));
        Put64Le(block.data() + kFieldVolumeOffset, spec.volume_offset);
        Put64Le(block.data() + kFieldVolumeBytes, spec.volume_bytes);
        Put64Le(block.data() + kFieldStateOffset, spec.state_offset);
        Put32Le(block.data() + kFieldStateBytes, static_cast<uint32_t>(spec.state_bytes));
        Put16Le(block.data() + kFieldPathBytes, static_cast<uint16_t>(spec.kernel_path.size()));
        std::copy(spec.kernel_path.begin(), spec.kernel_path.end(), block.begin() + static_cast<std::ptrdiff_t>(kFieldPath));
        Put32Le(block.data() + kFieldChecksum, Baleen::LayoutDescriptor::Checksum(block.data()));
        return block;
    }
}
