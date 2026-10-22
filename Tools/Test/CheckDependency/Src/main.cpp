/* main.cpp
    CheckDependency：模块依赖表校验、版本区间匹配与稳定拓扑排序的宿主侧检查，手工执行

    检查对象是 Packages/LikesProgramOS/Core/Src/DependencyGraph.cpp 的目标侧源码：
    用 vpath 按相对路径直接编进本工具，不复制实现；宿主 C 运行库提供 memcmp / memcpy 一类的
    基础符号，内核侧 Libc.cpp 不链接
    正例用真实的引导闭包模块标识（Packages/Common/Out/Contract/ModuleId.hpp 的生成常量）与
    闭包内真实的依赖图构造；负例逐项破坏依赖表与图结构，按拒绝分类断言，不按文本子串断言

    用例数固定，失败返回 1，成功打印计数
*/

#include <DependencyGraph.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace Dep = Kernel::Dependency;
    using DependencyEntry = Kernel::Dependency::DependencyEntry;
    using ModuleNode = Kernel::Dependency::ModuleNode;
    using TopologyResult = Kernel::Dependency::TopologyResult;
    using Reject = Kernel::Dependency::Reject;

    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 24;
    // 已跑用例数
    unsigned g_ran = 0;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 模块版本三元组
    struct Version {
        uint16_t major = 1;   // major
        uint16_t minor = 0;   // minor
        uint16_t patch = 0;   // patch
    };

    // 合成的模块标识：16 字节都填同一个非零值，便于在失败信息里分辨
    ModuleId Tag(uint8_t tag) {
        ModuleId id{};
        for (uint32_t index = 0; index < 16; ++index) id.bytes[index] = tag;
        return id;
    }

    // 标识的短文本（取首字节的十六进制），只用于失败信息
    std::string IdText(const ModuleId& id) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        for (uint32_t index = 0; index < 16; ++index) {
            text.push_back(kDigits[id.bytes[index] >> 4]);
            text.push_back(kDigits[id.bytes[index] & 0x0F]);
        }
        return text;
    }

    // 依赖条目：显式区间，max 的 major 为 0 表示不设上限
    DependencyEntry Dependency(const ModuleId& moduleId, Version minVersion, Version maxVersion, uint16_t flags = 0) {
        DependencyEntry entry{};
        entry.moduleId = moduleId;
        entry.minMajor = minVersion.major;
        entry.minMinor = minVersion.minor;
        entry.minPatch = minVersion.patch;
        entry.maxMajor = maxVersion.major;
        entry.maxMinor = maxVersion.minor;
        entry.maxPatch = maxVersion.patch;
        entry.flags = flags;
        return entry;
    }

    // 无上限依赖：同主版本、minor/patch 不低于下限
    DependencyEntry Upward(const ModuleId& moduleId, Version minVersion) { return Dependency(moduleId, minVersion, Version{0, 0, 0}); }

    // 夹具里的一个模块规格：标识、版本、预装顺序与依赖条目
    struct NodeSpec {
        ModuleId moduleId;                      // 模块标识
        Version version;                        // 模块版本
        uint32_t preloadOrder = 0;              // Modules.list 预装顺序；0 = 未提供
        std::vector<DependencyEntry> dependencies;   // 依赖条目
    };

    // 由图规格构建模块数组：依赖条目的指针指向 specs 内部的 vector，调用期间 specs 不得增删
    std::vector<ModuleNode> BuildNodes(std::vector<NodeSpec>& specs) {
        std::vector<ModuleNode> nodes(specs.size());
        for (std::size_t index = 0; index < specs.size(); ++index) {
            nodes[index].moduleId = specs[index].moduleId;
            nodes[index].major = specs[index].version.major;
            nodes[index].minor = specs[index].version.minor;
            nodes[index].patch = specs[index].version.patch;
            nodes[index].dependencies = specs[index].dependencies.empty() ? nullptr : specs[index].dependencies.data();
            nodes[index].dependencyCount = static_cast<uint32_t>(specs[index].dependencies.size());
            nodes[index].preloadOrder = specs[index].preloadOrder;
        }
        return nodes;
    }

    // 跑一次依赖排序：返回失败原因文本（成功为 nullptr）
    const char* Sort(std::vector<NodeSpec>& specs, TopologyResult& result, Reject& reject) {
        std::vector<ModuleNode> nodes = BuildNodes(specs);
        const Dep::GraphInput input{nodes.data(), static_cast<uint32_t>(nodes.size())};
        return Dep::SortByDependency(input, result, reject);
    }

    // 在规格里按标识找下标；找不到返回 specs.size()
    std::size_t IndexOf(const std::vector<NodeSpec>& specs, const ModuleId& moduleId) {
        for (std::size_t index = 0; index < specs.size(); ++index) {
            if (specs[index].moduleId == moduleId) return index;
        }
        return specs.size();
    }

    // 断言排序结果就是给定的标识序列
    void RequireOrder(const std::vector<NodeSpec>& specs, const TopologyResult& result, const std::vector<ModuleId>& expected, const char* what) {
        Require(result.count == expected.size(), std::string(what) + "：排序数量不符");
        for (uint32_t step = 0; step < result.count; ++step) {
            const ModuleId& actual = specs[result.index[step]].moduleId;
            Require(actual == expected[step], std::string(what) + "：第 " + std::to_string(step + 1) + " 位不符，实际 " + IdText(actual));
        }
    }

    // 断言“提供者先于消费者”：排序结果里每条依赖的提供者位置都在消费者之前
    void RequireProvidersFirst(const std::vector<NodeSpec>& specs, const TopologyResult& result) {
        std::vector<uint32_t> position(specs.size(), 0);
        for (uint32_t step = 0; step < result.count; ++step) position[result.index[step]] = step;
        for (std::size_t consumer = 0; consumer < specs.size(); ++consumer) {
            for (const DependencyEntry& dependency : specs[consumer].dependencies) {
                const std::size_t provider = IndexOf(specs, dependency.moduleId);
                Require(provider != specs.size(), "提供者不在图里却排了序");
                Require(position[provider] < position[consumer], "提供者必须排在消费者之前");
            }
        }
    }

    // 断言一次判定被拒绝且分类正确
    // 用宏而不是函数：调用表达式与分类变量是同一次判定的产物，函数实参的求值顺序不保证
    // 分类变量在调用之后才被读出——那就可能读成调用前的旧值，宏按语句顺序求值
