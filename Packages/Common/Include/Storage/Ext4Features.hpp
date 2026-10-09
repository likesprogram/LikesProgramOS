/* Ext4Features.hpp
    Ext4 卷格式的接受与拒绝清单：Baleen 只读实现与内核完整实现引用同一份定义

    未知的 INCOMPAT 位一律拒绝，不猜测语义；Baleen 侧只读实现的接受集合是内核实现的子集：
    引导器能读的卷，内核必须也能读，两侧的判定都从这里取，避免出现一侧能读、另一侧读不了的布局
    COMPAT 位只影响写路径，读取不受影响；RO_COMPAT 里只有 BIGALLOC 改变 extent 的单位，必须拒绝
    接受集合内另分必需位（kIncompatRequired）：只读解析路径的硬前提，缺失位的卷在挂载阶段拒绝

    位定义依据 Ext4 Disk Layout 的特性说明，见 Docs/Specs/Common/文件系统抽象与VFS.md 第八节
*/

#pragma once
#include <stdint.h>

namespace Storage {
    namespace Ext4Features {
        // INCOMPAT：不认识的位必须拒绝，因此每个认识的位都要在这里列出
        constexpr uint32_t kIncompatCompression = 0x0001;    // 压缩，拒绝
        constexpr uint32_t kIncompatFileType = 0x0002;       // 目录项带类型字节，接受
        constexpr uint32_t kIncompatRecover = 0x0004;        // 需要日志回放，拒绝（只读路径不回放日志）
        constexpr uint32_t kIncompatJournalDev = 0x0008;     // 外部日志设备，拒绝
        constexpr uint32_t kIncompatMetaBg = 0x0010;         // 元数据块组，拒绝
        constexpr uint32_t kIncompatExtents = 0x0040;        // extent 树，接受
        constexpr uint32_t kIncompat64Bit = 0x0080;          // 64 位块组描述符与块号，接受
        constexpr uint32_t kIncompatMmp = 0x0100;            // 多挂载保护，拒绝
        constexpr uint32_t kIncompatFlexBg = 0x0200;         // 弹性块组，接受（只影响块分配位置）
        constexpr uint32_t kIncompatEaInode = 0x0400;        // 扩展属性存进 inode，拒绝
        constexpr uint32_t kIncompatDirdata = 0x1000;        // 目录数据在独立文件，拒绝
        constexpr uint32_t kIncompatCsumSeed = 0x2000;       // 校验和种子，接受（只影响校验计算，不需要在只读解析里复核）
        constexpr uint32_t kIncompatLargeDir = 0x4000;       // 大目录项（名字超 255），拒绝
        constexpr uint32_t kIncompatInlineData = 0x8000;     // 内联数据，拒绝
        constexpr uint32_t kIncompatEncrypt = 0x10000;       // 加密，拒绝
        constexpr uint32_t kIncompatCasefold = 0x20000;      // 大小写折叠目录，拒绝

        // 接受的 INCOMPAT 位集合：只读最小实现能识别的位，不在集合内的未知位一律拒绝
        constexpr uint32_t kIncompatAccepted = kIncompatFileType | kIncompatExtents | kIncompat64Bit | kIncompatFlexBg | kIncompatCsumSeed;

        // 解析路径的必需位：目录项按 8 位 name_len + 1 字节类型解析（FILETYPE），
        // 文件与目录数据按 i_block 的 extent 树解析（EXTENTS）；缺失即不接受该卷，不猜测兼容布局
        // CSUM_SEED 与 FLEX_BG 不在必需集合内：前者只给校验和计算提供种子，后者只影响块分配位置
        constexpr uint32_t kIncompatRequired = kIncompatFileType | kIncompatExtents;

        // 明确拒绝的 INCOMPAT 位集合：列出以便诊断文本直接指出（其余未知位同样拒绝）
        constexpr uint32_t kIncompatRejected = kIncompatCompression | kIncompatRecover | kIncompatJournalDev | kIncompatMetaBg | kIncompatMmp | kIncompatEaInode | kIncompatDirdata | kIncompatLargeDir | kIncompatInlineData | kIncompatEncrypt | kIncompatCasefold;

        // RO_COMPAT：只读挂载下只有 BIGALLOC 改变解析方式
        constexpr uint32_t kRoCompatBigalloc = 0x0200;       // 以簇为单位分配，extent 长度含义不同，拒绝

        // INCOMPAT 是否全部可识别，有未知位或拒绝位即不受支持
        constexpr bool IncompatSupported(uint32_t flags) { return (flags & ~kIncompatAccepted) == 0; }

        // 解析前提位是否齐全；缺位的卷在挂载阶段就拒绝，不拖到解析目录项或 extent 时才报
        constexpr bool IncompatHasRequired(uint32_t flags) { return (flags & kIncompatRequired) == kIncompatRequired; }

        // RO_COMPAT 是否可读：只拒绝 BIGALLOC，其余位不影响只读解析
        constexpr bool RoCompatSupported(uint32_t flags) { return (flags & kRoCompatBigalloc) == 0; }
    }
}
