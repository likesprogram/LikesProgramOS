/* Fat.h
    FAT 引导扇区类型判定与 ESP 镜像校验接口
*/

#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace hostbuild {
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

    // 校验镜像文件的引导扇区必须是 FAT16，否则抛异常。role 用于错误信息，如 "ESP 镜像"
    // ESP 与 El Torito 的 EFI 镜像都用 FAT16：双 ESP 布局要求同一份内容同时落在 1 MiB 与 8 MiB、
    // 体积不得超过 7 MiB，而 FAT32 的最小卷是 32 MiB；UEFI 规范要求固件支持 FAT12/16/32
    void RequireFat16Image(const std::string& path, const std::string& role);
}
