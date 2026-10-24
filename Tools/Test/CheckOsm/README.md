# CheckOsm

宿主侧 C++20 检查工具：内核模块容器 `.osm` 的正例、逐字段负例与确定性检查。编译后输出 `Tools/Bin/CheckOsm`。**手工执行，不构成默认构建依赖**；用例内嵌在源码里，容器由 `PackModule` 的写出器现场构造。

```sh
make -C Tools/Test/CheckOsm
Tools/Bin/CheckOsm
```

工具复用 `Tools/Build/PackModule/Src/Osm.cpp` 的写出器与解析校验器（`Makefile` 按相对路径引用，不复制实现），因此检查的就是 `PackModule --verify` 实际使用的同一份容器实现；容器规格见[内核模块容器](../../../Docs/Specs/内核模块容器.md)。

## 检查内容

**用例（78 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，任一失败返回 1；用例数固定，运行时与实跑数核对，防止用例被误删。

- **正例**：写出器构造的规范容器（`TEXT`/`RODATA`/`DATA`/`BSS` 各一页、一条导出、一条导入、一条依赖、两条重定位）通过解析与字段复算，包括 `ModuleId`/`InterfaceId` 派生、版本、ABI 范围、入口、槽表与回填槽、重定位按目标升序；空的可选表、整页文件字节、无人类可读模块名都按合法容器接受；同一构造输入两次打包字节一致；`RELATIVE32` 条目被接受；依赖图正例（A→B）通过
- **头部与表区负例**：魔数、未知容器版本、`HeaderBytes`、`Flags`、`TotalBytes` 与实际长度不符、`ImageBytes` 非 4 KiB 倍数、保留字段非零、段数 0、`SectionEntryBytes`/`ImportEntryBytes`/`DependencyEntryBytes` 不符、段表/导出表/重定位表偏移不对齐或越界、表区重叠、字符串表越界、字符串表末字节非 NUL、段名偏移越界、导出/重定位/依赖条目数超上限、`ModuleId` 全零、`ModuleVersionMajor` 为零、`KernelAbiMin` 为零、模块名偏移与字节数不一致、模块名越出字符串表
- **段负例**：`MemoryOffset` 非 4 KiB 对齐、`MemoryBytes` 非 4 KiB 倍数、段内存区间重叠、W+X 权限、`Type` 与 `Flags` 不符、未知段类型、`BSS` 带文件字节、非 BSS 无文件字节、`FileBytes` 超过 `MemoryBytes`、段文件偏移不对齐、段文件区间越界、BSS 文件偏移非零
- **入口负例**：`EntryInit` 为零、`EntryInit`/`EntryActivate` 不在 `TEXT` 段
- **导出/导入负例**：导出接口版本为零、槽数为零/超过 64、槽位不在 `TEXT`、同 `InterfaceId+Version` 重复、`InterfaceId` 全零、保留字段非零；导入 `MinVersion` 为零、版本区间颠倒、槽数为零/超过 64、回填槽不在可写段/未 8 字节对齐/越界、同 `InterfaceId` 重复、`InterfaceId` 全零
- **依赖负例**：自依赖、`MinMajor` 为零、版本区间颠倒、重复条目、`Flags` 非零、依赖图环（A→B→A）、依赖图缺失被依赖模块
- **重定位负例**：未知类型、目标在 `TEXT`、目标未按宽度对齐、目标越界、保留字节非零

**未覆盖**：`Base + Addend` 的 64 位/32 位溢出（装载器才知道 `Base`）、入口处是否真是函数、导入槽数与提供者槽数的跨模块一致性、装载期提供者唯一与完整依赖拓扑排序（`CheckRegistry`/`CheckDependency`/`CheckLoader` 的范围）；这些在 `PackModule README` 的“不在核对范围”里同样列出。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与原因），或用例数与固定值不符 |

## 破坏性自测

把任一用例的期望改坏（例如把 `kCaseCount` 或某个 `ExpectReject` 的关键字改掉）后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。这一自测用于证明断言不是恒真。
