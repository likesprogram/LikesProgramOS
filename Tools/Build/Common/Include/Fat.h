/* Fat.h
    FAT 引导扇区类型判定与 FAT32 校验接口
*/

#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace makeiso {
    // FAT 类型枚举
    enum class FatType {
        Unknown, // 未知类型
        Fat12,   // FAT12
        Fat16,   // FAT16
        Fat32    // FAT32
    };

    // 按 Microsoft FAT 规范的簇数规则判定引导扇区所属的 FAT 类型（不依赖卷标字符串）
    // detail 写出诊断信息：成功时给簇数，失败时给原因
    FatType DetectFatType(std::span<const uint8_t> bootSector, std::string& detail);

    // 校验镜像文件的引导扇区必须是 FAT32，否则抛异常。role 用于错误信息，如 "ESP 镜像"
    // ESP 写死为 FAT32：UEFI 固件读它，内核必备的 FAT32 实现也读它
    void RequireFat32Image(const std::string& path, const std::string& role);
}
