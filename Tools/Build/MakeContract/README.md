# MakeContract

模块契约层的宿主侧 C++20 生成器：读 `Packages/Common/Contract/Interfaces.def` 与 `BootClosure.def`，生成契约头与常量表到 `Packages/Common/Out/Contract/`；`--check` 只校验不写文件，`--freeze` 维护槽位冻结基线。

输入格式、标识派生、槽位冻结与构建期检查的语义单点是[模块契约层](../../../Docs/Specs/模块契约层.md)第二、三、四、九节；本文件只说明工具用法、逐字段解析细则、生成物与失败文本。两者不一致时修本文件。

编译后输出 `Tools/Bin/MakeContract`。

```sh
make -C Tools/Build/MakeContract

# 生成：默认在仓库根执行，写 Packages/Common/Out/Contract/
Tools/Bin/MakeContract

# 校验：全部输入检查通过才退出 0，不写任何文件
Tools/Bin/MakeContract --check

# 生成到其它目录、或读另一份输入（负例与回归用）
Tools/Bin/MakeContract --out /tmp/contract
Tools/Bin/MakeContract --check --contract-dir /tmp/contract-copy

# 首次冻结与冒烟冻结（T5.1 才写仓库里的 Frozen.tsv）
Tools/Bin/MakeContract --freeze --contract-dir /tmp/contract-copy --frozen /tmp/Frozen.tsv
```

## 命令行

| 参数 | 含义 |
| --- | --- |
| `--contract-dir <目录>` | 契约输入目录，默认 `Packages/Common/Contract`；读 `Interfaces.def`、`BootClosure.def` 与可选的 `Frozen.tsv` |
| `--out <目录>` | 生成目录，默认 `Packages/Common/Out/Contract`；目录不存在时创建 |
| `--check` | 只校验不写文件；通过时打印接口、槽、闭包与冻结记录统计 |
| `--freeze` | 把当前定义里尚未冻结的每个接口版本追加进冻结表；已冻结行只读比对 |
| `--frozen <文件>` | `--freeze` 的输出路径，默认 `<contract-dir>/Frozen.tsv`；只允许与 `--freeze` 一起用 |
| `--module-check` | 模块契约校验模式：检查 `Module.mk` 的导出/导入声明，输出 PackModule 参数（见下节） |
| `--logical-name <名>` | 模块逻辑名：闭包单例提供者按它与固定表核对；只与 `--module-check` 一起用 |
| `--export <稳定名>:<版本>` | 模块导出声明，可多次；槽数由接口定义推出，模块配置不写 |
| `--import <稳定名>:<Min>:<Max>:<使用槽数>` | 模块导入声明，可多次 |
| `-h` / `--help` | 打印用法，退出 0 |

退出码：`0` 成功；`1` 输入或校验失败；`2` 用法错误。失败信息写 stderr，格式为 `<文件名>:<行号>: <原因>`（文件级错误省略行号），文件名不带目录，同一输入在任意目录运行的报错文本一致。

## 输入

### `Interfaces.def`

一行一个槽位，字段之间单个空格：

```text
interface <稳定名> <版本> <槽名> <C 签名>
```

| 字段 | 解析细则 |
| --- | --- |
| `interface` | 固定关键字，小写、逐字节相等；行内第一个空格之后开始取字段 |
| `<稳定名>` | `[A-Z][A-Za-z0-9]*(\.[A-Z][A-Za-z0-9]*)+`，至少两个分量，不超过 64 字节；禁止空分量、连续 `.`、下划线、连字符、非 ASCII |
| `<版本>` | 十进制 `u16`，无前导零，取值 `1..65534`；`65535` 是导入上限哨兵，不允许作为接口版本 |
| `<槽名>` | `[A-Z][A-Za-z0-9]*`，同一版本块内唯一，不超过 32 字节 |
| `<C 签名>` | `<返回类型> (<参数列表>)`，占行内剩余全部字节；空参数必须写成 `(void)`，参数之间只允许单个 `", "` |

签名允许的类型：定宽整数（`uint8_t`…`uint64_t`、`int32_t`、`int64_t`）、`void`、`void*`/`const void*`、上述类型的指针、`KernelError`、`Storage::Error`、`char*`/`const char*`（路径字符串）、契约句柄 `DeviceHandle`/`ClaimHandle`/`BindHandle`/`FileSystemId`/`ObjectHandle`、`FileReference`、`ModuleId`、`Boot::Info`、`Storage::FileInfo`、`Storage::FileSystemCaps`，以及结构类型（`DeviceFacts`/`DeviceResources`/`BindingFacts`/`SystemVolumeFacts`/`DriverDesc`/`BlockDeviceOps`/`FileSystemOps`）——结构类型只能以指针出现，`char` 只能以指针出现，`const` 只能修饰指针指向的类型。

