/* LayoutStateArea.hpp
    引导状态区域的形态判据：BaleenCore、UEFI 侧与宿主生成器共用同一份实现

    判据只读布局描述块本身，不读设备上的状态区域、也不依赖诊断编号与打印：
    目标侧把不通过映射成 E-CORE-90 / E-UEFI-90，宿主生成器把不通过转成构建错误，
    两边因此不会对同一份布局给出不同结论
    位置与重叠规则见 Docs/Specs/Baleen/布局描述.md 第四节
*/

#pragma once
#include <stdint.h>

#include <LayoutDescriptor.hpp>

namespace Baleen {
    namespace LayoutStateArea {
        // 按小端读取 32 位
        inline uint32_t Get32(const uint8_t* field) {
            uint32_t value = 0;
            for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
            return value;
        }

        // 按小端读取 64 位
        inline uint64_t Get64(const uint8_t* field) {
            return static_cast<uint64_t>(Get32(field)) | (static_cast<uint64_t>(Get32(field + 4)) << 32);
        }

        // 状态区域形态是否成立：StateOffset 为 0 表示没有状态区域、StateBytes 忽略；
        // 否则要求起点是 4096 的整数倍、长度至少 4096、端点不回绕，
        // 且不与布局描述块、描述符区默认范围、系统卷跨度相交
        // 只读布局描述块本身，不读设备上的状态区域
        inline bool Valid(const uint8_t* block) {
            using Baleen::LayoutDescriptor;
            const uint64_t stateOffset = Get64(block + LayoutDescriptor::kFieldStateOffset);
            if (stateOffset == 0) return true;
            const uint32_t stateBytes = Get32(block + LayoutDescriptor::kFieldStateBytes);
            if ((stateOffset & (LayoutDescriptor::kStateAlign - 1)) != 0) return false;
            if (stateBytes < LayoutDescriptor::kStateBytes) return false;
            const uint64_t stateEnd = stateOffset + stateBytes;
            if (stateEnd < stateOffset) return false;
            // 与布局描述自身 [kOffset, kOffset + kBytes) 相交
            if (stateOffset < LayoutDescriptor::kOffset + LayoutDescriptor::kBytes && LayoutDescriptor::kOffset < stateEnd) return false;
            // 与描述符区默认位置 [kDescriptorOffset, kDescriptorOffset + kDescriptorBytes) 相交
            if (stateOffset < LayoutDescriptor::kDescriptorOffset + LayoutDescriptor::kDescriptorBytes && LayoutDescriptor::kDescriptorOffset < stateEnd) return false;
            // 与系统卷 [VolumeOffset, VolumeOffset + VolumeBytes) 相交：用“起点互在对方跨度内”
            // 的减法判定，避免 VolumeOffset + VolumeBytes 回绕带来的错误结论
            const uint64_t volumeOffset = Get64(block + LayoutDescriptor::kFieldVolumeOffset);
            const uint64_t volumeBytes = Get64(block + LayoutDescriptor::kFieldVolumeBytes);
            if ((stateOffset >= volumeOffset && stateOffset - volumeOffset < volumeBytes) ||
                (volumeOffset >= stateOffset && volumeOffset - stateOffset < stateBytes)) return false;
            return true;
        }
    }
}
