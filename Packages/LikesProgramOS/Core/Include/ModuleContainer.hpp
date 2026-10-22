/* ModuleContainer.hpp
    内核模块容器（.osm）的有界解析：头、段表、导出 / 导入 / 依赖 / 重定位 / 字符串表

    本文件独立实现容器字节布局，不链接宿主工具、不依赖 Storage / Print：输入是容器字节
    指针与长度，输出是固定上界的只读视图，所有偏移与长度先做 64 位加法溢出检查再比较，
    不按容器内容长度分配内存
    解析只做容器格式与表项自洽校验：版本、对齐、重叠、
    越界、段类型与权限组合、BSS、入口落在 TEXT、槽位 / 回填槽 / 重定位目标的可写与对齐、
    依赖不自依赖且区间不颠倒；不解析段文件内容、不映射、不重定位、不复算摘要
    视图里的指针都指向调用方给出的容器字节，调用方必须保证字节在视图使用期间保持有效

    失败返回 ContainerError 分类，原因文本由 ContainerErrorText 给出；装载器统一映射到
    E-KRN-57 并把文本作为 detail，本文件不打印、不停机
*/

#pragma once
#include <stdint.h>

#include <InterfaceId.hpp>
#include <ModuleId.hpp>

namespace Kernel {
    // 容器格式固定值
    constexpr uint16_t kContainerVersion = 1;            // 当前格式版本
    constexpr uint32_t kContainerHeaderBytes = 0x80;     // 文件头长度
    constexpr uint32_t kContainerPageBytes = 0x1000;     // 段内存偏移与跨度的对齐粒度
    constexpr uint32_t kContainerMaxTotalBytes = 0x4000000; // 文件总长上限 64 MiB
    // 数量上限：与容器格式一致
    constexpr uint32_t kContainerMaxSections = 16;       // 段数 1..16
    constexpr uint32_t kContainerMaxExports = 128;       // 导出条目 0..128
    constexpr uint32_t kContainerMaxImports = 128;       // 导入条目 0..128
    constexpr uint32_t kContainerMaxDependencies = 32;   // 依赖条目 0..32
    constexpr uint32_t kContainerMaxRelocations = 65536; // 重定位条目 0..65536
    constexpr uint32_t kContainerMaxSlots = 64;          // 导出槽数 / 导入使用槽数 1..64
    // 表区条目长度
    constexpr uint32_t kContainerSectionEntryBytes = 32;
    constexpr uint32_t kContainerExportEntryBytes = 28;
    constexpr uint32_t kContainerImportEntryBytes = 28;
    constexpr uint32_t kContainerDependencyEntryBytes = 32;
    constexpr uint32_t kContainerRelocationEntryBytes = 16;
    // 版本区间上限的固定取值
    constexpr uint16_t kContainerAbiMaxUnlimited = 0xFFFF;   // KernelAbiMax 不设上限
    constexpr uint16_t kContainerVersionUnlimited = 0xFFFF;  // 导入 MaxVersion 不设上限
    constexpr uint16_t kContainerMajorUnlimited = 0;         // 依赖 MaxMajor 不设上限

    // 段类型：容器格式的四种取值
    enum class ContainerSectionType : uint32_t {
        Text = 1,     // 可执行代码，R|X
        Rodata = 2,   // 只读数据，R
        Data = 3,     // 可写数据，R|W
        Bss = 4,      // 零初始化数据，R|W，不占文件字节
    };

    // 段权限位
    constexpr uint32_t kContainerFlagRead = 1;
    constexpr uint32_t kContainerFlagWrite = 2;
    constexpr uint32_t kContainerFlagExecute = 4;

    // 重定位类型：容器格式的两种取值
    enum class ContainerRelocationType : uint8_t {
        Relative64 = 1,   // 在内存偏移处写入 8 字节 Base + Addend
        Relative32 = 2,   // 在内存偏移处写入 4 字节 Base + Addend
    };

