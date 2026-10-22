/* BaleenLayout.cpp
    Baleen 布局描述的宿主侧编码实现：Ext4 卷标识读取、模板解析与字段回填

    字节表的字段常量与校验规则取自目标侧 LayoutDescriptor.hpp；本文件不改变格式，
    只按这些常量回填字节；保留字段一律写 0，校验和覆盖 [0x000, 0x140)
*/

#include <BaleenLayout.h>

#include <algorithm>
#include <cstdio>
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
        constexpr std::size_t kFieldFlags = Baleen::LayoutDescriptor::kFieldFlags;
        constexpr std::size_t kFieldUuid = Baleen::LayoutDescriptor::kFieldVolumeUuid;
        constexpr std::size_t kFieldVolumeOffset = Baleen::LayoutDescriptor::kFieldVolumeOffset;
        constexpr std::size_t kFieldVolumeBytes = Baleen::LayoutDescriptor::kFieldVolumeBytes;
        constexpr std::size_t kFieldStateOffset = Baleen::LayoutDescriptor::kFieldStateOffset;
        constexpr std::size_t kFieldStateBytes = Baleen::LayoutDescriptor::kFieldStateBytes;
        constexpr std::size_t kFieldPathBytes = Baleen::LayoutDescriptor::kFieldKernelPathBytes;
        constexpr std::size_t kFieldReserved = Baleen::LayoutDescriptor::kFieldReserved;
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

        // 把 32 位值写成 0x 前缀的八位十六进制，供标志位报错使用
        std::string Hex32(uint32_t value) {
            char text[11] = {};
            std::snprintf(text, sizeof(text), "0x%08X", value);
            return text;
        }

        // 校验内核路径：长度不超过字段宽度，语法与引导侧统一路径一致（绝对路径、分量非空、无 . 与 ..）
        void CheckKernelPath(const std::string& path) {
            if (path.size() > kPathMaxBytes) throw std::runtime_error("布局描述的内核路径超过 " + std::to_string(kPathMaxBytes) + " 字节");
            if (!BaleenLayoutPathValid(path)) throw std::runtime_error("布局描述的内核路径不符合统一路径语法（须为卷内绝对路径、分量非空、不含 . 与 ..）：" + path);
        }
    }

    bool BaleenLayoutPathValid(const std::string& path) {
        const std::size_t bytes = path.size();
        if (bytes == 0 || path[0] != '/') return false;
        // 只有 "/" 的路径没有路径分量
        if (bytes == 1) return false;
        std::size_t begin = 1;
        for (std::size_t i = begin; i <= bytes; ++i) {
            if (i < bytes && path[i] != '/') continue;
            const std::size_t component = i - begin;
            if (component == 0) return false;
            if (component == 1 && path[begin] == '.') return false;
            if (component == 2 && path[begin] == '.' && path[begin + 1] == '.') return false;
            begin = i + 1;
        }
        return true;
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
        // 标志位、保留字段与尾部保留区按引导侧的同一套判据核对：模板带出未知位或非零保留字节，
        // 说明它不是本格式写入方产出的字节，拒绝而不是猜测
        const uint32_t unknownFlags = Get32Le(block.data() + kFieldFlags) & ~Baleen::LayoutDescriptor::kKnownFlags;
        if (unknownFlags != 0) throw std::runtime_error("布局描述模板包含未定义的标志位 " + Hex32(unknownFlags) + "：" + path);
        if (Get16Le(block.data() + kFieldReserved) != 0) throw std::runtime_error("布局描述模板的保留字段非零：" + path);
        // 模板可以只带有效内容（不足一个块），此时尾部按不存在处理；带出来的尾部字节必须全 0
        const std::size_t tailEnd = std::min<std::size_t>(block.size(), static_cast<std::size_t>(BaleenLayout::kBlockBytes));
        for (std::size_t i = kLayoutBytes; i < tailEnd; ++i) {
            if (block[i] != 0) throw std::runtime_error("布局描述模板的尾部保留区非零，偏移 " + Hex32(static_cast<uint32_t>(i)) + "：" + path);
        }
        const uint16_t pathBytes = Get16Le(block.data() + kFieldPathBytes);
        if (pathBytes == 0 || pathBytes > kPathMaxBytes) throw std::runtime_error("布局描述模板的内核路径长度非法：" + path);
        const std::string kernel(block.begin() + kFieldPath, block.begin() + kFieldPath + pathBytes);
        CheckKernelPath(kernel);
        return kernel;
    }

    std::vector<uint8_t> BuildBaleenLayout(const BaleenLayoutSpec& spec) {
        // 标志位：只接受已定义的位；未定义的位会让引导侧按格式不符拒绝（E-CORE-22 / E-UEFI-22），
        // 生成器提前报错，避免产出一份目标侧不接受的布局描述
        const uint32_t unknownFlags = spec.flags & ~Baleen::LayoutDescriptor::kKnownFlags;
        if (unknownFlags != 0) throw std::runtime_error("布局描述包含未定义的标志位 " + Hex32(unknownFlags) + "（已定义位 " + Hex32(Baleen::LayoutDescriptor::kKnownFlags) + "）");
        // 内核路径必须显式给出：旧的扁平默认路径已退役，缺路径不产出会被选版拒绝的布局描述
        if (spec.kernel_path.empty()) throw std::runtime_error("布局描述缺少内核路径：必须由调用方显式给出（参数或模板），没有默认值");
        CheckKernelPath(spec.kernel_path);
        if (spec.volume_image.empty()) throw std::runtime_error("布局描述需要系统卷镜像以读取卷标识");
        if (spec.volume_bytes == 0) throw std::runtime_error("布局描述的系统卷长度必须非 0");
        if (spec.state_offset != 0 && spec.state_bytes == 0) throw std::runtime_error("布局描述声明了状态区域位置却没有长度");
        if (spec.state_bytes > 0xFFFFFFFFull) throw std::runtime_error("布局描述的引导状态区域长度超出 32 位");
        const std::array<uint8_t, kExt4UuidBytes> uuid = ReadExt4VolumeUuid(spec.volume_image);

        // 整块先清零：有效内容之后的尾部 [0x144, 0x1000) 保持全 0，与引导侧校验一致
        std::vector<uint8_t> block(BaleenLayout::kBlockBytes, 0);
        Put32Le(block.data(), kMagic);
        Put16Le(block.data() + 4, kVersion);
        Put16Le(block.data() + 6, kHeaderBytes);
        Put32Le(block.data() + 8, kLayoutBytes);
        // 标志位由部署侧给出：当前只定义"记录损坏时允许退到缺省集"，未定义的位必须为 0
        // 引导侧按 LayoutDescriptor::kKnownFlags 核对，出现未定义的位即拒绝（E-CORE-22 / E-UEFI-22）
        Put32Le(block.data() + kFieldFlags, spec.flags);
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
