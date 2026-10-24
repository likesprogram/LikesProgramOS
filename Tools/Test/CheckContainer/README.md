# CheckContainer

宿主侧 C++20 检查工具：内核侧模块容器有界解析（`Packages/LikesProgramOS/Core/Src/ModuleContainer.cpp`）的宿主侧检查。编译后输出 `Tools/Bin/CheckContainer`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckContainer
Tools/Bin/CheckContainer
Tools/Bin/CheckContainer --modules Packages/KernelModules/Out/Modules
```

容器字节表见[内核模块容器](../../../Docs/Specs/内核模块容器.md)；本工具用 `vpath` 把内核目标侧源码按相对路径直接编进本工具（不复制实现），解析器本身不依赖 `Storage` / `Print` / 宿主工具，也不链接 `Libc.cpp`（内部用逐字节比较，不引 `memcmp` / `memcpy`）。

## 正例来源

- **真实模块产物**：`Packages/KernelModules/Out/Modules/` 下全部 `.osm`（M5 真实模块，尺寸/表项不再统一）逐个解析，核对 `ModuleId` 非零、`ImageBytes` 非零且 4 KiB 对齐、各段类型/内存区间落在 `ImageBytes` 内、模块版本与 ABI、模块名、入口落在 TEXT、重定位条目可读且类型合法；不断言具体容器尺寸
- **合成夹具**：真实模块的表项集合随模块职责不同（导入/导出/依赖数量各异），为覆盖全部表项负例，工具内嵌一个规范的合成容器（四段各一页、一条导出、一条导入、一条依赖、两条重定位、两张槽位表）；它同时作为这些表负例的改坏基底。合成夹具只存在于测试内，不是第二份容器实现

## 检查内容

**用例（89 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，失败返回 1；用例数固定，运行时与实跑数核对，防止用例被误删。负例按 `Kernel::ContainerError` 分类断言（不按文本子串）。

- **正例（5）**：全部真实容器通过；合成容器通过并核对视图字段；重定位按索引读取（越界索引被拒）；导出 / 导入槽位访问器（越界索引被拒）；可选表全空的容器通过
- **文件头与表区（28）**：空指针、短于 0x80、魔数、未知版本、`HeaderBytes`、`Flags`、`TotalBytes` 不符或超 64 MiB、`ImageBytes` 非页倍数、保留字段非零、段数 0/超 16、`SectionEntryBytes`、导出数超 128、`ImportEntryBytes`、`DependencyEntryBytes`、重定位数超 65536、段表/导出表/导入表/依赖表/重定位表偏移不对齐或越界、字符串表越界/区间越界/末字节非 NUL
- **模块身份与模块名（6）**：`ModuleId` 全零、`ModuleVersionMajor` 为零、`KernelAbiMin` 为零、模块名偏移与字节数不一致、模块名越出字符串表、段名偏移越出字符串表
- **段表（13）**：未知类型、W+X、类型与权限不符、对齐非法、`MemoryOffset` 未按 4 KiB 对齐、`MemoryBytes` 非 4 KiB 倍数、内存区间越过 `ImageBytes`、内存区间重叠、BSS 带文件字节、非 BSS 无文件字节、`FileBytes` 超过 `MemoryBytes`、文件偏移未 4 字节对齐、文件区间越界
- **入口（3）**：`EntryInit` 为零、`EntryInit` 不在 TEXT、`EntryActivate` 不在 TEXT
- **表区重叠（2）**：依赖表与重定位表重叠、导出槽位表与导入表重叠
- **导出表（9）**：版本为零、标识全零、保留字段非零、槽数为零 / 超 64、槽位表偏移不对齐 / 越界、槽位不在 TEXT、同标识同版本重复
- **导入表（12）**：标识全零、`MinVersion` 为零、版本区间颠倒、保留字段非零、槽数为零 / 超 64、回填表偏移不对齐 / 越界、回填槽不在可写段 / 未 8 字节对齐 / 越界、同标识重复
- **依赖表（6）**：标识全零、自依赖、`MinMajor` 为零、`Flags` 非零、版本区间颠倒、同标识重复
- **重定位表（5）**：未知类型、保留字节非零、目标在 TEXT、目标未按宽度对齐、目标越出可写段

**未覆盖**：容器内容摘要（`E-KRN-58` 的装载器路径）、段文件内容 / 重定位应用 / 导入解析 / 依赖拓扑（T4.2–T4.5 的后续工具）、映射与页权限（M6）。`Base + Addend` 溢出需要装载基址，属 T4.2。

## 与内核集成

- 解析器源文件要加入 `Packages/LikesProgramOS/Core/Makefile` 的 `LOCAL_OBJS`：`ModuleContainer.o`（本任务未改 `Core/Makefile`，登记由 T4.1 的构建接线负责）
- 不新增链接依赖：`ModuleContainer.cpp` 只 include `ModuleContainer.hpp`（`<stdint.h>` + 生成的 `ModuleId.hpp` / `InterfaceId.hpp`），不引 `Libc`、`Storage`、`Print`；`Core` 已有的 `-I$(COMMON_DIR)/Out/Contract` 覆盖生成头
- 内核栈：解析函数局部只有约 4.5 KiB 的区间数组；`ContainerView` 由调用方静态持有（约 10 KiB），`ParseModuleContainer` 不分配内存

## 选项与退出码

| 选项 | 说明 |
| --- | --- |
| `--modules <目录>` | 真实 `.osm` 产物目录，默认 `Packages/KernelModules/Out/Modules`（从仓库根执行） |
| `-h`, `--help` | 显示用法 |

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与期望 / 实际分类），或用例数与固定值不符 |
| 2 | 真实模块产物缺失或不可用（先跑 `make modules`），或命令行错误 |

## 破坏性自测

把任一负例的期望分类改坏（例如把 `SectionMemoryOverlap` 改成 `SectionMemoryRange`）后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。这一自测用于证明分类断言不是恒真。