    // 解析失败分类：全部映射到 E-KRN-57，文本作 detail；分类只增不改
    enum class ContainerError : uint8_t {
        None = 0,              // 成功
        NullPointer,           // 容器字节指针为空
        TooShort,              // 短于 0x80 字节文件头
        HeaderMagic,           // 魔数不符
        HeaderVersion,         // 未知容器版本
        HeaderBytes,           // HeaderBytes 不是 0x80
        HeaderFlags,           // Flags 非零
        HeaderTotalBytes,      // TotalBytes 与实际长度不符或超 64 MiB
        HeaderImageBytes,      // ImageBytes 不是 4 KiB 整数倍
        HeaderReserved,        // 头部保留字段非零
        SectionCount,          // 段数不在 1..16
        SectionTableOffset,    // 段表偏移不对齐或越界
        SectionTableBounds,    // 段表区越出文件
        SectionEntryBytes,     // SectionEntryBytes 不是 32
        SectionType,           // 未知段类型
        SectionFlags,          // 段权限组合与类型不符（含 W+X）
        SectionAlign,          // 段对齐不是 4..4096 的 2 的幂
        SectionMemoryOffset,   // MemoryOffset 不是 4 KiB 整数倍
        SectionMemoryBytes,    // MemoryBytes 不是 4 KiB 整数倍或为零
        SectionMemoryRange,    // MemoryOffset + MemoryBytes 越过 ImageBytes
        SectionFileOffset,     // 段文件偏移不对齐
        SectionFileBytes,      // BSS 带文件字节或非 BSS 无文件字节 / 超过 MemoryBytes
        SectionFileRange,      // 段文件区间越出文件或落在 0x80 之前
        SectionName,           // 段名偏移越出字符串表
        SectionMemoryOverlap,  // 两个段的内存区间重叠
        RegionOverlap,         // 表区、槽位表、字符串表与段文件区重叠
        EntryInit,             // EntryInit 为零或不在 TEXT 段
        EntryActivate,         // EntryActivate 不在 TEXT 段
        EntryAbort,            // EntryAbort 不在 TEXT 段
        ExportCount,           // 导出条目数超过 128
        ExportTableOffset,     // 导出表偏移不对齐或越界
        ExportTableBounds,     // 导出表区越出文件
        ExportEntry,           // 导出条目字段非法（标识 / 版本 / 保留）
        ExportSlotCount,       // 导出槽数不在 1..64
        ExportSlotTableOffset, // 槽位表偏移不对齐或越界
        ExportSlotTableBounds, // 槽位表区越出文件
        ExportSlot,            // 槽位值不在 TEXT 段
        ExportDuplicate,       // 同 InterfaceId + Version 重复
        ImportCount,           // 导入条目数超过 128
        ImportEntryBytes,      // ImportEntryBytes 不是 28
        ImportTableOffset,     // 导入表偏移不对齐或越界
        ImportTableBounds,     // 导入表区越出文件
        ImportEntry,           // 导入条目字段非法（标识 / 版本区间 / 保留）
        ImportSlotCount,       // 导入使用槽数不在 1..64
        ImportSlotTableOffset, // 回填表偏移不对齐或越界
        ImportSlotTableBounds, // 回填表区越出文件
        ImportSlot,            // 回填槽不在可写段或未 8 字节对齐
        ImportDuplicate,       // 同 InterfaceId 重复
        DependencyCount,       // 依赖条目数超过 32
        DependencyEntryBytes,  // DependencyEntryBytes 不是 32
        DependencyTableOffset, // 依赖表偏移不对齐或越界
        DependencyTableBounds, // 依赖表区越出文件
        DependencyEntry,       // 依赖条目字段非法（标识 / 主版本 / Flags / 区间）
        DependencyDuplicate,   // 同 ModuleId 重复
        RelocationCount,       // 重定位条目数超过 65536
        RelocationTableOffset, // 重定位表偏移不对齐或越界
        RelocationTableBounds, // 重定位表区越出文件
        RelocationEntry,       // 重定位类型、保留字段、目标可写性或对齐非法
        StringTableOffset,     // 字符串表偏移越界
        StringTableBounds,     // 字符串表区越出文件
        StringTableNul,        // 字符串表末字节不是 NUL
        ModuleName,            // 模块名偏移 / 字节数不一致或越出字符串表
        ModuleId,              // ModuleId 全零
        ModuleVersion,         // ModuleVersionMajor 为零
        KernelAbi,             // KernelAbiMin 为零
    };

    // 解析出的一个段：字段与段表 32 字节条目一一对应
    struct ContainerSection {
        const uint8_t* name = nullptr;   // 段名起点（字符串表内），空表示匿名
        uint32_t nameBytes = 0;          // 段名字节数，不含 NUL
        uint32_t type = 0;               // 段类型，取值同 ContainerSectionType
        uint32_t flags = 0;              // 权限位
        uint32_t fileOffset = 0;         // 文件偏移
        uint32_t fileBytes = 0;          // 文件内字节数
        uint32_t memoryOffset = 0;       // 相对模块基址的内存偏移
        uint32_t memoryBytes = 0;        // 内存跨度
        uint32_t align = 0;              // 段对齐
    };

