# CheckDependency

宿主侧 C++20 检查工具：核对内核模块**依赖表校验、依赖版本区间匹配与稳定拓扑排序**。编译后输出 `Tools/Bin/CheckDependency`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckDependency
Tools/Bin/CheckDependency
```

依赖本工具前先有契约生成物 `Packages/Common/Out/Contract/ModuleId.hpp`（顶层 `make contract-generated` 产出；本工具 Makefile 按 `CONTRACT_DIR` 引用生成物）。

## 检查对象与复用方式

工具按宿主选项编译**目标侧的同一份源码**，不复制实现：

| 源文件 | 作用 |
| --- | --- |
| `Packages/LikesProgramOS/Core/Src/DependencyGraph.cpp` | 依赖表逐条校验、依赖解析（提供者 / 版本区间）、稳定拓扑排序 |
| `Packages/LikesProgramOS/Core/Include/DependencyGraph.hpp` | 输入视图、拒绝分类与结果结构 |

`Makefile` 用 `vpath` + 相对路径（`../../../Packages/LikesProgramOS/Core/...`）直接引用源码；宿主 C 运行库满足基础符号需求，内核侧 `Libc.cpp` 不链接。

规格依据：`Docs/Specs/内核模块容器.md` 第九节（依赖表 32 字节条目与匹配规则）、第十二节第 9 步（依赖检查）；同层顺序见 `Docs/Specs/模块契约层.md` 3.1 与 `Docs/Specs/模块与驱动装载.md` 第二节。

## 检查内容（24 项用例）

**正例：稳定排序**

- 引导闭包 13 模块按计划 6.3 的依赖图构造：预装顺序本身已满足依赖，输出必须与 `Modules.list` 预装顺序逐项一致，且“提供者先于消费者”
- 同一输入连续两次结果逐项一致；反转输入数组的存放顺序后，按标识序列比较结果不变（排序键是预装顺序与模块标识，不是数组下标）
- 同层（都无依赖）按预装顺序排；预装顺序缺失（0）排到最后并以 `ModuleId` 字节序兜底
- 链式依赖：即使预装顺序号是反的，提供者仍先于消费者

**拒绝：依赖表结构（`InvalidDependencyList`）**

- 自依赖、依赖条目 `ModuleId` 全零、`MinMajor` 为零、区间倒置（`Max < Min`）、`Flags` 非零、同一被依赖模块重复登记
- 依赖条目超上限（> 32）、依赖表不可读（条目数非零但指针为空）
- `CheckDependencyTable` 逐表校验入口：空表与合法表通过、自依赖被拒

**拒绝：图结构（`InvalidModuleList`）**

- 模块数为 0 或超过上限、模块标识全零、同一 `ModuleId` 在图中出现两次

**拒绝：依赖解析（`MissingProvider` / `VersionMismatch`）**

- 依赖的模块不在本次启动图里（无提供者）
- 提供者版本低于下限；无上限依赖跨主版本

**版本区间匹配规则（直接核对 `VersionMatches`）**

- 无上限：主版本相等且 `(Minor, Patch) >= (MinMinor, MinPatch)`，下边界、高 minor、高 patch、低 patch、低 minor、低主版本、高主版本逐项
- 有上限：闭区间，下边界 / 区间内 / 上边界匹配，低于下界与高于上界不匹配
- 上下限相同的单版本区间：只有该版本匹配

**拒绝：环（`Cycle`）**

- 两节点环、三节点环；环之外还有独立模块（先排掉入度为零的模块）仍必须判为环

## 给 T4.6 的接口约定

```cpp
// 输入：本次启动参与依赖判定的全部模块
struct ModuleNode {
    ModuleId moduleId;                 // 容器头 0x50 的 ModuleId
    uint16_t major, minor, patch;      // 容器头 0x66 / 0x68 / 0x6A
    const DependencyEntry* dependencies;   // 容器依赖表条目（第九节 32 字节）
    uint32_t dependencyCount;          // 0..32
    uint32_t preloadOrder;             // 列表中的闭包成员顺序；0 = 未提供
};
const char* SortByDependency(const GraphInput&, TopologyResult&, Reject&);

// 输出：index 是输入 modules 数组的下标，按装载顺序（提供者在前）
struct TopologyResult { uint32_t count; uint32_t index[kMaxModules]; };
```

- **什么时候调用**：依赖只用到容器元数据（标识、版本、依赖表），不需要映射与重定位。建议 T4.6 在步骤 4（标识与版本）之后、步骤 6（映射）之前构建一次图并排序；步骤 9 的“依赖检查”只需按排序顺序确认被依赖模块已完成 `Init` 并发布接口。
- **编号映射**：`Reject::InvalidModuleList` / `Reject::InvalidDependencyList` → `E-KRN-57`（容器/集合结构非法）；`Reject::MissingProvider` / `Reject::VersionMismatch` / `Reject::Cycle` → `E-KRN-59`（detail 用返回的原因文本）。
- **排序规则**：Kahn 算法，每步在“依赖已全部排入”的模块里取 `(preloadOrder, ModuleId 字节序)` 最小者；预装顺序为 0 的排到最后、以标识字节序兜底。输入顺序本身满足依赖时输出与 `Modules.list` 预装顺序一致。
- **容器解析阶段**可在 T4.1 里逐表调用 `CheckDependencyTable(selfModuleId, entries, count, reject)`，与整图构建共用同一份判据。
- **与 T4.1 的对接**：`ContainerView::dependencies[i]`（`ContainerDependency`）与本工具的 `DependencyEntry` 字段语义相同，映射时逐字段赋值并令 `flags = 0`（容器视图没有 Flags——解析器已校验并丢弃）；`ModuleNode` 的标识与版本取 `ContainerView` 的 `moduleId / versionMajor / versionMinor / versionPatch`，`preloadOrder` 取 `Modules.list` 条目序号。本工具不包含容器头，两个解析器可分别编译与测试。
- **容量**：模块数 ≤ 64、每模块依赖条目 ≤ 32；超限按结构拒绝返回，不分配内存。

## 已知规格张力（待 M0 复核）

- **有上限依赖的跨主版本区间**：容器第九节的匹配算法写的是“有上限时按 (Major, Minor, Patch) 字典序落在 [Min, Max] 闭区间内”，据此 `MaxMajor > MinMajor` 的区间会匹配上限内的高主版本；而 `模块与驱动装载` 14.3 第 4 条写“依赖的 MinMajor 是兼容主版本，提供者 major 相同才匹配”。本实现按容器第九节的字节表规则（正式匹配算法）执行，并在用例里显式覆盖跨主版本上限；两种口径需要 M0 收敛，收敛后只需改 `VersionMatches` 一处与对应用例。

## 登记

工具目录与可执行文件已就绪；顶层 `Makefile` 的 `TEST_TOOL_DIRS` 与 `Tools/Test/CheckAll.sh` 的 `host_tools` 静态清单**由协调者统一登记**（本任务不改那两处）。内核侧需要把 `DependencyGraph.o` 加进 `Packages/LikesProgramOS/Core/Makefile` 的 `LOCAL_OBJS`（本任务不改该文件）。

退出码：`0` 全部通过、`1` 有用例失败、`2` 参数错误。
