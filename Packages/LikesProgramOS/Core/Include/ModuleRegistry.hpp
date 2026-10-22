/* ModuleRegistry.hpp
    模块注册表与内核核心导出：已装载模块的状态、映像区间与导出登记，以及 Core.* 十张槽表

    已装载集合按 ModuleId 去重，Init 成功后才登记模块；状态机为 Loaded / Initialized /
    Failed；Core.* 槽位语义与入口 ABI 由生成头钉住

    本文件只做数据与登记：不打印、不停机。失败原因由调用方决定怎么报
    内核核心导出的槽表符号（_Contract_Export_<稳定名去点>_v1，如 _Contract_Export_CorePage_v1）
    在这里定义，供生成头 kKernelExports 链接；
    需要平台状态的槽（Core.Boot / Core.Page / Core.Abi 槽 1）经 BindCoreServices 绑定，
    未绑定时按“未就绪”返回空值或零，不猜测事实
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <Diagnostics.hpp>
#include <InterfaceRegistry.hpp>
#include <Memory.hpp>
#include <ModuleId.hpp>

namespace Kernel {
    // 模块装载状态：Loaded 表示映像与导出已就绪、Init 尚未成功
    enum class ModuleState : uint32_t {
        Empty = 0,        // 空槽
        Loaded = 1,       // 已映射、已重定位、已解析导入，尚未 Init 成功
        Initialized = 2,  // Init 成功并已发布导出
        Failed = 3,       // Init 失败或装载中失败（映像已回收，记录留作诊断）
    };

    // 模块记录：注册表按 ModuleId 索引
    struct ModuleRecord {
        ModuleId moduleId = {};       // 模块标识
        uint16_t major = 0;           // 模块版本 major
        uint16_t minor = 0;           // 模块版本 minor
        uint16_t patch = 0;           // 模块版本 patch
        uint64_t base = 0;            // 映像基址（物理地址；恒等映射下等于可直接调用的地址）
        uint32_t imageBytes = 0;      // 映像跨度
        uint32_t pageCount = 0;       // 映像占用的页数，回收时归还
        uint32_t preloadOrder = 0;    // Modules.list 预装顺序；0 = 未提供
        ModuleState state = ModuleState::Empty;   // 装载状态
        uint32_t exportFirst = 0;     // 本模块在导出条目池里的首条下标
        uint32_t exportCount = 0;     // 本模块登记的导出条数
    };

    // 容量：模块记录 64、导出条目 128（与注册表容量一致）、槽位指针 512
    constexpr uint32_t kModuleMaxRecords = 64;
    constexpr uint32_t kModuleMaxExportEntries = 128;
    constexpr uint32_t kModuleMaxSlotPointers = 512;

    // 导出池检查点：一次装载开始前取，失败回滚时按它回收
    struct ExportPoolMark {
        uint32_t exportCount = 0;   // 导出条目水位
        uint32_t slotCount = 0;     // 槽位指针水位
    };

    // 模块注册表：固定静态容量，装载期单线程使用；不做动态分配
    class ModuleRegistry {
    public:
        // 清空全部记录与导出池：内核初始化时调用一次
        void Reset();
        // 追加一条 Loaded 记录；表满或 ModuleId 已存在返回空
        ModuleRecord* Add(const ModuleId& moduleId, uint16_t major, uint16_t minor, uint16_t patch, uint64_t base, uint32_t imageBytes, uint32_t pageCount, uint32_t preloadOrder);
        // 按标识查找；找不到返回空
        ModuleRecord* Find(const ModuleId& moduleId);
        const ModuleRecord* Find(const ModuleId& moduleId) const;
        // 是否已登记（含 Failed 记录）：装载步骤 4 的“已装载集合不重复”用
        bool Contains(const ModuleId& moduleId) const;
        // 取一条记录；越界返回空
        const ModuleRecord* At(uint32_t index) const;
        // 已登记记录数
        uint32_t Count() const { return m_count; }
        // 修改状态；记录不存在返回 false
        bool SetState(const ModuleId& moduleId, ModuleState state);

        // 导出池：为一次登记预留槽位指针数组，返回数组起点；池满返回空
        const void** AllocateSlotPointers(uint32_t slotCount);
        // 导出池：为一次登记预留导出条目数组，返回数组起点；池满返回空
        ModuleExportEntry* AllocateExportEntries(uint32_t exportCount);
        // 取当前水位
        ExportPoolMark MarkPools() const;
        // 回滚到水位：撤销水位之后分配的导出条目与槽位指针
        void RollbackPools(const ExportPoolMark& mark);

    private:
        ModuleRecord m_records[kModuleMaxRecords] = {};                  // 模块记录，前 m_count 条有效
        uint32_t m_count = 0;                                            // 记录条数
        ModuleExportEntry m_exportEntries[kModuleMaxExportEntries] = {}; // 导出条目池
        uint32_t m_exportCount = 0;                                      // 导出条目水位
        const void* m_slotPointers[kModuleMaxSlotPointers] = {};         // 槽位指针池
        uint32_t m_slotCount = 0;                                        // 槽位指针水位
    };

    // 内核核心服务绑定：Core.Boot 的 BootInfo 与 Core.Page 的页帧分配器、Core.Abi 槽 1 的 BuildId
    // 在内核初始化时调用（InitializeCoreExports 会代调一次）；未绑定的槽按未就绪返回
    void BindCoreServices(PageAllocator* pages, const Boot::Info* bootInfo);

    // 内核核心导出初始化：清空注册表 → 登记生成头 kKernelExports → 运行时核对 Core.Abi
    // 任一步失败返回 E-KRN-59（ABI/契约失败并入此号，detail 区分失败项）；成功返回空编号
    // E-KRN-56 只留给 BuildId 与模块主版本硬门，不用于导出登记失败
    // 调用点：页帧分配器就绪、BootInfo 校验通过之后、装载任何模块之前
    Diagnostics::Diagnostic InitializeCoreExports(InterfaceRegistry& registry, PageAllocator* pages, const Boot::Info* bootInfo, const char*& reason);
}