#define REQUIRE_REJECT(callExpression, rejectVariable, expected, what) \
    do { \
        const char* rejectReason = (callExpression); \
        Require(rejectReason != nullptr, std::string(what) + "：应被拒绝"); \
        Require((rejectVariable) == (expected), std::string(what) + "：拒绝分类不符（原因：" + rejectReason + "）"); \
    } while (false)

    // —— 正例：依赖图与稳定排序 ——

    // 引导闭包 13 模块按闭包内真实的依赖关系构造：预装顺序本身已满足依赖，输出必须与预装顺序一致
    void CaseBootClosureOrder() {
        std::vector<NodeSpec> specs;
        const auto add = [&specs](const ModuleId& id, uint32_t order, const std::vector<DependencyEntry>& dependencies) {
            NodeSpec node;
            node.moduleId = id;
            node.preloadOrder = order;
            node.dependencies = dependencies;
            specs.push_back(node);
        };
        add(kModDevModel, 1, {});
        add(kModDriverLoader, 2, {Upward(kModDevModel, Version{1, 0, 0})});
        add(kModAtaPio, 3, {Upward(kModDevModel, Version{1, 0, 0}), Upward(kModDriverLoader, Version{1, 0, 0})});
        add(kModAhci, 4, {Upward(kModDevModel, Version{1, 0, 0}), Upward(kModDriverLoader, Version{1, 0, 0})});
        add(kModNvme, 5, {Upward(kModDevModel, Version{1, 0, 0}), Upward(kModDriverLoader, Version{1, 0, 0})});
        add(kModUsbCore, 6, {Upward(kModDevModel, Version{1, 0, 0}), Upward(kModDriverLoader, Version{1, 0, 0})});
        add(kModXhci, 7, {Upward(kModUsbCore, Version{1, 0, 0})});
        add(kModUsbHub, 8, {Upward(kModXhci, Version{1, 0, 0})});
        add(kModUsbStorage, 9, {Upward(kModUsbHub, Version{1, 0, 0})});
        add(kModVfs, 10, {Upward(kModDevModel, Version{1, 0, 0})});
        add(kModExt4, 11, {Upward(kModVfs, Version{1, 0, 0})});
        add(kModISO9660, 12, {Upward(kModVfs, Version{1, 0, 0})});
        add(kModFAT32File, 13, {Upward(kModVfs, Version{1, 0, 0})});

        TopologyResult result{};
        Reject reject = Reject::None;
        const char* reason = Sort(specs, result, reject);
        Require(reason == nullptr, std::string("闭包依赖图应通过：") + (reason == nullptr ? "" : reason));
        Require(reject == Reject::None, "通过的图拒绝分类应为 None");
        const std::vector<ModuleId> expected = {
            kModDevModel, kModDriverLoader, kModAtaPio, kModAhci, kModNvme, kModUsbCore, kModXhci,
            kModUsbHub, kModUsbStorage, kModVfs, kModExt4, kModISO9660, kModFAT32File,
        };
        RequireOrder(specs, result, expected, "闭包输出必须与 Modules.list 预装顺序一致");
        RequireProvidersFirst(specs, result);
    }

    // 同输入两次结果一致，且与输入数组的存放顺序无关（排序键是预装顺序与模块标识，不是数组下标）
    void CaseDeterministic() {
        std::vector<NodeSpec> specs;
        for (uint32_t index = 0; index < 13; ++index) {
            NodeSpec node;
            node.moduleId = Tag(static_cast<uint8_t>(0x30 + index));
            node.preloadOrder = index + 1;
            specs.push_back(node);
        }
        TopologyResult first{};
        TopologyResult second{};
        Reject firstReject = Reject::None;
        Reject secondReject = Reject::None;
        Require(Sort(specs, first, firstReject) == nullptr, "第一次排序应通过");
        Require(Sort(specs, second, secondReject) == nullptr, "第二次排序应通过");
        Require(first.count == second.count, "两次排序数量必须一致");
        for (uint32_t step = 0; step < first.count; ++step) Require(first.index[step] == second.index[step], "两次排序结果必须逐项一致");
        // 反转数组存放顺序：结果按标识序列必须不变
        std::vector<ModuleId> before;
        for (uint32_t step = 0; step < first.count; ++step) before.push_back(specs[first.index[step]].moduleId);
        std::vector<NodeSpec> reversed(specs.rbegin(), specs.rend());
        TopologyResult reversedResult{};
        Reject reversedReject = Reject::None;
        Require(Sort(reversed, reversedResult, reversedReject) == nullptr, "反转存放顺序后排序应通过");
        for (uint32_t step = 0; step < reversedResult.count; ++step) {
            Require(reversed[reversedResult.index[step]].moduleId == before[step], "排序结果不得随输入数组的存放顺序变化");
        }
    }

    // 同层（都无依赖）按 Modules.list 预装顺序：顺序号小的先排
    void CaseSameLayerPreloadOrder() {
        std::vector<NodeSpec> specs;
        NodeSpec late;
        late.moduleId = Tag(0x22);
        late.preloadOrder = 2;
        specs.push_back(late);
        NodeSpec early;
        early.moduleId = Tag(0x11);
        early.preloadOrder = 1;
        specs.push_back(early);
        TopologyResult result{};
        Reject reject = Reject::None;
        Require(Sort(specs, result, reject) == nullptr, "同层排序应通过");
        RequireOrder(specs, result, {Tag(0x11), Tag(0x22)}, "同层必须按预装顺序");
    }

    // 预装顺序缺失（0）排到最后，并以 ModuleId 字节序兜底；已知顺序的模块无论标识大小都排在前
    void CaseOrderFallbackById() {
        std::vector<NodeSpec> specs;
        NodeSpec unknownSmall;
        unknownSmall.moduleId = Tag(0x01);   // 标识最小，但没有预装顺序
        specs.push_back(unknownSmall);
        NodeSpec known;
        known.moduleId = Tag(0x99);          // 标识大，但有预装顺序
        known.preloadOrder = 5;
        specs.push_back(known);
        NodeSpec unknownLarge;
        unknownLarge.moduleId = Tag(0x80);
        specs.push_back(unknownLarge);
        TopologyResult result{};
        Reject reject = Reject::None;
        Require(Sort(specs, result, reject) == nullptr, "缺预装顺序的排序应通过");
        RequireOrder(specs, result, {Tag(0x99), Tag(0x01), Tag(0x80)}, "已知预装顺序在前，缺失时按标识字节序兜底");
    }

    // 提供者先于消费者：链上预装顺序是反的也要按依赖排出提供者在前
    void CaseProviderBeforeConsumer() {
        std::vector<NodeSpec> specs;
        NodeSpec consumer;
        consumer.moduleId = Tag(0x31);
        consumer.preloadOrder = 1;   // 预装顺序最小，但依赖 0x32
        consumer.dependencies.push_back(Upward(Tag(0x32), Version{1, 0, 0}));
        specs.push_back(consumer);
        NodeSpec provider;
        provider.moduleId = Tag(0x32);
        provider.preloadOrder = 2;
        provider.version = Version{1, 4, 2};
        specs.push_back(provider);
        TopologyResult result{};
        Reject reject = Reject::None;
        Require(Sort(specs, result, reject) == nullptr, "链式排序应通过");
        RequireOrder(specs, result, {Tag(0x32), Tag(0x31)}, "提供者必须先于消费者");
        RequireProvidersFirst(specs, result);
    }

    // —— 依赖表结构拒绝：自依赖 / 全零标识 / 最低主版本为零 / 区间倒置 / 标志非零 / 重复 / 超限 / 不可读 ——

    // 自依赖：依赖自己的 ModuleId 必须拒绝
    void CaseSelfDependency() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        node.dependencies.push_back(Upward(Tag(0x11), Version{1, 0, 0}));
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::InvalidDependencyList, "自依赖");
    }

    // 依赖条目的 ModuleId 全零：按依赖表不合法拒绝
    void CaseZeroDependencyId() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        node.dependencies.push_back(Upward(Tag(0x00), Version{1, 0, 0}));
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::InvalidDependencyList, "依赖标识全零");
    }

    // 最低主版本为零：MinMajor 必须非零
    void CaseZeroMinMajor() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        DependencyEntry dependency = Upward(Tag(0x22), Version{0, 1, 0});
        node.dependencies.push_back(dependency);
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::InvalidDependencyList, "最低主版本为零");
    }

    // 区间倒置：有上限且 Max < Min 必须拒绝
    void CaseInvertedRange() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        node.dependencies.push_back(Dependency(Tag(0x22), Version{1, 5, 0}, Version{1, 4, 9}));
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::InvalidDependencyList, "区间倒置");
    }

    // 依赖条目标志非零：当前不定义可选依赖
    void CaseDependencyFlags() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        node.dependencies.push_back(Dependency(Tag(0x22), Version{1, 0, 0}, Version{0, 0, 0}, 1));
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::InvalidDependencyList, "依赖标志非零");
    }

    // 重复依赖：同一被依赖模块登记两条（哪怕区间不同）必须拒绝
    void CaseDuplicateDependency() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        node.dependencies.push_back(Upward(Tag(0x22), Version{1, 0, 0}));
        node.dependencies.push_back(Dependency(Tag(0x22), Version{1, 2, 0}, Version{1, 3, 0}));
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::InvalidDependencyList, "重复依赖");
    }

    // 依赖条目超上限（33 条）：容器 DependencyCount 上限 32
    void CaseDependencyLimit() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        for (uint32_t index = 0; index < Dep::kMaxDependencies + 1; ++index) {
            node.dependencies.push_back(Upward(Tag(static_cast<uint8_t>(0x40 + index)), Version{1, 0, 0}));
        }
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::InvalidDependencyList, "依赖条目超上限");
    }

    // 依赖表不可读：条目数非零但指针为空
    void CaseDependencyUnreadable() {
        ModuleNode node{};
        node.moduleId = Tag(0x11);
        node.dependencies = nullptr;
        node.dependencyCount = 4;
        const Dep::GraphInput input{&node, 1};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Dep::SortByDependency(input, result, reject), reject, Reject::InvalidDependencyList, "依赖表不可读");
    }

    // —— 图结构拒绝：模块数为零 / 超上限 / 标识全零 / 标识重复 ——

    // 模块数非法：0 与超过 kMaxModules 都拒绝
    void CaseModuleCountInvalid() {
        TopologyResult result{};
        Reject reject = Reject::None;
        const Dep::GraphInput empty{nullptr, 0};
        REQUIRE_REJECT(Dep::SortByDependency(empty, result, reject), reject, Reject::InvalidModuleList, "模块数为零");
        std::vector<ModuleNode> nodes(Dep::kMaxModules + 1);
        for (uint32_t index = 0; index < nodes.size(); ++index) nodes[index].moduleId = Tag(static_cast<uint8_t>(0x40 + index));
        const Dep::GraphInput tooMany{nodes.data(), static_cast<uint32_t>(nodes.size())};
        REQUIRE_REJECT(Dep::SortByDependency(tooMany, result, reject), reject, Reject::InvalidModuleList, "模块数超上限");
    }

    // 图内模块标识全零：模块侧的 ModuleId 全零非法
    void CaseZeroModuleId() {
        ModuleNode node{};
        node.moduleId = Tag(0x00);
        const Dep::GraphInput input{&node, 1};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Dep::SortByDependency(input, result, reject), reject, Reject::InvalidModuleList, "模块标识全零");
    }

    // 图内模块标识重复：同一 ModuleId 一次启动只能有一个实例
    void CaseDuplicateModule() {
        std::vector<ModuleNode> nodes(2);
        nodes[0].moduleId = Tag(0x11);
        nodes[1].moduleId = Tag(0x11);
        const Dep::GraphInput input{nodes.data(), static_cast<uint32_t>(nodes.size())};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Dep::SortByDependency(input, result, reject), reject, Reject::InvalidModuleList, "模块标识重复");
    }

    // —— 依赖解析拒绝：缺失提供者与版本不匹配 ——

    // 缺失提供者：依赖的模块不在本次启动的图里
    void CaseMissingProvider() {
        NodeSpec node;
        node.moduleId = Tag(0x11);
        node.dependencies.push_back(Upward(Tag(0x55), Version{1, 0, 0}));
        std::vector<NodeSpec> specs{node};
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::MissingProvider, "缺失提供者");
    }

    // 版本不匹配（低于下限）：提供者 (1.2.0) 不满足最低 (1.3.0)
    void CaseVersionBelowMinimum() {
        std::vector<NodeSpec> specs;
        NodeSpec consumer;
        consumer.moduleId = Tag(0x11);
        consumer.dependencies.push_back(Upward(Tag(0x22), Version{1, 3, 0}));
        specs.push_back(consumer);
        NodeSpec provider;
        provider.moduleId = Tag(0x22);
        provider.version = Version{1, 2, 0};
        specs.push_back(provider);
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::VersionMismatch, "版本低于下限");
    }

    // 版本不匹配（无上限时跨主版本）：提供者 major 2 不满足无上限依赖的 major 1
    void CaseVersionMajorMismatch() {
        std::vector<NodeSpec> specs;
        NodeSpec consumer;
        consumer.moduleId = Tag(0x11);
        consumer.dependencies.push_back(Upward(Tag(0x22), Version{1, 0, 0}));
        specs.push_back(consumer);
        NodeSpec provider;
        provider.moduleId = Tag(0x22);
        provider.version = Version{2, 0, 0};
        specs.push_back(provider);
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::VersionMismatch, "无上限依赖不得跨主版本");
    }

    // 版本区间匹配规则：无上限、有上限闭区间与上下边界逐项核对
    void CaseVersionMatchesRules() {
        const ModuleId id = Tag(0x22);
        // 无上限：major 相等且 (Minor, Patch) >= 下限
        const DependencyEntry upward = Upward(id, Version{1, 2, 3});
        Require(Dep::VersionMatches(upward, 1, 2, 3), "无上限：等于下限应匹配");
        Require(Dep::VersionMatches(upward, 1, 2, 4), "无上限：patch 更高应匹配");
        Require(Dep::VersionMatches(upward, 1, 9, 0), "无上限：minor 更高应匹配");
        Require(!Dep::VersionMatches(upward, 1, 2, 2), "无上限：patch 低于下限不应匹配");
        Require(!Dep::VersionMatches(upward, 1, 1, 9), "无上限：minor 低于下限不应匹配");
        Require(!Dep::VersionMatches(upward, 0, 9, 9), "无上限：主版本低于下限不应匹配");
        Require(!Dep::VersionMatches(upward, 2, 0, 0), "无上限：主版本高于下限不应匹配");
        // 有上限：取 [Min, Max] 闭区间（字典序）；区间跨主版本时，上限内的高主版本也算匹配
        // 匹配只看版本是否落在区间内，不额外要求提供者主版本等于 MinMajor
        const DependencyEntry bounded = Dependency(id, Version{1, 2, 0}, Version{2, 0, 0});
        Require(Dep::VersionMatches(bounded, 1, 2, 0), "有上限：下边界应匹配");
        Require(Dep::VersionMatches(bounded, 1, 9, 9), "有上限：区间内应匹配");
        Require(Dep::VersionMatches(bounded, 2, 0, 0), "有上限：上边界应匹配");
        Require(!Dep::VersionMatches(bounded, 1, 1, 9), "有上限：低于下边界不应匹配");
        Require(!Dep::VersionMatches(bounded, 2, 0, 1), "有上限：高于上边界不应匹配");
        // 上下限相同的单版本区间：只有该版本匹配
        const DependencyEntry exact = Dependency(id, Version{1, 3, 4}, Version{1, 3, 4});
        Require(Dep::VersionMatches(exact, 1, 3, 4), "单版本区间：该版本应匹配");
        Require(!Dep::VersionMatches(exact, 1, 3, 3), "单版本区间：低 patch 不应匹配");
        Require(!Dep::VersionMatches(exact, 1, 3, 5), "单版本区间：高 patch 不应匹配");
    }

    // —— 环拒绝 ——

    // 两节点环：A 依赖 B、B 依赖 A
    void CaseCycleTwoNodes() {
        std::vector<NodeSpec> specs;
        NodeSpec first;
        first.moduleId = Tag(0x11);
        first.dependencies.push_back(Upward(Tag(0x22), Version{1, 0, 0}));
        specs.push_back(first);
        NodeSpec second;
        second.moduleId = Tag(0x22);
        second.dependencies.push_back(Upward(Tag(0x11), Version{1, 0, 0}));
        specs.push_back(second);
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::Cycle, "两节点环");
    }

    // 三节点环：A → B → C → A
    void CaseCycleThreeNodes() {
        std::vector<NodeSpec> specs;
        NodeSpec first;
        first.moduleId = Tag(0x11);
        first.dependencies.push_back(Upward(Tag(0x22), Version{1, 0, 0}));
        specs.push_back(first);
        NodeSpec second;
        second.moduleId = Tag(0x22);
        second.dependencies.push_back(Upward(Tag(0x33), Version{1, 0, 0}));
        specs.push_back(second);
        NodeSpec third;
        third.moduleId = Tag(0x33);
        third.dependencies.push_back(Upward(Tag(0x11), Version{1, 0, 0}));
        specs.push_back(third);
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::Cycle, "三节点环");
    }

    // 环之外还有可先排入的独立模块：先把入度为零的排掉，剩下的环仍必须被拒绝
    void CaseCycleWithIndependentModule() {
        std::vector<NodeSpec> specs;
        NodeSpec independent;
        independent.moduleId = Tag(0x01);
        independent.preloadOrder = 1;
        specs.push_back(independent);
        NodeSpec first;
        first.moduleId = Tag(0x11);
        first.preloadOrder = 2;
        first.dependencies.push_back(Upward(Tag(0x22), Version{1, 0, 0}));
        specs.push_back(first);
        NodeSpec second;
        second.moduleId = Tag(0x22);
        second.preloadOrder = 3;
        second.dependencies.push_back(Upward(Tag(0x11), Version{1, 0, 0}));
        specs.push_back(second);
        TopologyResult result{};
        Reject reject = Reject::None;
        REQUIRE_REJECT(Sort(specs, result, reject), reject, Reject::Cycle, "独立模块之外的环");
    }

    // 依赖表逐条校验的独立入口：解析容器依赖表时逐表调用，判据与图构建共用
    void CaseCheckDependencyTable() {
        const ModuleId self = Tag(0x11);
        Reject reject = Reject::None;
        Require(Dep::CheckDependencyTable(self, nullptr, 0, reject) == nullptr && reject == Reject::None, "空依赖表应通过");
        // 正例：两条互不相同的依赖
        DependencyEntry good[2] = {Upward(Tag(0x22), Version{1, 0, 0}), Dependency(Tag(0x33), Version{1, 2, 0}, Version{1, 3, 9})};
        Require(Dep::CheckDependencyTable(self, good, 2, reject) == nullptr && reject == Reject::None, "合法依赖表应通过");
        // 自依赖：逐条校验入口同样拒绝
        DependencyEntry selfDependency[1] = {Upward(self, Version{1, 0, 0})};
        REQUIRE_REJECT(Dep::CheckDependencyTable(self, selfDependency, 1, reject), reject, Reject::InvalidDependencyList, "逐表校验拒绝自依赖");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckDependency: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 依次跑全部用例
    void Execute() {
        Case("正例：引导闭包按预装顺序装载", CaseBootClosureOrder);
        Case("正例：同输入两次一致且与数组顺序无关", CaseDeterministic);
        Case("正例：同层按预装顺序", CaseSameLayerPreloadOrder);
        Case("正例：预装顺序缺失按标识兜底", CaseOrderFallbackById);
        Case("正例：提供者先于消费者", CaseProviderBeforeConsumer);
        Case("拒绝：自依赖", CaseSelfDependency);
        Case("拒绝：依赖标识全零", CaseZeroDependencyId);
        Case("拒绝：最低主版本为零", CaseZeroMinMajor);
        Case("拒绝：区间倒置", CaseInvertedRange);
        Case("拒绝：依赖标志非零", CaseDependencyFlags);
        Case("拒绝：重复依赖", CaseDuplicateDependency);
        Case("拒绝：依赖条目超上限", CaseDependencyLimit);
        Case("拒绝：依赖表不可读", CaseDependencyUnreadable);
        Case("拒绝：模块数非法", CaseModuleCountInvalid);
        Case("拒绝：模块标识全零", CaseZeroModuleId);
        Case("拒绝：模块标识重复", CaseDuplicateModule);
        Case("拒绝：缺失提供者", CaseMissingProvider);
        Case("拒绝：版本低于下限", CaseVersionBelowMinimum);
        Case("拒绝：无上限依赖跨主版本", CaseVersionMajorMismatch);
        Case("版本区间匹配规则", CaseVersionMatchesRules);
        Case("拒绝：两节点环", CaseCycleTwoNodes);
        Case("拒绝：三节点环", CaseCycleThreeNodes);
        Case("拒绝：独立模块之外的环", CaseCycleWithIndependentModule);
        Case("依赖表逐条校验入口", CaseCheckDependencyTable);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckDependency: " << g_ran << "/" << kCaseCount << " cases passed" << std::endl;
    }
}

// 入口：跑全部用例
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string arg = argv[1];
            if (arg == "-h" || arg == "--help") {
                std::cout <<
                    "CheckDependency — 模块依赖表校验、版本区间匹配与稳定拓扑排序的宿主侧检查\n"
                    "\n"
                    "用法：CheckDependency\n"
                    "\n"
                    "检查对象是 Packages/LikesProgramOS/Core/Src/DependencyGraph.cpp（宿主侧编译同一份实现）\n"
                    "用例数固定，失败返回 1\n";
                return 0;
            }
            Fail("无法识别的参数：" + arg);
        }
        Execute();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckDependency: " << error.what() << '\n';
        return 1;
    }
}
