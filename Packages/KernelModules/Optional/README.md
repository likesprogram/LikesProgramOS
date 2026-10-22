# Optional — 可选运行期模块失败/成功夹具

`Optional` 是 G6d 的**可选模块注入载体**：默认构建的 `_Module_Init` 在核对 `KernelAbiContext` 后返回 `KernelError::kCorrupt`，用来触发内核「可选运行期模块失败只 `WARN`、不重启」路径；另提供一键构建的**成功版**（`-DOPTIONAL_INIT_OK`，`Init` 返回 `kOk`）作为同集对照证据。它不导出接口、不导入接口、没有模块依赖，也不申请任何资源。**本模块只进验证变体集，不随正式发布集交付。**

模块容器与入口语义见[内核模块容器](../../../Docs/Specs/内核模块容器.md)、[模块契约层](../../../Docs/Specs/模块契约层.md)；模块编译与打包规则见[KernelModules README](../README.md)与 `../Common/ModuleRules.mk`。

## 一、用途与边界

| 项 | 说明 |
| --- | --- |
| 注入路径（失败版） | 可选运行期模块 `Init` 失败：内核只输出 `WARN`（带模块标识与返回码），**不重启、不中断启动**，继续后续流程 |
| 对照证据（成功版） | `Init` 成功：与失败版同集、同 `ModuleId`/版本，只是内容不同；用于证明「可选模块成功时正常 `Init`」 |
| 依赖 | 无：0 导出 / 0 导入 / 0 依赖，不调用任何契约槽位 |
| 装载时机 | 挂载系统卷之后，作为清单 `Type=2` 与 `Modules.list` 里的运行期条目；**不是**引导闭包成员，Baleen 不预装，由内核按列表装载 |
| 发行范围 | 只进验证变体集；正式发布集不含本模块（构建链用 `RUNTIME_MODULES` 变量单独控制） |

## 二、模块身份

`Module.mk` 是模块事实的单点；`ModuleId` 按契约层 3.2 派生：

```text
ModuleId = SHA-256("LPMOD01" ‖ "Optional" ASCII)[0..16)
         = 5f1fd14b78a5411c2ae826d70b67c939
```

| 项 | 取值 |
| --- | --- |
| `MODULE_NAME` / 产物主名 | `Optional` |
| `MODULE_LOGICAL_NAME` | `Optional`（不是引导闭包成员，不与 `BootClosure.def` 对应） |
| 版本 | `1.0.0`；major `1` 必须等于 `.os` 镜像数据的 `ModuleMajor`（硬门，容器 4.2） |
| `KernelAbiMin/Max` | `1` / `0xFFFF`（`ModuleRules.mk` 默认值，容器 4.3） |
| 容器文件名 | `Optional.1.0.0.osm`（`<逻辑名>.<major>.<minor>.<patch>.osm`） |
| 容器 `ModuleId` 字段 | `0x50` 起 16 字节，等于上面的派生值；装载器按容器内标识校验 |

生成头 `ModuleId.hpp` 只收录 `BootClosure.def` 的 13 个闭包成员，**没有 `kModOptional` 常量**；`ModuleId` 由 `PackModule --name Optional` 按同一派生规则写进容器头，本 README 记录核对值。失败版与成功版共用同一个 `ModuleId` 与版本，容器头一致。

## 三、构建与产物

失败版（默认，`Init` 返回 `kCorrupt`）：

```sh
make -C Packages/KernelModules/Optional check      # ELF 硬约束
make -C Packages/KernelModules/Optional package    # 打包并自动 --verify
```

成功版（`Init` 返回 `kOk`）：

```sh
make -C Packages/KernelModules/Optional -f Makefile.ok check
make -C Packages/KernelModules/Optional -f Makefile.ok package
```

`Makefile.ok` 在 `include ModuleRules.mk` **之后**给 `CXXFLAGS` 追加 `-DOPTIONAL_INIT_OK`：默认编译选项由 `ModuleRules.mk` 用 `?=` 给出，追加放在它之后才能既保留默认选项又带上宏；产物根目录默认 `Out/Ok/`，与失败版分开，两份 `.osm` 同名同版本、内容不同。

| 变体 | 命令 | 产物 | sha256 |
| --- | --- | --- | --- |
| 失败版（默认） | `make package` | `Packages/KernelModules/Out/Modules/Optional.1.0.0.osm` | `67cbabb463b2a27ea58b45154b2f7fbc6ad1f4bb5a797a9395af85ffa84b91cf` |
| 失败版（单包独立落点） | `make package OUT_DIR="$(pwd)/Packages/KernelModules/Optional/Out"` | `Packages/KernelModules/Optional/Out/Modules/Optional.1.0.0.osm` | 同上 |
| 成功版 | `make -f Makefile.ok package` | `Packages/KernelModules/Optional/Out/Ok/Modules/Optional.1.0.0.osm` | `734818f25d8eb08af2bbcf7a386368a1122bb14effdac8072b114b4c3fa00661` |