文件规则：

- 纯 ASCII、LF 行尾、末行可带 LF；不允许空行、CR、NUL、UTF-8 BOM、行首尾空白；字段之间单个空格
- 全文件按稳定名 ASCII 字节升序；同一稳定名的版本升序；同一版本块的槽行连续，行序即槽位顺序
- 以 `#` 开头的行整行跳过（兼容 `BootClosure.def` 的说明块；正式 `Interfaces.def` 按规格不放注释）
- 同一 `(稳定名, 版本)` 只允许出现一次；重复即"重复的接口版本块"，按重复提供者拒绝

### `BootClosure.def`

一行一个闭包成员：

```text
closure <顺序> <逻辑名>
```

- `<顺序>`：十进制、无前导零、`1..13`，且必须按行 `1, 2, …` 连续升序
- `<逻辑名>`：`[A-Z][A-Za-z0-9]*`，不超过 64 字节，唯一；派生 `ModuleId`
- 行数必须恰好 13；`#` 注释行与空行跳过（说明块占多行）
- 13 个逻辑名派生出的 `ModuleId` 两两不同

### `Frozen.tsv`

```text
<稳定名>\t<版本>\t<槽数>\t<槽位定义摘要>
```

- 摘要是该版本槽位定义（每槽一行 `<槽号>\t<槽名>\t<签名>`，LF 连接、末尾不带多余换行）的 SHA-256 前 16 字节，32 位小写十六进制
- 每行 4 个制表符分隔字段；`#` 注释行与空行跳过；同一 `(稳定名, 版本)` 只允许一行
- 文件缺席表示没有已发布版本，跳过冻结比对；空文件合法
- 冻结表中的每个已发布版本必须仍在当前 `Interfaces.def` 中存在，且槽数与摘要一致，否则校验失败
- 由 `--freeze` 追加：已冻结行只读比对，新版本块在尾部追加；输出按 `(稳定名, 版本)` 升序、固定注释头，同一输入重复执行字节一致
- **首次写入发生在 T5.1 的 v1 冻结**；M1–M4 开发期文件缺席，冻结检查自动跳过

## 生成物

全部写到生成目录，共 10 项，不散落到 `Packages/Common/Include/`；生成头不提交（`Out/` 被 `.gitignore` 忽略），输入 `.def` 与冻结表提交。

| 文件 | 内容 | 主要消费者 |
| --- | --- | --- |
| `InterfaceId.hpp` | `InterfaceId` 类型、判等/全零/文本辅助、每个稳定名的常量 | 注册表、PackModule、模块表、宿主检查 |
| `ModuleId.hpp` | `ModuleId` 类型、`kModuleKernelCore`（全零）、每个闭包逻辑名的常量 | 闭包期望集表、模块表、注册表 |
| `Types.hpp` | `KernelError` 与名字、`KernelErrorFromStorage`、句柄 typedef、ops 表与契约结构、`ModuleTables` | 全部模块、装载器、PackModule |
| `Entry.hpp` | `KernelAbiContext`、三入口 `extern "C"` 声明、入口符号名常量 | 模块骨架、装载器、PackModule |
| `Slots.hpp` | 每接口每版本的 `<接口>SlotsV<n>` 结构、`<接口>Slots` 别名、槽数/当前版本常量、导出与回填符号声明 | 提供者、消费者、装载器、注册表 |
| `Exports.hpp` | `kKernelExports`（全部 `Core.*` 版本）、`kClosureProviders`、提供者模块的导出表常量 | 内核注册表初始化、PackModule、CheckRegistry |
| `Imports.hpp` | 导入上限哨兵、`MakeImportEntry`、每接口的区间与槽数常量 | 模块表、PackModule、CheckRegistry |
| `BootClosure.hpp` | `kBootClosureCount`、`kBootClosure[]`（顺序 + `ModuleId`） | 内核装载器（E-KRN-55）、MakeManifest、CheckPreload |
| `Abi.hpp` | `kKernelAbiVersion`、`kDriveSdkVersion`、`Core.Abi` 槽 0/2 的固定实现与绑定 `static_assert` | 装载器、模块、注册表 |
| `Contract.hpp` | 聚合 include 全部生成头 | 所有需要契约的包 |

