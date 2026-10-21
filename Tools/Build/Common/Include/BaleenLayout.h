/* BaleenLayout.h
    Baleen 布局描述的宿主侧编码：按规格填充字段、从 Ext4 系统卷读卷标识

    布局描述的字节表、写入位置与校验规则见 Docs/Specs/Baleen/布局描述.md
    模板提供内核路径，位置类字段（卷标识、卷偏移与长度、状态区域、校验和）由组装器回填
*/

#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <LayoutDescriptor.hpp>

namespace hostbuild {
    // 布局描述在介质上的位置与默认取值：字段偏移与固定取值的单点定义在
    // Packages/Baleen/Common/Include/LayoutDescriptor.hpp，本结构只补宿主侧的默认值
    struct BaleenLayout {
        static constexpr uint64_t kWriteOffset = Baleen::LayoutDescriptor::kOffset;        // 写入介质时的字节位置（512 字节基准 256）
        static constexpr uint64_t kBlockBytes = Baleen::LayoutDescriptor::kBytes;          // 占一个 4096 字节块
        static constexpr uint64_t kStateOffset = Baleen::LayoutDescriptor::kStateOffset;   // 引导状态区域的默认字节位置
        static constexpr uint64_t kStateBytes = Baleen::LayoutDescriptor::kStateBytes;     // 引导状态区域的默认长度
        static constexpr uint64_t kReservedEnd = 0x22000;                                  // 布局描述与状态区域合计占用的结束位置（不含），供组装器登记
        static constexpr const char* kDefaultKernelPath = "/LikesProgram/System/LikesProgram.os";   // 内核路径的默认值，见 Docs/Specs/Common/系统卷布局.md
    };

    // 组装参数：内核路径来自模板或参数，其余字段由组装器按实际布局给出
    struct BaleenLayoutSpec {
        std::string kernel_path = BaleenLayout::kDefaultKernelPath;  // 内核镜像在系统卷内的绝对路径
        std::string volume_image;      // 系统卷镜像路径，用于读卷标识
        uint64_t volume_offset = 0;    // 系统卷在介质上的起始字节偏移
        uint64_t volume_bytes = 0;     // 系统卷长度，字节；必须非 0
        uint64_t state_offset = BaleenLayout::kStateOffset;  // 引导状态区域起始偏移；0 表示无状态区域
        uint64_t state_bytes = BaleenLayout::kStateBytes;    // 引导状态区域长度，字节
        // 记录损坏时是否允许退到本描述指定的缺省集（置 LayoutDescriptor::kFlagDegradeWithoutRecord）
        // 默认不置位：那条路会丢掉设备已见最高安全版本与记录里的吊销并集，是可预期性换可用性
        uint32_t flags = 0;                                  // 布局描述的标志位
    };

    // 从 Ext4 镜像读卷标识（superblock 的 s_uuid）；镜像过小或魔数不符时报错
    std::array<uint8_t, 16> ReadExt4VolumeUuid(const std::string& path);

    // 统一路径语法的宿主侧副本：以 / 起头、分量非空、不含 . 与 ..（结尾 / 与空分量同样拒绝）
    // 判据的规格是 Docs/Specs/Common/文件系统抽象与VFS.md 3.5；目标侧对应实现是
    // Packages/Baleen/Core/Include/LayoutCheck.hpp 的 PathValid(..., absolute=true)。
    // 这里保留纯函数副本而不是 include 目标头，宿主工具链因此不沾 BALEEN_STAGE_PREFIX 等阶段宏
    bool BaleenLayoutPathValid(const std::string& path);

    // 从模板读内核路径；格式标记、版本、标志位、保留字段、尾部保留区或路径语法不符时报错
    std::string ReadTemplateKernelPath(const std::string& path);

    // 生成布局描述的完整 4096 字节块：读卷标识、核对参数、回填位置字段并算校验和
    std::vector<uint8_t> BuildBaleenLayout(const BaleenLayoutSpec& spec);
}
