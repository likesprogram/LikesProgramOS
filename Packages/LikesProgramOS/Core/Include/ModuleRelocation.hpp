/* ModuleRelocation.hpp
    模块重定位：按容器重定位表逐项写入 Base + Addend，只允许 RELATIVE64 与 RELATIVE32

    写入经映像记录里的可写指针（ModuleImage::memory），不重复做物理地址换算：
    TEXT 段不允许重定位，本层按段表复核而不是相信解析结果——重定位是模块字节在映射后唯一
    被改写的环节，写错位置会把模块代码写坏且无法回滚
    逐项检查：类型、目标按宽度对齐、目标区间完整落在映像跨度内、目标完整落在可写段内、
    Base + Addend 不溢出 64 位、RELATIVE32 结果不超出 32 位；任一项失败即停止并返回分类，
    已写入的条目不回滚，由调用方回收整个映像
    失败返回 RelocationError 分类，原因文本由 RelocationErrorText 给出；装载器统一映射到 E-KRN-57
    并把文本作为 detail，本文件不打印、不停机
*/

#pragma once
#include <stdint.h>

#include <ModuleContainer.hpp>
#include <ModuleImage.hpp>

namespace Kernel {
    // 重定位失败分类：全部映射到 E-KRN-57，文本作 detail；分类只增不改
    enum class RelocationError : uint8_t {
        None = 0,           // 成功
        NullPointer,        // 容器视图没有容器字节
        ImageMismatch,      // 映像基址为零、未按 4 KiB 对齐或跨度与容器视图不一致
        Table,              // 重定位表偏移、条目数或表区越过容器字节
        UnknownType,        // 重定位类型不受支持
        TargetAlignment,    // 目标偏移未按取值宽度对齐
        TargetRange,        // 目标区间越过映像跨度
        TargetSection,      // 目标不在可写段（DATA / BSS）内
        AddressOverflow,    // Base + Addend 溢出 64 位
        Result32,           // RELATIVE32 的 Base + Addend 超出 32 位
    };

    // 应用容器重定位表：成功返回 None；失败返回分类并停止（已写入的条目不回滚）
    RelocationError ApplyModuleRelocations(const ContainerView& view, const ModuleImage& image);

    // 失败分类的英文原因文本：给 E-KRN-57 的 detail 用，不带标点
    const char* RelocationErrorText(RelocationError error);
}