模块表 `<模块>Tables.cpp` 不在生成物里：它需要每模块的 `CONTRACT_EXPORTS` / `CONTRACT_IMPORTS` 声明，属 T1.7 模块骨架的输入，由模块构建生成；本工具提供 `ModuleTables` 结构、`kKernelExports` 与闭包提供者导出表作为它的原料。

### 命名约定

| 对象 | 规则 | 示例 |
| --- | --- | --- |
| 接口标识常量 | `kIf` + 稳定名去 `.` | `Core.Page` → `kIfCorePage` |
| 模块标识常量 | `kMod` + 逻辑名 | `DevModel` → `kModDevModel` |
| 内核核心模块标识 | 固定全零 | `kModuleKernelCore` |
| 槽结构 | 稳定名去 `.` + `SlotsV<版本>` | `CorePageSlotsV1` |
| 当前版本别名 | 稳定名去 `.` + `Slots` | `CorePageSlots` |
| 槽数/当前版本常量 | `k` + 稳定名去 `.` + `SlotCountV<版本>` / `CurrentVersion` | `kCorePageSlotCountV1`、`kCorePageCurrentVersion` |
| 导出/回填符号 | `_Contract_Export_` / `_Contract_Import_` + 稳定名**删点** + `_v<版本>`（与 PackModule 的 StripDots 一致） | `_Contract_Export_CorePage_v1`、`_Contract_Import_FsVfs_v1` |
| 版本常量 | 固定名 | `kKernelAbiVersion`、`kDriveSdkVersion` |

## `--check` 判定

任一失败即退出 1、不写文件；生成模式同样先完成全部检查再开始写，不产出半套生成物。

1. **字节级**：文件非空、纯 ASCII、LF 行尾、无 BOM/CR/NUL、非注释行无首尾空白、无空行（`Interfaces.def`）
2. **字段语法**：`interface`/`closure` 关键字、字段个数、稳定名/逻辑名/槽名语法与长度、版本取值与无前导零、签名格式与类型白名单
3. **排序与分组**：稳定名 ASCII 升序、同稳定名版本升序、版本块连续，槽位号等于块内行序
4. **唯一性**：同版本块内槽名唯一、槽数 `1..64`、`(稳定名, 版本)` 不重复、派生 `InterfaceId` 与 `ModuleId` 不碰撞、生成常量名不冲突
5. **提供者固定**：规格 4.3 的 10 个 `Core.*` 接口与 5 个闭包单例接口都有 v1；闭包单例提供者与 4.3 表一致（写入 `Exports.hpp`）
6. **ABI 绑定**：`Core.Abi` v1 存在，槽 0/1/2 的槽名与签名与规格 5.2/5.3 一致
7. **闭包表**：13 行、序号 `1..13` 连续升序、逻辑名唯一、`ModuleId` 不重复
8. **冻结比对**：`Frozen.tsv` 存在时，每个已发布版本仍在当前定义中且槽数与摘要一致；缺席时跳过

失败文本形如：

```text
MakeContract: Interfaces.def:29: 槽名在版本块内重复：PageBytes
MakeContract: Interfaces.def:17: 接口必须按稳定名 ASCII 升序排列：Core.Log 不能出现在 Core.Mem 之后
MakeContract: Interfaces.def:56: 重复的接口版本块（同一接口标识与版本）：Core.Page v1
MakeContract: BootClosure.def:26: 闭包顺序必须从 1 连续升序
MakeContract: BootClosure.def: 闭包成员必须恰好 13 个，实际 12 个
MakeContract: Frozen.tsv:14: 槽位定义与冻结记录不符：Core.Page v1
```

不在本工具检查范围的项（需要模块构建输入或产物，见规格 9.4）：模块声明的导出/导入区间可满足性、驱动/文件系统模块未导出单例、`<模块>Tables.cpp` 与容器表一致性、入口符号偏移；这些由 T1.3 `PackModule --verify`、T1.7 骨架与 T4.4 `CheckRegistry` 覆盖。

## 模块契约校验（`--module-check`）

`ModuleRules.mk` 在 `package` 前调用本模式：读 `Module.mk` 的 `CONTRACT_EXPORTS` / `CONTRACT_IMPORTS`，校验通过后把 PackModule 需要的参数打印到 stdout（一行一个）：

```sh
Tools/Bin/MakeContract --module-check --contract-dir <契约目录> --logical-name DevModel \
    --export Dev.Model:1 --import Core.Log:1:1:4
# 输出：
# --export Dev.Model:1:6
# --import Core.Log:1:1:4
```

