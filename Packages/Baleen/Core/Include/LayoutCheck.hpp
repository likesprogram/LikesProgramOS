/* LayoutCheck.hpp
    布局描述的校验：BaleenCore 与 UEFI 侧共用同一份判据与编号
*/

#pragma once
#include <stdint.h>

#include <Diagnostics.hpp>
#include <LayoutDescriptor.hpp>
#include <LayoutStateArea.hpp>

namespace Baleen {
    namespace Core {
        namespace LayoutCheck {
            // —— 布局描述的失败编号：序号与失败诊断编号表一致，前缀随编译目标 ——
            constexpr Diagnostic kFailMagic{BALEEN_STAGE_CODE("18"), "The layout descriptor magic doesn't match"};
            constexpr Diagnostic kFailVersion{BALEEN_STAGE_CODE("19"), "The layout descriptor version isn't supported"};
            constexpr Diagnostic kFailHead{BALEEN_STAGE_CODE("20"), "The layout descriptor header length isn't supported"};
            constexpr Diagnostic kFailBytes{BALEEN_STAGE_CODE("21"), "The layout descriptor length doesn't match"};
            constexpr Diagnostic kFailFlags{BALEEN_STAGE_CODE("22"), "The layout descriptor flags aren't supported"};
            constexpr Diagnostic kFailReserved{BALEEN_STAGE_CODE("23"), "The layout descriptor reserved field isn't zero"};
            constexpr Diagnostic kFailChecksum{BALEEN_STAGE_CODE("24"), "The layout descriptor checksum doesn't match"};
            constexpr Diagnostic kFailPathBytes{BALEEN_STAGE_CODE("25"), "The layout descriptor kernel path length is invalid"};
            constexpr Diagnostic kFailPathAbsolute{BALEEN_STAGE_CODE("26"), "The layout descriptor kernel path isn't absolute"};
            constexpr Diagnostic kFailVolumeBytes{BALEEN_STAGE_CODE("27"), "The layout descriptor volume length is zero"};
            // 引导状态区域参数非法：长度不足、起点未按 4096 对齐、与保留区重叠都属于这一类
            // 与 SelectSet 共用同一编号与文本，布局校验在选版之前先报
            constexpr Diagnostic kFailStateArea{BALEEN_STAGE_CODE("90"), "The boot state area is invalid: check StateOffset and StateBytes"};
            // 路径语法不只是一条“首字符是 /”：分量必须非空，且不许出现 . 与 ..
            constexpr Diagnostic kFailPathSyntax{BALEEN_STAGE_CODE("93"), "The layout descriptor kernel path is invalid"};

            // 按小端读取 16 位
            inline uint16_t Get16(const uint8_t* field) {
                return static_cast<uint16_t>(field[0] | (field[1] << 8));
            }

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

            // 统一路径语法：分量非空、不含 . 与 ..；absolute 为真时要求以 / 起头
            // 清单条目路径与布局路径共用这套规则
            inline bool PathValid(const uint8_t* path, uint32_t bytes, bool absolute) {
                if (bytes == 0) return false;
                uint32_t begin = 0;
                if (path[0] == '/') {
                    if (!absolute) return false;
                    begin = 1;
                    if (bytes == 1) return false;
                } else if (absolute) {
                    return false;
                }
                for (uint32_t i = begin; i <= bytes; ++i) {
                    // 路径最终作为 C 字符串交给文件系统：NUL 会在那里截断，按非法路径拒绝
                    if (i < bytes && path[i] == 0) return false;
                    if (i < bytes && path[i] != '/') continue;
                    const uint32_t component = i - begin;
                    if (component == 0) return false;
                    if (component == 1 && path[begin] == '.') return false;
                    if (component == 2 && path[begin] == '.' && path[begin + 1] == '.') return false;
                    begin = i + 1;
                }
                return true;
            }

            // 引导状态区域形态的判据在 LayoutStateArea 单点实现（宿主生成器也复用同一份）：
            // 这里只把它映射成本阶段的编号，不另写一份判据
            inline Diagnostic CheckStateArea(const uint8_t* block) {
                return Baleen::LayoutStateArea::Valid(block) ? Diagnostic{} : kFailStateArea;
            }

            // 校验布局描述：固定字段、标志位、保留字段、校验和、尾部保留区、内核路径语法与状态区域形态
            // 成功返回空 code；任一不通过返回对应编号
            inline Diagnostic Check(const uint8_t* block) {
                using Baleen::LayoutDescriptor;
                if (Get32(block + LayoutDescriptor::kFieldMagic) != LayoutDescriptor::kMagic) return kFailMagic;
                if (Get16(block + LayoutDescriptor::kFieldVersion) != LayoutDescriptor::kVersion) return kFailVersion;
                if (Get16(block + LayoutDescriptor::kFieldHeaderBytes) != LayoutDescriptor::kHeaderBytes) return kFailHead;
                if (Get32(block + LayoutDescriptor::kFieldLayoutBytes) != LayoutDescriptor::kLayoutBytes) return kFailBytes;
                if ((Get32(block + LayoutDescriptor::kFieldFlags) & ~LayoutDescriptor::kKnownFlags) != 0) return kFailFlags;
                if (Get16(block + LayoutDescriptor::kFieldReserved) != 0) return kFailReserved;
                if (Get32(block + LayoutDescriptor::kFieldChecksum) != LayoutDescriptor::Checksum(block)) return kFailChecksum;
                // 有效内容之后的整段保留区必须为 0：否则说明写入方越界或介质上有非预期内容
                for (uint32_t i = LayoutDescriptor::kLayoutBytes; i < LayoutDescriptor::kBytes; ++i) {
                    if (block[i] != 0) return kFailReserved;
                }
                const uint32_t pathBytes = Get16(block + LayoutDescriptor::kFieldKernelPathBytes);
                if (pathBytes == 0 || pathBytes > LayoutDescriptor::kKernelPathMaxBytes) return kFailPathBytes;
                if (block[LayoutDescriptor::kFieldKernelPath] != '/') return kFailPathAbsolute;
                if (!PathValid(block + LayoutDescriptor::kFieldKernelPath, pathBytes, true)) return kFailPathSyntax;
                if (Get64(block + LayoutDescriptor::kFieldVolumeBytes) == 0) return kFailVolumeBytes;
                // 状态区域形态放在已有判据之后：布局校验阶段先报，不等到读取状态区域时才发现
                return CheckStateArea(block);
            }
        }
    }
}
