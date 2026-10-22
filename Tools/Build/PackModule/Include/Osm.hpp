/* Osm.hpp
    内核模块容器（.osm）的字节布局、写出器与解析校验器的共用声明

    字节表由下面的 Layout 命名空间定义：PackModule 按本文件写出容器，
    PackModule --verify 与 CheckOsm 用同一份 Parse 复算全部容器约束，写出器与校验器同源
    容器只承载布局与表项：ELF64 到容器输入的翻译在 PackModule/Src/Elf.cpp；
    接口 ID 与模块 ID 都是 16 字节标识，派生规则由契约层生成器统一给出
*/

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace osm {
    // 一个字节序列
    using Bytes = std::vector<std::uint8_t>;
    // 16 字节二进制标识：ModuleId 与 InterfaceId 形状相同，语义不同
    using IdBytes = std::array<std::uint8_t, 16>;
    // 接口稳定标识
    using InterfaceId = IdBytes;
    // 模块稳定标识
    using ModuleId = IdBytes;

    // 容器字节表：字段偏移、固定值与数量上限
    namespace Layout {
        // 文件头固定长度
        constexpr std::uint32_t kHeaderBytes = 0x80;
        // 容器格式版本，当前值
        constexpr std::uint16_t kVersion = 1;
        // 格式标记：LPOSMOD 加终止零，字节序固定
        constexpr std::array<std::uint8_t, 8> kMagic = { 0x4C, 0x50, 0x4F, 0x53, 0x4D, 0x4F, 0x44, 0x00 };
        // 文件总长度上限，64 MiB
        constexpr std::uint32_t kMaxTotalBytes = 0x4000000;
        // 容器页大小：段的内存偏移与跨度都按它对齐
        constexpr std::uint32_t kPageBytes = 0x1000;
        // 段表文件偏移的最小值
        constexpr std::uint32_t kMinTableOffset = 0x80;

        // 段数范围
        constexpr std::uint16_t kMinSectionCount = 1;
        constexpr std::uint16_t kMaxSectionCount = 16;
        // 段表条目固定长度
        constexpr std::uint16_t kSectionEntryBytes = 32;
        // 段类型取值
        constexpr std::uint32_t kTypeText = 1;
        constexpr std::uint32_t kTypeRodata = 2;
        constexpr std::uint32_t kTypeData = 3;
        constexpr std::uint32_t kTypeBss = 4;
        // 段权限位
        constexpr std::uint32_t kFlagRead = 1;
        constexpr std::uint32_t kFlagWrite = 2;
        constexpr std::uint32_t kFlagExecute = 4;
        // 段对齐范围：4 到 4096 的 2 的幂
        constexpr std::uint32_t kMinSectionAlign = 4;
        constexpr std::uint32_t kMaxSectionAlign = 4096;

        // 导出条目范围与固定长度
        constexpr std::uint16_t kMaxExportCount = 128;
        constexpr std::uint16_t kExportEntryBytes = 28;
        // 导入条目范围与固定长度
        constexpr std::uint16_t kMaxImportCount = 128;
        constexpr std::uint16_t kImportEntryBytes = 28;
        // 依赖条目范围与固定长度
        constexpr std::uint16_t kMaxDependencyCount = 32;
        constexpr std::uint16_t kDependencyEntryBytes = 32;
        // 重定位条目数上限与固定长度
        constexpr std::uint32_t kMaxRelocationCount = 65536;
        constexpr std::uint32_t kRelocationEntryBytes = 16;
        // 导出槽数与导入使用槽数的范围
        constexpr std::uint16_t kMinSlotCount = 1;
        constexpr std::uint16_t kMaxSlotCount = 64;
        // 重定位类型取值
        constexpr std::uint8_t kRelocationRelative64 = 1;
        constexpr std::uint8_t kRelocationRelative32 = 2;
        // KernelAbiMax 取此值表示不设上限
        constexpr std::uint16_t kAbiMaxUnlimited = 0xFFFF;
        // 导入 MaxVersion 取此值表示不设上限
        constexpr std::uint16_t kVersionUnlimited = 0xFFFF;
        // 依赖 MaxMajor 取此值表示不设上限
        constexpr std::uint16_t kMajorUnlimited = 0;

        // 文件头字段偏移，相对文件起点
        constexpr std::uint32_t kFieldMagic = 0x00;
        constexpr std::uint32_t kFieldVersion = 0x08;
        constexpr std::uint32_t kFieldHeaderBytes = 0x0A;
        constexpr std::uint32_t kFieldFlags = 0x0C;
        constexpr std::uint32_t kFieldTotalBytes = 0x10;
        constexpr std::uint32_t kFieldImageBytes = 0x14;
        constexpr std::uint32_t kFieldEntryInit = 0x18;
        constexpr std::uint32_t kFieldEntryActivate = 0x1C;
        constexpr std::uint32_t kFieldEntryAbort = 0x20;
        constexpr std::uint32_t kFieldSectionTableOffset = 0x24;
        constexpr std::uint32_t kFieldSectionCount = 0x26;
        constexpr std::uint32_t kFieldSectionEntryBytes = 0x28;
        constexpr std::uint32_t kFieldExportCount = 0x2A;
        constexpr std::uint32_t kFieldExportTableOffset = 0x2C;
        constexpr std::uint32_t kFieldImportCount = 0x30;
        constexpr std::uint32_t kFieldImportEntryBytes = 0x32;
        constexpr std::uint32_t kFieldImportTableOffset = 0x34;
        constexpr std::uint32_t kFieldDependencyCount = 0x38;
        constexpr std::uint32_t kFieldDependencyEntryBytes = 0x3A;
        constexpr std::uint32_t kFieldDependencyTableOffset = 0x3C;
        constexpr std::uint32_t kFieldRelocationCount = 0x40;
        constexpr std::uint32_t kFieldRelocationTableOffset = 0x44;
        constexpr std::uint32_t kFieldStringTableOffset = 0x48;
        constexpr std::uint32_t kFieldStringTableBytes = 0x4C;
        constexpr std::uint32_t kFieldModuleId = 0x50;
        constexpr std::uint32_t kFieldModuleNameOffset = 0x60;
        constexpr std::uint32_t kFieldModuleNameBytes = 0x64;
        constexpr std::uint32_t kFieldModuleVersionMajor = 0x66;
        constexpr std::uint32_t kFieldModuleVersionMinor = 0x68;
        constexpr std::uint32_t kFieldModuleVersionPatch = 0x6A;
        constexpr std::uint32_t kFieldKernelAbiMin = 0x6C;
        constexpr std::uint32_t kFieldKernelAbiMax = 0x6E;
        constexpr std::uint32_t kFieldReserved = 0x70;
        // 标识字段长度
        constexpr std::uint32_t kIdBytes = 16;

        // 段表条目内偏移，相对条目起点
        constexpr std::uint32_t kSectionNameOffset = 0x00;
        constexpr std::uint32_t kSectionType = 0x04;
        constexpr std::uint32_t kSectionFlags = 0x08;
        constexpr std::uint32_t kSectionFileOffset = 0x0C;
        constexpr std::uint32_t kSectionFileBytes = 0x10;
        constexpr std::uint32_t kSectionMemoryOffset = 0x14;
        constexpr std::uint32_t kSectionMemoryBytes = 0x18;
        constexpr std::uint32_t kSectionAlign = 0x1C;

        // 导出表条目内偏移，相对条目起点
        constexpr std::uint32_t kExportInterfaceId = 0x00;
        constexpr std::uint32_t kExportVersion = 0x10;
        constexpr std::uint32_t kExportSlotCount = 0x12;
        constexpr std::uint32_t kExportSlotTableOffset = 0x14;
        constexpr std::uint32_t kExportReserved = 0x18;

        // 导入表条目内偏移，相对条目起点
        constexpr std::uint32_t kImportInterfaceId = 0x00;
        constexpr std::uint32_t kImportMinVersion = 0x10;
        constexpr std::uint32_t kImportMaxVersion = 0x12;
        constexpr std::uint32_t kImportSlotPatchOffset = 0x14;
        constexpr std::uint32_t kImportSlotCount = 0x18;
        constexpr std::uint32_t kImportReserved = 0x1A;

        // 依赖表条目内偏移，相对条目起点
        constexpr std::uint32_t kDependencyModuleId = 0x00;
        constexpr std::uint32_t kDependencyMinMajor = 0x10;
        constexpr std::uint32_t kDependencyMinMinor = 0x12;
        constexpr std::uint32_t kDependencyMinPatch = 0x14;
        constexpr std::uint32_t kDependencyMaxMajor = 0x16;
        constexpr std::uint32_t kDependencyMaxMinor = 0x18;
        constexpr std::uint32_t kDependencyMaxPatch = 0x1A;
        constexpr std::uint32_t kDependencyFlags = 0x1C;

        // 重定位表条目内偏移，相对条目起点
        constexpr std::uint32_t kRelocationType = 0x00;
        constexpr std::uint32_t kRelocationReserved1 = 0x01;
        constexpr std::uint32_t kRelocationReserved16 = 0x02;
        constexpr std::uint32_t kRelocationTarget = 0x04;
        constexpr std::uint32_t kRelocationAddend = 0x08;
    }

    // 写出一段容器的输入段：名字、类型与权限、内存布局、文件字节
    struct SectionSpec {
        std::string name;              // 段名，空表示匿名；写进字符串表
        std::uint32_t type = 0;        // 段类型，取 Layout::kType*
        std::uint32_t flags = 0;       // 权限位，写出器按原值写，匹配规则由校验器判定
        std::uint32_t memoryOffset = 0;  // 相对模块基址的内存偏移
        std::uint32_t memoryBytes = 0;   // 内存跨度
        std::uint32_t align = 0;         // 段对齐
        Bytes fileBytes;               // 段文件数据；BSS 为空
    };

    // 写出一段导出接口条目：稳定名派生 InterfaceId，槽位值由调用方给出
    struct ExportSpec {
        std::string stableName;            // 接口稳定名，如 Core.Page
        std::uint16_t version = 0;         // 接口版本
        std::vector<std::uint32_t> slots;  // 各槽函数相对模块基址的偏移
    };

    // 写出一段导入接口条目：稳定名派生 InterfaceId，回填槽由调用方给出
    struct ImportSpec {
        std::string stableName;                // 接口稳定名
        std::uint16_t minVersion = 0;          // 可接受的最低版本
        std::uint16_t maxVersion = 0;          // 可接受的最高版本；0xFFFF 表示不设上限
        std::vector<std::uint32_t> patchSlots; // 各函数指针槽相对模块基址的内存偏移
    };

    // 写出一段模块依赖条目：逻辑名派生 ModuleId
    struct DependencySpec {
        std::string logicalName;          // 被依赖模块的逻辑名，写进字符串表
        std::uint16_t minMajor = 0;       // 可接受的最低 major
        std::uint16_t minMinor = 0;       // 可接受的最低 minor
        std::uint16_t minPatch = 0;       // 可接受的最低 patch
        std::uint16_t maxMajor = 0;       // 可接受的最高 major；0 表示不设上限
        std::uint16_t maxMinor = 0;       // 最高 minor；MaxMajor 为 0 时忽略
        std::uint16_t maxPatch = 0;       // 最高 patch；MaxMajor 为 0 时忽略
    };

    // 写出一段重定位条目
    struct RelocationSpec {
        std::uint8_t type = 0;         // 重定位类型，取 Layout::kRelocation*
        std::uint32_t target = 0;      // 相对模块基址的内存偏移
        std::uint64_t addend = 0;      // 加到基址上的加数
    };

    // 写出一个容器的全部输入
    struct Spec {
        std::string moduleName;              // 模块逻辑名，同时作为人类可读名与 ModuleId 派生输入
        std::uint16_t versionMajor = 0;      // 模块版本 major，非零
        std::uint16_t versionMinor = 0;      // 模块版本 minor
        std::uint16_t versionPatch = 0;      // 模块版本 patch
        std::uint16_t abiMin = 0;            // 可接受的内核 ABI 下限，非零
        std::uint16_t abiMax = 0;            // 可接受的内核 ABI 上限；0xFFFF 表示不设上限
        std::uint32_t entryInit = 0;         // Init 入口相对模块基址的偏移，非零
        std::uint32_t entryActivate = 0;     // Activate 入口偏移；0 表示没有
        std::uint32_t entryAbort = 0;        // Abort 入口偏移；0 表示没有
        std::vector<SectionSpec> sections;   // 段，写出时按 MemoryOffset 升序排序
        std::vector<ExportSpec> exports;     // 导出接口条目
        std::vector<ImportSpec> imports;     // 导入接口条目
        std::vector<DependencySpec> dependencies; // 模块依赖条目
        std::vector<RelocationSpec> relocations;  // 重定位条目，写出时按 Target 升序排序
    };

    // 解析出的一个段
    struct SectionInfo {
        std::string name;                // 段名，空表示匿名
        std::uint32_t type = 0;          // 段类型
        std::uint32_t flags = 0;         // 权限位
        std::uint32_t fileOffset = 0;    // 文件偏移
        std::uint32_t fileBytes = 0;     // 文件内字节数
        std::uint32_t memoryOffset = 0;  // 相对模块基址的内存偏移
        std::uint32_t memoryBytes = 0;   // 内存跨度
        std::uint32_t align = 0;         // 段对齐
    };

    // 解析出的一个导出接口条目
    struct ExportInfo {
        InterfaceId interfaceId{};          // 接口稳定标识
        std::uint16_t version = 0;          // 接口版本
        std::vector<std::uint32_t> slots;   // 槽位表的各槽偏移
    };

    // 解析出的一个导入接口条目
    struct ImportInfo {
        InterfaceId interfaceId{};             // 接口稳定标识
        std::uint16_t minVersion = 0;          // 可接受的最低版本
        std::uint16_t maxVersion = 0;          // 可接受的最高版本
        std::vector<std::uint32_t> patchSlots; // 函数指针槽的内存偏移
    };

    // 解析出的一个模块依赖条目
    struct DependencyInfo {
        ModuleId moduleId{};             // 被依赖模块的稳定标识
        std::uint16_t minMajor = 0;      // 可接受的最低 major
        std::uint16_t minMinor = 0;      // 可接受的最低 minor
        std::uint16_t minPatch = 0;      // 可接受的最低 patch
        std::uint16_t maxMajor = 0;      // 可接受的最高 major；0 表示不设上限
        std::uint16_t maxMinor = 0;      // 最高 minor
        std::uint16_t maxPatch = 0;      // 最高 patch
    };

    // 解析出的一个重定位条目
    struct RelocationInfo {
        std::uint8_t type = 0;       // 重定位类型
        std::uint32_t target = 0;    // 相对模块基址的内存偏移
        std::uint64_t addend = 0;    // 加到基址上的加数
    };

    // 解析并通过全部容器约束校验后的镜像
    struct Image {
        std::uint16_t version = 0;                  // 容器格式版本
        std::uint32_t totalBytes = 0;               // 文件总长度
        std::uint32_t imageBytes = 0;               // 装载映像跨度
        std::uint32_t entryInit = 0;                // Init 入口偏移
        std::uint32_t entryActivate = 0;            // Activate 入口偏移
        std::uint32_t entryAbort = 0;               // Abort 入口偏移
        ModuleId moduleId{};                        // 模块稳定标识
        std::string moduleName;                     // 人类可读模块名，空表示没有
        std::uint16_t versionMajor = 0;             // 模块版本 major
        std::uint16_t versionMinor = 0;             // 模块版本 minor
        std::uint16_t versionPatch = 0;             // 模块版本 patch
        std::uint16_t abiMin = 0;                   // 内核 ABI 下限
        std::uint16_t abiMax = 0;                   // 内核 ABI 上限
        std::vector<SectionInfo> sections;          // 段，按 MemoryOffset 升序
        std::vector<ExportInfo> exports;            // 导出接口条目
        std::vector<ImportInfo> imports;            // 导入接口条目
        std::vector<DependencyInfo> dependencies;   // 模块依赖条目
        std::vector<RelocationInfo> relocations;    // 重定位条目，按 Target 升序
    };

    // 按输入写出一个容器；布局顺序固定、无时间戳，同一输入产出同样字节
    Bytes Build(const Spec& spec);

    // 解析一个容器并复算全部容器约束；任何不符抛出 std::runtime_error，消息为英文
    Image Parse(std::span<const std::uint8_t> bytes);

    // 依赖图的一个节点：模块标识与它依赖的模块标识
    struct DependencyNode {
        ModuleId moduleId{};              // 本模块标识
        std::string name;                 // 诊断用的名字
        std::vector<ModuleId> dependencies; // 依赖的模块标识
    };

    // 检查模块依赖图的重复、自依赖、缺失与环；任何不成立抛出 std::runtime_error
    void VerifyDependencyGraph(std::span<const DependencyNode> nodes);

    // 判断接口稳定名是否符合命名语法：非空、不超过 64 字节、至少两个点分隔分量、每分量首字符
    // 大写且其余为字母或数字
    bool IsValidInterfaceName(std::string_view name) noexcept;

    // 判断模块逻辑名是否符合命名语法：非空、不超过 64 字节、首字符大写、其余为字母或数字
    bool IsValidModuleName(std::string_view name) noexcept;

    // 派生 InterfaceId：SHA-256("LPIF01" || 稳定名) 取前 16 字节，语法非法时抛出 std::runtime_error
    InterfaceId DeriveInterfaceId(std::string_view stableName);

    // 派生 ModuleId：SHA-256("LPMOD01" || 逻辑名) 取前 16 字节，语法非法时抛出 std::runtime_error
    ModuleId DeriveModuleId(std::string_view logicalName);

    // 把一段字节写成小写十六进制文本
    std::string HexText(std::span<const std::uint8_t> bytes);
}
