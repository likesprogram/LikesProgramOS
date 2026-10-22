# Hello — 模块骨架样板

`Hello` 是 `Packages/KernelModules` 的骨架样板模块（不在引导闭包内，只用于演示与复制）：它核对内核传入的 `KernelAbiContext`、记录上下文事实，并演示一条导入（`Core.Log:1:1:4`，经导入槽写一行日志）；不申请资源、不发布导出、不启用中断与线程。13 个闭包模块按本包复制，避免样板漂移。

## 一、构建与产物

```sh
make -C Packages/KernelModules/Hello          # 构建 Out/Hello/Hello.elf
make -C Packages/KernelModules/Hello check    # 复核 ELF 硬约束
make -C Packages/KernelModules                 # 经顶层模块 Makefile 构建
```

产物是 `Packages/KernelModules/Out/Hello/Hello.elf`（`ET_DYN`、ELF64），由 `PackModule`（T1.3）打成 `.osm`；本包不产出 `.osm`。

## 二、模块身份：`MODULE_NAME`、逻辑名与版本

`Module.mk` 是模块事实的单点：

| 变量 | 含义 | 派生 / 约束 |
| --- | --- | --- |
| `MODULE_NAME` | 模块包名与产物主名 | 产物 `Out/<MODULE_NAME>/<MODULE_NAME>.elf`；`Hello` 与逻辑名相同 |
| `MODULE_LOGICAL_NAME` | 参与 `ModuleId` 派生的逻辑名 | `ModuleId` = `SHA-256("LPMOD01" ‖ 逻辑名 ASCII)[0..16)`（容器 4.1、契约层 3.2）；闭包模块必须与 `Packages/Common/Contract/BootClosure.def` 的行逐字一致，改名等于换一个模块 |
| `MODULE_MAJOR` | 模块版本 major | 非零，且必须等于 `.os` 镜像数据的 `ModuleMajor`（硬门，容器 4.2） |
| `MODULE_MINOR` / `MODULE_PATCH` | 模块版本 minor / patch | 向下兼容新增用 minor，修正用 patch |
| `MODULE_SRCS` | 源码清单 | 显式登记，新增 `.cpp` 必须加入 |
| `MODULE_ENTRY_DEFAULTS` | 非空时链接弱符号默认入口 | 见第四节 |
| `MODULE_REQUIRED_SECTIONS` | `check` 必须存在的段 | 样板要求四段齐全 |

`MODULE_LOGICAL_NAME` 与版本经 `-D` 传给源码（`MODULE_LOGICAL_NAME`、`MODULE_MAJOR`、`MODULE_MINOR`、`MODULE_PATCH`），`Src/Module.cpp` 不再写死；`ModuleId`、`KernelAbiMin/Max` 由 T1.4 按生成的契约头与 `Module.mk` 填进 `.osm`，当前 ELF 不承载它们。

## 三、入口约定

入口符号名固定、C 调用约定、默认可见性（`-fvisibility=hidden` 下由 `Common/ModuleAbi.hpp` 的声明显式声明 `default`）；`PackModule` 从 ELF 符号表按名字取偏移，不解析 C++ 修饰名，不 strip。

| 入口 | 何时调用 | 本骨架的实现 |
| --- | --- | --- |
| `_Module_Init` | 映射、重定位、导入解析与依赖检查全部通过后（容器第十二节第 10 步） | **必须存在**；空指针或 `bytes` 小于 `kKernelAbiContextMinBytes` 返回 `kInvalidArgument`，否则记录 `bytes`/`version` 并返回 `kOk` |
| `_Module_Activate` | 阶段 3 起、激活条件满足后 | 本骨架不提供，使用 `ModuleEntry.cpp` 的弱默认实现（返回 `kNotSupported`）；P5 阶段 0–2 不调用 |
| `_Module_Abort` | `Init` 返回非 `kOk` 且入口存在时 | 本骨架提供强符号，返回 `kOk`（`Init` 没登记资源，撤销是空操作） |

