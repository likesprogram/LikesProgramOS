/* Ext4Features.hpp
    Ext4 卷格式的接受与拒绝清单：Baleen 只读实现与内核 Ext4 模块共用的单点定义

    位定义与拒绝理由由本文件给出；
    INCOMPAT 里不认识的位一律拒绝，不猜测语义；COMPAT 只影响写路径，读取不看
    接受集合拆成两个常量（子集与超集）：
      kBaleenAccepted  Baleen 只读最小实现接受的集合（原 kIncompatAccepted 的取值）
      kKernelAccepted  内核完整实现的最终接受集合，超集：含阶段 3 日志回放需要的 RECOVER
    子集关系由 static_assert 固定；**当前阶段只读挂载两侧都按 kBaleenAccepted 判定**：
    kKernelAccepted 只是阶段 3 的预留，当前阶段任何路径不得按它接受卷——否则会宣称
    尚未实现的日志回放能力。判定函数保持「按传入集合判定」的形式，
    不把某个集合写死进函数，阶段 3 切换时只改调用点的实参
*/

#pragma once
#include <stdint.h>

namespace Storage {
    namespace Ext4Features {
        // INCOMPAT：不认识的位必须拒绝，因此每个认识的位都要在这里列出
        constexpr uint32_t kIncompatCompression = 0x0001;    // 压缩，拒绝
        constexpr uint32_t kIncompatFileType = 0x0002;       // 目录项带类型字节，接受
        constexpr uint32_t kIncompatRecover = 0x0004;        // 需要日志回放，只读路径拒绝
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

        // Baleen 只读最小实现接受的 INCOMPAT 集合：原 kIncompatAccepted 的取值
        // 内容与阶段 3 之前的内核只读实现相同：引导器能读的卷，当前阶段内核只读也必须能读
        constexpr uint32_t kBaleenAccepted = kIncompatFileType | kIncompatExtents | kIncompat64Bit | kIncompatFlexBg | kIncompatCsumSeed;

        // 旧名的等价别名：现有调用点与宿主夹具按旧名引用，语义与 kBaleenAccepted 完全相同
        constexpr uint32_t kIncompatAccepted = kBaleenAccepted;

        // 内核完整实现的最终接受集合：kBaleenAccepted 的超集
        // RECOVER 在阶段 3 的日志回放落地后才可接受；当前阶段只读挂载不得按本集合判定
        constexpr uint32_t kKernelAccepted = kBaleenAccepted | kIncompatRecover;

        // 集合关系是契约：删除 kBaleenAccepted 的位等于不兼容改动，按 ABI 兼容规则走新版本
        static_assert((kBaleenAccepted & ~kKernelAccepted) == 0, "kBaleenAccepted 必须是 kKernelAccepted 的子集");

        // 解析路径的必需位：目录项按 8 位 name_len + 1 字节类型解析（FILETYPE），
        // 文件与目录数据按 i_block 的 extent 树解析（EXTENTS）；缺失即不接受该卷，不猜测兼容布局
        // CSUM_SEED 与 FLEX_BG 不在必需集合内：前者只给校验和计算提供种子，后者只影响块分配位置
        constexpr uint32_t kIncompatRequired = kIncompatFileType | kIncompatExtents;

        // 明确拒绝的 INCOMPAT 位集合：列出以便诊断文本直接指出（其余未知位同样拒绝）
        constexpr uint32_t kIncompatRejected = kIncompatCompression | kIncompatRecover | kIncompatJournalDev | kIncompatMetaBg | kIncompatMmp | kIncompatEaInode | kIncompatDirdata | kIncompatLargeDir | kIncompatInlineData | kIncompatEncrypt | kIncompatCasefold;

        // RO_COMPAT：只读挂载下只有 BIGALLOC 改变解析方式
        constexpr uint32_t kRoCompatBigalloc = 0x0200;       // 以簇为单位分配，extent 长度含义不同，拒绝

        // INCOMPAT 是否全部落在给定接受集合内；默认按当前阶段只读挂载的 kBaleenAccepted 判定
        constexpr bool IncompatSupported(uint32_t flags, uint32_t accepted = kBaleenAccepted) { return (flags & ~accepted) == 0; }

        // 解析前提位是否齐全；缺位的卷在挂载阶段就拒绝，不拖到解析目录项或 extent 时才报
        constexpr bool IncompatHasRequired(uint32_t flags) { return (flags & kIncompatRequired) == kIncompatRequired; }

        // RO_COMPAT 是否可读：只拒绝 BIGALLOC，其余位不影响只读解析
        constexpr bool RoCompatSupported(uint32_t flags) { return (flags & kRoCompatBigalloc) == 0; }
    }
}