独立复核：`Tools/Bin/PackModule --verify <路径>`；两次打包字节一致：删掉 `.osm` 重跑后 `cmp` 相同（见第六节）。

容器事实（两版一致，只有代码路径不同）：

| 字段 | 取值 |
| --- | --- |
| 文件长度 / `TotalBytes` | `4272` 字节 |
| 容器格式 / 头长 | `Version=1`、`HeaderBytes=0x80` |
| 段表 | 1 段（`.text`）；`ImageBytes=0x2000` |
| 入口偏移 | `EntryInit=0x1000`、`EntryActivate=0x11e0`、`EntryAbort=0x11f0`（后两者来自 `Common/ModuleEntry.cpp` 弱默认实现） |
| 导出 / 导入 / 依赖 / 重定位 | `0 / 0 / 0 / 0` |

## 四、失败注入语义

`_Module_Init` 的顺序固定：

1. `context == nullptr` → `kInvalidArgument`；
2. `context->bytes < kKernelAbiContextMinBytes`（4）→ `kInvalidArgument`；
3. 失败版（默认）→ `kCorrupt`；成功版（定义 `OPTIONAL_INIT_OK`）→ `kOk`。

失败版用于「可选模块失败」：装载器只输出 `WARN`（模块标识、版本与返回码 `kCorrupt`），不重启、不阻塞后续必需模块与挂载流程；本模块不打印 `FATAL`、不等待、不复位（契约层 7.2）。成功版用于对照，证明同一路径在 `Init` 成功时按普通运行期模块处理。

## 五、清单夹具模块侧事实（与 G6d 构建链接口对齐）

运行期模块与引导闭包 `.osm` 共用清单 `Type=2`，**不新增清单类型、不动条目保留字节**；必需/可选由**集内相对路径前缀**承载，容器本身不带该信息（容器规格 1.1：容器不自声明装载层级）。

| 事实 | 取值 |
| --- | --- |
| 清单产物条目类型 | `Type=2`（`.osm`；与闭包模块同类型，清单格式与解析不变） |
| 清单相对路径（失败版与成功版相同） | `Modules/Optional/Optional.1.0.0.osm` |
| `Modules.list` 条目 | **收录**（列表是内核侧完整模块清单；本模块不属于 `BootClosure.def` 期望集，Baleen 不预装它，内核挂卷后按运行期模块装载） |
| 运行期识别 | 内核按「`Modules.list` 条目不属于引导闭包期望集」识别为运行期模块 |
| 必需/可选承载 | 路径前缀：`Modules/Optional/` 开头为可选，其余路径为必需；本模块固定落 `Modules/Optional/` |
| 卷内落点 | `KernelSets/<BuildId>/Modules/Optional/Optional.1.0.0.osm` |
| 版本一致性 | 清单/容器版本 `1.0.0`，major `1` 与 `.os` 的 `ModuleMajor` 硬相等 |
| 发行范围 | 只进验证变体集；正式发布集不登记本条目、不落本文件 |

运行期模块**必需/可选不是容器字段**：失败版与成功版的容器字节不区分这两类；构建链把本模块放进变体集时按 `Modules/Optional/` 前缀标记为可选，内核据此只 `WARN`。失败版与成功版不要同时进同一变体集——同一路径只能有一条清单条目。

## 六、验证记录

```sh
# 失败版
make -C Packages/KernelModules/Optional check
make -C Packages/KernelModules/Optional package
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/Optional.1.0.0.osm

# 成功版
make -C Packages/KernelModules/Optional -f Makefile.ok check
make -C Packages/KernelModules/Optional -f Makefile.ok package
Tools/Bin/PackModule --verify Packages/KernelModules/Optional/Out/Ok/Modules/Optional.1.0.0.osm

# 确定性：删 .osm 重跑，两次 cmp 相同
sha256sum Packages/KernelModules/Out/Modules/Optional.1.0.0.osm \
          Packages/KernelModules/Optional/Out/Ok/Modules/Optional.1.0.0.osm
```

结果：两种变体 `check` 通过；`PackModule` 打包后自动 `--verify` 通过（1 段、0 导出/0 导入/0 依赖/0 重定位）；`_Module_Init` 的返回路径经反汇编核对（失败版返回 `kCorrupt`、成功版返回 `kOk`）；sha256 见第三节与本节。

## 七、参考

- [模块契约层](../../../Docs/Specs/模块契约层.md)：`ModuleId` 派生（3.2）、`KernelError`（7.1）、入口 ABI（6.3）。
- [内核模块容器](../../../Docs/Specs/内核模块容器.md)：容器字节、入口偏移、模块版本与 `KernelAbiMin/Max`。
- [模块与驱动装载](../../../Docs/Specs/模块与驱动装载.md)：可选模块失败 `WARN` 与 `Init`/`Abort` 契约。
- [KernelModules README](../README.md)：模块包构建规则与 `PackModule` 接口。