`KernelAbiContext` 是内核→模块的唯一入口参数（契约层 6.2）：32 字节，首字段 `bytes`，`version`、`kernelAbiVersion`、`moduleBase`、`moduleId` 依次排列；模块按 `bytes` 有界读取，只读，不得改写。布局由生成头 `Entry.hpp`（经 `Common/ModuleAbi.hpp` 转发）的 `static_assert` 锁住总长与各字段偏移，生成头变化会直接编译失败。

返回值是 `KernelError`（契约层 7.1）：`u32` 底层、`0` 为 `kOk`；模块**不**自己输出 `FATAL`、不等待、不复位，失败可见性由装载器决定。

## 四、骨架内容与段

`Src/Module.cpp` 有意让四个段都有内容，复制后可直接复核容器段表：

| 段 | 内容 | 验证点 |
| --- | --- | --- |
| `.text` | 三个入口与 `Common/Libc.cpp` | 入口在 `.text`、偏移非零 |
| `.rodata` | 逻辑名字符串 `kLogicalName` | 只读段 |
| `.data` | 自描述表 `g_selfDescription`，持有指向 `.rodata` 的指针 | 构建侧产出 `R_X86_64_RELATIVE`，装载器重定位路径可验证 |
| `.bss` | `g_contextBytes` / `g_contextVersion` | 可写段与清零约定 |

弱符号默认入口由 `Common/ModuleEntry.cpp` 提供：`MODULE_ENTRY_DEFAULTS := 1` 时才链接，模块定义同名强符号即覆盖。`_Module_Init` 没有默认实现——容器要求入口真实存在，默认空实现会让“忘了写 Init”在链接期静默通过。

## 五、导出 / 导入声明与模块表

模块的接口事实在构建配置里声明（契约层 2.4，`MakeContract --module-check` 在 `package` 时校验）：

```makefile
# 导出项 <稳定名>:<版本>；同一 (InterfaceId, Version) 只能有一个提供者
CONTRACT_EXPORTS := Dev.Model:1
# 导入项 <稳定名>:<MinVersion>:<MaxVersion>:<使用槽数>
CONTRACT_IMPORTS := Core.Log:1:1:4
```

- 稳定名必须先在 `Packages/Common/Contract/Interfaces.def` 里定义；生成器据此产出 `InterfaceId`、槽位结构与 `KernelAbiVersion`。
- 模块表符号由源码里的宏定义，命名与生成头一致（`_Contract_Export_<稳定名去点>_v<版本>` / `_Contract_Import_<稳定名去点>_v<版本>`）：`LP_CONTRACT_EXPORT(DevModel, 1, DevModelSlotsV1) = { ... };`、`LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};`（第一个参数是稳定名去点写法，宏带 `visibility("default")` 与 `used`）。
- 导出槽表是**非 const** 的可写段对象（`PackModule` 要求槽表落 Data/Bss 才能逐槽读 `R_X86_64_RELATIVE` 重定位）；初始化器只放模块内函数地址，不能放导入回填槽。
- 导入回填区零初始化落 `.bss`，装载器在 `Init` 前按导入表回填 8 字节槽；模块在 `Init` 里判空后再按槽调用。
- `Hello` 定义 `CONTRACT_IMPORTS := Core.Log:1:1:4` 并声明一张导入回填区，演示上述用法；`CONTRACT_EXPORTS` 留空（导出样例留给真实闭包模块）。
- 跨模块调用只能经导入表回填的函数指针，不得直接引用内核或其他模块的符号。

## 六、复制成新模块

1. `cp -r Packages/KernelModules/Hello Packages/KernelModules/<新名>`。
2. 改 `Module.mk` 的 `MODULE_NAME`、`MODULE_LOGICAL_NAME`（闭包模块取 `BootClosure.def` 的行）、版本与 `MODULE_SRCS`。
3. 改 `Src/Module.cpp` 的 `_Module_Init`，删掉自描述表与工作区中不需要的部分；需要导出/导入时按第五节填两个 `CONTRACT_` 变量。
4. `make -C Packages/KernelModules/<新名> check`，再跑顶层 `make -C Packages/KernelModules`。

变量语义与可直接复制的变量块见 `../Common/ModuleTemplate.mk`；目录职责与 `PackModule` 接口见 `../README.md`。
