/* DependencyGraph.cpp
    模块依赖图的实现：依赖表逐条校验、依赖解析（提供者与版本区间）与稳定拓扑排序

    依赖检查的判据：被依赖模块必须先完成 Init，版本落在声明区间内，环或缺失依赖即失败；
    失败只回报原因文本与分类，不打印、不停机、不分配内存——编号与停机由调用方决定
    排序是 Kahn 算法加固定优先级：每一步在“依赖已全部排入”的模块里取
    (Modules.list 预装顺序, ModuleId 字节序) 最小的一个，因此输入顺序已满足依赖时
    输出与预装顺序一致，不按哈希值或输入数组下标排序
*/

#include <DependencyGraph.hpp>

namespace {
    // 版本三元组字典序比较：返回 -1 / 0 / 1
    int CompareVersion(uint16_t major, uint16_t minor, uint16_t patch, uint16_t otherMajor, uint16_t otherMinor, uint16_t otherPatch) {
        if (major != otherMajor) return major < otherMajor ? -1 : 1;
        if (minor != otherMinor) return minor < otherMinor ? -1 : 1;
        if (patch != otherPatch) return patch < otherPatch ? -1 : 1;
        return 0;
    }

    // 模块标识字节序比较：按无符号字节逐字节比较，返回 -1 / 0 / 1
    int CompareId(const ModuleId& left, const ModuleId& right) {
        for (uint32_t index = 0; index < 16; ++index) {
            if (left.bytes[index] == right.bytes[index]) continue;
            return left.bytes[index] < right.bytes[index] ? -1 : 1;
        }
        return 0;
    }

    // 预装顺序的比较键：0 表示未提供，这种模块在同层里统一排到最后
    constexpr uint32_t kNoPreloadOrder = 0xFFFFFFFFu;

    // 取预装顺序的比较键
    uint32_t OrderKey(const Kernel::Dependency::ModuleNode& node) {
        return node.preloadOrder == 0 ? kNoPreloadOrder : node.preloadOrder;
    }

    // 在模块数组里按标识查下标；找不到返回 kMaxModules（下标上界之外，不会与真实下标撞）
    uint32_t FindModule(const Kernel::Dependency::GraphInput& input, const ModuleId& id) {
        for (uint32_t index = 0; index < input.moduleCount; ++index) {
            if (input.modules[index].moduleId == id) return index;
        }
        return Kernel::Dependency::kMaxModules;
    }
}

namespace Kernel {
    namespace Dependency {
        const char* CheckDependencyTable(const ModuleId& selfModuleId, const DependencyEntry* entries, uint32_t count, Reject& reject) {
            reject = Reject::InvalidDependencyList;
            if (count > kMaxDependencies) return "A module dependency table exceeds the entry limit";
            if (count != 0 && entries == nullptr) return "A module dependency table is unreadable";
            for (uint32_t index = 0; index < count; ++index) {
                const DependencyEntry& entry = entries[index];
                if (IsZero(entry.moduleId)) return "A module dependency has a zero module ID";
                if (entry.moduleId == selfModuleId) return "A module depends on itself";
                if (entry.minMajor == 0) return "A module dependency has a zero minimum major version";
                if (entry.flags != 0) return "A module dependency has unsupported flags";
                // 区间倒置：有上限且按 (Major, Minor, Patch) 字典序 Max < Min
                if (entry.maxMajor != 0
                    && CompareVersion(entry.maxMajor, entry.maxMinor, entry.maxPatch, entry.minMajor, entry.minMinor, entry.minPatch) < 0) {
                    return "A module dependency has an inverted version range";
                }
                // 同一被依赖模块只能登记一次：两个区间会让“满足哪一条”失去唯一答案
                for (uint32_t earlier = 0; earlier < index; ++earlier) {
                    if (entries[earlier].moduleId == entry.moduleId) return "A module dependency is listed twice";
                }
            }
            reject = Reject::None;
            return nullptr;
        }

        bool VersionMatches(const DependencyEntry& entry, uint16_t major, uint16_t minor, uint16_t patch) {
            // 先判不低于下限；无上限时还要求主版本与下限一致（不跨主版本“向下兼容”）
            if (CompareVersion(major, minor, patch, entry.minMajor, entry.minMinor, entry.minPatch) < 0) return false;
            if (entry.maxMajor == 0) return major == entry.minMajor;
            // 有上限：闭区间，含两端
            return CompareVersion(major, minor, patch, entry.maxMajor, entry.maxMinor, entry.maxPatch) <= 0;
        }

