/* DependencyGraph.hpp
    模块依赖图：依赖表校验、依赖版本区间匹配与稳定拓扑排序

    依赖表条目：ModuleId + Min/Max 版本区间 + Flags；被依赖模块必须先完成 Init，
    环、缺失依赖与版本不匹配都判失败；同层顺序按“提供者先于消费者”，同层按
    Modules.list 预装顺序，顺序缺失时以 ModuleId 字节序兜底

    本模块只做数据与判定：不打印、不停机、不分配内存；失败返回原因文本与分类，
    编号由调用方（装载编排）按分类映射——容器/依赖表结构错归 E-KRN-57，
    依赖缺失、版本不匹配与环归 E-KRN-59（detail 用返回的原因文本）

    容量固定：模块数 ≤ 64、每模块依赖条目 ≤ 32（容器 DependencyCount 取值 0..32）；
    输入视图与 ModuleContainer.hpp 的容器视图对接：它的 ContainerDependency 与本文件的
    DependencyEntry 字段语义相同（容器视图没有 Flags 字段——解析器已校验并丢弃，映射时填 0），
    装载编排按字段填入即可；本模块不反向依赖容器解析器，两者可分别编译与测试
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <ModuleId.hpp>

namespace Kernel {
    namespace Dependency {
        // 一次启动参与依赖判定的模块数上限：引导闭包 13 + 运行期模块，留足余量
        constexpr uint32_t kMaxModules = 64;
        // 单个模块的依赖条目上限：容器 DependencyCount 取值 0..32
        constexpr uint32_t kMaxDependencies = 32;
        // 依赖条目的固定字节数：容器的 32 字节条目
        constexpr uint32_t kEntryBytes = 32;

        // 依赖条目视图：与容器的 32 字节依赖表条目字段一一对应
        struct DependencyEntry {
            ModuleId moduleId;    // 被依赖模块标识；全零拒绝，等于本模块标识（自依赖）拒绝
            uint16_t minMajor;    // 可接受的最低版本 major，非零
            uint16_t minMinor;    // 可接受的最低版本 minor
            uint16_t minPatch;    // 可接受的最低版本 patch
            uint16_t maxMajor;    // 可接受的最高版本 major；0 表示不设上限
            uint16_t maxMinor;    // 最高版本 minor；maxMajor 为 0 时忽略
            uint16_t maxPatch;    // 最高版本 patch；maxMajor 为 0 时忽略
            uint32_t flags;       // 必须为 0；当前不定义可选依赖
        };

        // 依赖条目的字节布局是容器契约：字段偏移与容器依赖表条目一致，尺寸由编译器填充钉死
        static_assert(sizeof(DependencyEntry) == kEntryBytes, "依赖条目必须与容器契约的 32 字节条目同长");
        static_assert(offsetof(DependencyEntry, minMajor) == 0x10, "依赖条目的 MinMajor 偏移已变");
        static_assert(offsetof(DependencyEntry, maxMajor) == 0x16, "依赖条目的 MaxMajor 偏移已变");
        static_assert(offsetof(DependencyEntry, flags) == 0x1C, "依赖条目的 Flags 偏移已变");

        // 图内一个模块：标识、版本，以及它声明的依赖条目
        struct ModuleNode {
            ModuleId moduleId;                 // 模块标识；全零或与其他节点重复都拒绝
            uint16_t major;                    // 模块版本 major
            uint16_t minor;                    // 模块版本 minor
            uint16_t patch;                    // 模块版本 patch
            const DependencyEntry* dependencies;   // 容器视图里的依赖条目数组；条目数为 0 时可为空
            uint32_t dependencyCount;          // 依赖条目数，取值 0..kMaxDependencies
            uint32_t preloadOrder;             // Modules.list 预装顺序 1..n；0 = 未提供，同层排到最后
        };

        // 依赖图输入：本次启动参与拓扑排序的全部模块
        struct GraphInput {
            const ModuleNode* modules;   // 模块数组
            uint32_t moduleCount;        // 模块数，取值 1..kMaxModules
        };

        // 拒绝分类：供调用方选编号
        // InvalidModuleList / InvalidDependencyList 是结构类（容器或集合非法，对应 E-KRN-57）；
        // MissingProvider / VersionMismatch / Cycle 是依赖解析类（对应 E-KRN-59）
        enum class Reject : uint8_t {
            None,                  // 通过
            InvalidModuleList,     // 节点数组非法：为空、超上限、标识全零或重复
            InvalidDependencyList, // 依赖表非法：超上限、不可读、标识全零、自依赖、区间倒置、标志非零、重复条目
            MissingProvider,       // 依赖的模块不在本次启动的图里（无提供者）
            VersionMismatch,       // 提供者在图里但版本不落在声明区间内
            Cycle,                 // 依赖成环
        };

        // 稳定拓扑排序结果：index 是输入 modules 数组的下标，按装载顺序排列
        struct TopologyResult {
            uint32_t count;                 // 排序后的模块数
            uint32_t index[kMaxModules];    // 每项是输入 modules 数组的下标
        };

        // 依赖表逐条校验：条目数上限、数组可读、标识全零、自依赖、最低主版本为零、
        // 区间倒置（MaxMajor 非零且低于 Min 的字典序）、标志非零、同一被依赖模块重复登记
        // 容器解析器解析依赖表时可以逐表调用；SortByDependency 内部复用同一份判据，不维护第二份
        // 通过返回空指针并把 reject 置 None；失败返回原因文本并把 reject 置 InvalidDependencyList
        const char* CheckDependencyTable(const ModuleId& selfModuleId, const DependencyEntry* entries, uint32_t count, Reject& reject);

        // 单条依赖的版本匹配（版本区间规则）
        // 无上限（MaxMajor == 0）：提供者 major == MinMajor 且 (Minor, Patch) >= (MinMinor, MinPatch)
        // 有上限：按 (Major, Minor, Patch) 字典序，提供者落在 [Min, Max] 闭区间内
        bool VersionMatches(const DependencyEntry& entry, uint16_t major, uint16_t minor, uint16_t patch);

        // 依赖图构建与稳定拓扑排序
        // 校验顺序：模块列表 → 每个模块的依赖表 → 依赖解析（无提供者 / 版本不匹配）→ 拓扑排序（环）
        // 通过返回空指针、result 填出装载顺序；失败返回原因文本（detail 用，不含标点）并带回分类
        // 稳定规则：每一步在“依赖已全部排入”的模块里取 (预装顺序, ModuleId 字节序) 最小的一个，
        // 因此输入顺序本身已满足依赖时输出与 Modules.list 预装顺序一致；预装顺序缺失（0）排到最后，
        // 以 ModuleId 字节序兜底——不按哈希值或输入数组下标排序
        const char* SortByDependency(const GraphInput& input, TopologyResult& result, Reject& reject);
    }
}