    // 解析出的一个导出接口条目
    struct ContainerExport {
        InterfaceId interfaceId = {};    // 接口稳定标识
        uint16_t version = 0;            // 接口版本
        uint16_t slotCount = 0;          // 槽位数
        const uint8_t* slots = nullptr;  // slotCount 个 u32 槽位值起点（容器字节内）
    };

    // 解析出的一个导入接口条目
    struct ContainerImport {
        InterfaceId interfaceId = {};      // 接口稳定标识
        uint16_t minVersion = 0;           // 可接受的最低版本
        uint16_t maxVersion = 0;           // 可接受的最高版本；0xFFFF 表示不设上限
        uint16_t slotCount = 0;            // 使用槽位数
        const uint8_t* patchSlots = nullptr; // slotCount 个 u32 回填槽内存偏移起点
    };

    // 解析出的一个模块依赖条目
    struct ContainerDependency {
        ModuleId moduleId = {};   // 被依赖模块标识
        uint16_t minMajor = 0;    // 可接受的最低 major
        uint16_t minMinor = 0;    // 可接受的最低 minor
        uint16_t minPatch = 0;    // 可接受的最低 patch
        uint16_t maxMajor = 0;    // 可接受的最高 major；0 表示不设上限
        uint16_t maxMinor = 0;    // 最高 minor
        uint16_t maxPatch = 0;    // 最高 patch
    };

    // 解析出的一个重定位条目：映射层按索引读取，不建数组
    struct ContainerRelocation {
        uint8_t type = 0;        // 重定位类型，取值同 ContainerRelocationType
        uint32_t target = 0;     // 相对模块基址的内存偏移
        uint64_t addend = 0;     // 加到基址上的加数
    };

    // 容器视图：固定上界数组，指针指向容器字节；只有解析成功时才可用
    struct ContainerView {
        const uint8_t* bytes = nullptr;   // 容器起点
        uint32_t bytesCount = 0;          // 容器总长度
        uint32_t imageBytes = 0;          // 装载映像跨度
        uint32_t entryInit = 0;           // Init 入口偏移，非零且落在 TEXT
        uint32_t entryActivate = 0;       // Activate 入口偏移；0 表示没有
        uint32_t entryAbort = 0;          // Abort 入口偏移；0 表示没有
        ModuleId moduleId = {};           // 模块稳定标识
        const uint8_t* moduleName = nullptr; // 人类可读模块名起点，空表示没有
        uint16_t moduleNameBytes = 0;     // 模块名字节数，不含 NUL
        uint16_t versionMajor = 0;        // 模块版本 major
        uint16_t versionMinor = 0;        // 模块版本 minor
        uint16_t versionPatch = 0;        // 模块版本 patch
        uint16_t abiMin = 0;              // 内核 ABI 下限
        uint16_t abiMax = 0;              // 内核 ABI 上限；0xFFFF 表示不设上限
        uint32_t sectionCount = 0;        // 段数
        ContainerSection sections[kContainerMaxSections] = {};       // 段，按内存偏移升序
        uint32_t exportCount = 0;         // 导出条目数
        ContainerExport exports[kContainerMaxExports] = {};          // 导出条目
        uint32_t importCount = 0;         // 导入条目数
        ContainerImport imports[kContainerMaxImports] = {};          // 导入条目
        uint32_t dependencyCount = 0;     // 依赖条目数
        ContainerDependency dependencies[kContainerMaxDependencies] = {}; // 依赖条目
        uint32_t relocationCount = 0;     // 重定位条目数，条目本身用 ReadContainerRelocation 读取
        uint32_t relocationTableOffset = 0; // 重定位表文件偏移，ReadContainerRelocation 用
    };

    // 有界解析并校验一个容器；成功返回 None 并填 view，失败返回分类且 view 不保证可用
    // 调用方保证 bytes 在 view 使用期间有效；bytesCount 是容器实际字节数
    ContainerError ParseModuleContainer(const uint8_t* bytes, uint32_t bytesCount, ContainerView& view);

    // 按索引读取一个重定位条目；索引越界返回 RelocationEntry
    ContainerError ReadContainerRelocation(const ContainerView& view, uint32_t index, ContainerRelocation& relocation);

    // 读取导出条目的第 slot 个槽位偏移；索引越界返回 false
    bool ContainerExportSlot(const ContainerExport& entry, uint32_t slot, uint32_t& offset);

    // 读取导入条目的第 slot 个回填槽内存偏移；索引越界返回 false
    bool ContainerImportSlot(const ContainerImport& entry, uint32_t slot, uint32_t& offset);

    // 失败分类的英文原因文本：给 E-KRN-57 的 detail 用，不带标点
    const char* ContainerErrorText(ContainerError error);
}