        const char* SortByDependency(const GraphInput& input, TopologyResult& result, Reject& reject) {
            reject = Reject::InvalidModuleList;
            result.count = 0;
            if (input.moduleCount == 0 || input.moduleCount > kMaxModules) return "The module graph has an invalid module count";
            if (input.modules == nullptr) return "The module graph is unreadable";

            // 1. 模块标识：全零拒绝，同一集内不得重复（一个 ModuleId 一次启动只有一个实例）
            for (uint32_t index = 0; index < input.moduleCount; ++index) {
                if (IsZero(input.modules[index].moduleId)) return "A module in the graph has a zero module ID";
                for (uint32_t earlier = 0; earlier < index; ++earlier) {
                    if (input.modules[earlier].moduleId == input.modules[index].moduleId) return "A module is listed twice in the graph";
                }
            }

            // 2. 依赖表逐表校验，并把每条依赖解析到提供者：提供者必须在本次启动的图里且版本区间匹配
            // indegree 是入度（依赖数）；dependedBy 记录“哪些模块依赖本模块”，供拓扑推进时减入度
            uint32_t indegree[kMaxModules] = {};
            uint64_t dependedBy[kMaxModules] = {};
            for (uint32_t index = 0; index < input.moduleCount; ++index) {
                const ModuleNode& node = input.modules[index];
                if (const char* reason = CheckDependencyTable(node.moduleId, node.dependencies, node.dependencyCount, reject); reason != nullptr) return reason;
                reject = Reject::MissingProvider;
                for (uint32_t dependencyIndex = 0; dependencyIndex < node.dependencyCount; ++dependencyIndex) {
                    const DependencyEntry& dependency = node.dependencies[dependencyIndex];
                    const uint32_t provider = FindModule(input, dependency.moduleId);
                    // 自依赖已在依赖表校验里拒绝，这里剩下的“查不到”就是本次启动没有提供者
                    if (provider == kMaxModules) return "A module dependency has no provider in this boot closure";
                    const ModuleNode& providerNode = input.modules[provider];
                    if (!VersionMatches(dependency, providerNode.major, providerNode.minor, providerNode.patch)) {
                        reject = Reject::VersionMismatch;
                        return "A module dependency version isn't satisfied by its provider";
                    }
                    dependedBy[provider] = dependedBy[provider] | (static_cast<uint64_t>(1) << index);
                    ++indegree[index];
                }
            }

            // 3. 稳定拓扑排序：每步在入度为零（依赖已排入）的模块里按 (预装顺序, ModuleId) 取最小
            // 预装顺序缺失的模块排到最后，以 ModuleId 字节序兜底；整图无入度节点即成环
            bool emitted[kMaxModules] = {};
            reject = Reject::Cycle;
            for (uint32_t step = 0; step < input.moduleCount; ++step) {
                uint32_t best = kMaxModules;
                for (uint32_t index = 0; index < input.moduleCount; ++index) {
                    if (emitted[index] || indegree[index] != 0) continue;
                    if (best == kMaxModules) { best = index; continue; }
                    const uint32_t candidateOrder = OrderKey(input.modules[index]);
                    const uint32_t currentOrder = OrderKey(input.modules[best]);
                    if (candidateOrder != currentOrder) {
                        if (candidateOrder < currentOrder) best = index;
                        continue;
                    }
                    if (CompareId(input.modules[index].moduleId, input.modules[best].moduleId) < 0) best = index;
                }
                if (best == kMaxModules) return "The module dependency graph has a cycle";
                emitted[best] = true;
                result.index[step] = best;
                // 依赖本模块的节点入度减一：本模块已排入，它们离“依赖满足”更近一步
                const uint64_t dependents = dependedBy[best];
                for (uint32_t dependent = 0; dependent < input.moduleCount; ++dependent) {
                    if ((dependents & (static_cast<uint64_t>(1) << dependent)) != 0) --indegree[dependent];
                }
            }

            result.count = input.moduleCount;
            reject = Reject::None;
            return nullptr;
        }
    }
}
