/* LayoutDescriptor.hpp
    Baleen 布局描述：介质上固定结构的字段偏移与固定取值

    字节表、写入位置与校验规则由本头单点定义；
    本头给目标侧读取实现（BaleenCore / UEFI）与宿主侧生成器共用，改动必须两侧同步
    位置字段都是绝对字节偏移；固定取值改动会同时影响两侧，不能只改一边
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    // 布局描述：偏移 0x20000 起一个 4096 字节块，512 / 2048 / 4096 三种单位下都整扇区可读
    struct LayoutDescriptor {
        static constexpr uint64_t kOffset = 0x20000;         // 介质字节偏移（512 字节基准 256）
        static constexpr uint64_t kBytes = 0x1000;           // 占一个 4096 字节块
        static constexpr uint32_t kMagic = 0x59414C42;       // 'BLAY'
        static constexpr uint16_t kVersion = 1;              // 当前格式版本
        static constexpr uint16_t kHeaderBytes = 0x40;       // 头部固定长度
        static constexpr uint32_t kLayoutBytes = 0x144;      // 本结构有效字节数
        static constexpr uint32_t kKernelPathMaxBytes = 255; // 内核路径字节数上限
        static constexpr uint32_t kChecksumBytes = 0x140;    // 校验和覆盖 [0x000, 0x140)

        // 字段偏移
        static constexpr uint32_t kFieldMagic = 0x000;           // 格式标记
        static constexpr uint32_t kFieldVersion = 0x004;         // 版本
        static constexpr uint32_t kFieldHeaderBytes = 0x006;     // 头部长度
        static constexpr uint32_t kFieldLayoutBytes = 0x008;     // 有效长度
        static constexpr uint32_t kFieldFlags = 0x00C;           // 标志；已定义的位见下，未定义的位必须为 0
        static constexpr uint32_t kFieldVolumeUuid = 0x010;      // 卷标识（Ext4 s_uuid，16 字节）
        static constexpr uint32_t kFieldVolumeOffset = 0x020;    // 系统卷起始字节偏移（8 字节）
        static constexpr uint32_t kFieldVolumeBytes = 0x028;     // 系统卷长度（8 字节）
        static constexpr uint32_t kFieldStateOffset = 0x030;     // 引导状态区域起始偏移（8 字节），0 表示无
        static constexpr uint32_t kFieldStateBytes = 0x038;      // 引导状态区域长度（4 字节）
        static constexpr uint32_t kFieldKernelPathBytes = 0x03C; // 内核路径字节数（2 字节）
        static constexpr uint32_t kFieldReserved = 0x03E;        // 保留（2 字节）
        static constexpr uint32_t kFieldKernelPath = 0x040;      // 内核路径（256 字节字段）
        static constexpr uint32_t kFieldChecksum = 0x140;        // 校验和（4 字节）

        // 标志位：位 0 = 引导状态记录损坏时允许退到本描述指定的缺省集
        // 默认不置位：记录损坏意味着设备已见最高安全版本与记录里的吊销并集都丢了，
        // 那时直启缺省集是可用性换掉一档防回滚强度；要不要换由介质侧显式声明
        static constexpr uint32_t kFlagDegradeWithoutRecord = 0x00000001;
        // 已定义的标志位合集：位运算之外的位一律拒绝（E-CORE-22 / E-UEFI-22）
        static constexpr uint32_t kKnownFlags = kFlagDegradeWithoutRecord;

        // 引导状态区域的默认位置与长度
        static constexpr uint64_t kStateOffset = 0x21000;
        static constexpr uint32_t kStateBytes = 0x1000;
        // 状态区域起点对齐：4096 的整数倍，使 512 / 1024 / 2048 / 4096 逻辑块下都能整块读写
        static constexpr uint64_t kStateAlign = 0x1000;
        // 描述符区默认字节偏移：位置表见一级引导契约；IPL 槽可覆盖，读取方不读槽，
        // 布局校验只按这个默认位置作为保留区保守判定
        static constexpr uint64_t kDescriptorOffset = 0x22000;
        // 描述符区保守占位：按最大本地扇区 4096 字节计，即 [0x22000, 0x23000)
        static constexpr uint32_t kDescriptorBytes = 0x1000;

        // 校验和：覆盖 [0, kChecksumBytes) 逐字节按无符号相加，取低 32 位
        // 只用于检测写损与非预期字节变化，不构成认证
        static uint32_t Checksum(const uint8_t* block) {
            uint32_t sum = 0;
            for (uint32_t i = 0; i < kChecksumBytes; ++i) sum += block[i];
            return sum;
        }
    };
}