校验项：

- 逻辑名语法（§3.3），闭包单例的提供者必须与 `kSingletonProviders` 的固定表一致
- 导出：接口与版本在 `Interfaces.def` 中存在、同模块内不重复、槽数由该版本槽位定义推出；`Core.*` 由内核核心唯一提供，模块导出直接拒绝
- 导入：区间 `1 <= Min <= Max <= 0xFFFF`、同一接口只有一条导入、接口存在且区间内有已定义版本、使用槽数不超过区间内最高版本的槽数
- 冻结比对与 `.def` 校验随 `Load` 一起执行（`Frozen.tsv` 存在时）

失败以 `MakeContract: Module.mk: <原因>` 写 stderr 并退出 1；参数格式错误退出 2。`ModuleRules.mk` 把工具输出作为 PackModule 的 `--export`/`--import` 参数，构建失败即配置失败，不产出半套 `.osm`。

## 确定性

- 输出顺序由 `Interfaces.def` 文件顺序与规格排序规则决定，不写时间戳、构建路径、用户名、环境信息
- 生成文本的 `static_assert` 消息与错误文本不含路径；同一输入两次生成（任意输出目录）逐字节一致
- 每次生成全部 10 个文件整份覆盖，不做增量差分，避免残留旧字节

## 当前输入规模

`Interfaces.def` 定义 **15 个接口 / 15 个版本块 / 55 个槽位**（`Core.*` 10 个接口 32 槽 + 闭包单例 5 个接口 23 槽），`BootClosure.def` 定义 13 个闭包成员。槽位分布：

| 接口 | 槽数 |
| --- | --- |
| `Core.Page` / `Core.PhysMap` / `Core.Mem` / `Core.Log` / `Core.Time` | 5 / 3 / 4 / 4 / 3 |
| `Core.Boot` / `Core.Dma` / `Core.Mmio` / `Core.Object` / `Core.Abi` | 2 / 2 / 2 / 4 / 3 |
| `Dev.Model` / `Drv.Loader` / `Block.Registry` / `Fs.Vfs` / `Boot.Volume` | 6 / 4 / 4 / 8 / 1 |

## 边界与遗留

- **`Frozen.tsv` 已在 T5.1 首次冻结**：`Packages/Common/Contract/Frozen.tsv` 含 15 个 v1 版本块，此后只增行；默认生成与 `--check` 都读它做冻结比对
- **运行期模块的导出/导入表**：需要 T1.7 模块骨架的 `CONTRACT_EXPORTS` / `CONTRACT_IMPORTS` 声明；`Imports.hpp` 当前只给按接口可推导的区间与槽数常量，接入模块声明后扩展生成每模块条目
- **模块表 `.cpp`**：由模块构建按 T1.7 模板生成，本工具提供结构与常量原料
- **设备事实结构布局**：`DeviceFacts` / `DeviceResources` / `BindingFacts` v1 的字段来自规格 10.2，总线地址编码、BAR/IRQ 表容量等细节待设备模型规格（T5.2）定稿时同步 `Types.hpp` 模板；本工具生成的是 v1 候选布局，`static_assert` 已钉住字节数
- **`kKernelAbiVersion` / `kDriveSdkVersion` 当前值** 硬编码在 `Src/Generate.cpp`（规格 5.1/5.2 的当前值都是 1），抬版时同步改工具与规格两处；`Core.Abi` 槽 0/2 的固定实现与符号名由 `Abi.hpp` 给出，内核核心导出表必须登记
- **`.def` 解析宽容度**：`Interfaces.def` 按规格不许空行，`BootClosure.def` 允许空行与 `#` 说明块；`#` 注释行在两类文件中都跳过，正式 `Interfaces.def` 不放注释
- **格式细则的语义单点**：本文件与规格不一致时修本文件；`Interfaces.def` 的排序、唯一性与签名白名单以规格第二、三节为准

## 构建依赖

`Makefile` 照 `Tools/Build/MakeManifest` 的结构：`OBJ_DIR` / `BIN_DIR` 可覆盖（`BIN_DIR ?= ../../Bin`），`-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror` 加链接 `-Wl,--fatal-warnings`；`Sha256.cpp` / `HostIo.cpp` 从 `Tools/Common/Src` 经 `vpath` 编入。标识派生只有本工具一处实现，与 `Tools/Common` 的 SHA-256 同源。
