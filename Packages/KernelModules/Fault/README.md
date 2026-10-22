# Fault — 必需运行期模块 Init 失败夹具

`Fault` 是 G6d 的**失败注入载体**：`_Module_Init` 在核对 `KernelAbiContext` 之后固定返回 `KernelError::kCorrupt`，用来触发内核「必需运行期模块初始化失败」路径——`FATAL` + 屏幕 + 等待 + 复位；它不导出接口、不导入接口、没有模块依赖，也不申请任何资源。**本模块只进验证变体集，不随正式发布集交付。**

模块容器与入口语义见[内核模块容器](../../../Docs/Specs/内核模块容器.md)、[模块契约层](../../../Docs/Specs/模块契约层.md)；模块编译与打包规则见[KernelModules README](../README.md)与 `../Common/ModuleRules.mk`。

## 一、用途与边界

| 项 | 说明 |
| --- | --- |
| 注入路径 | 必需运行期模块 `Init` 失败：内核输出 `FATAL`（编号在 `E-KRN` `80–89` 段，由 M6 G6d 分配并登记）、屏幕可见、等待后复位，不做降级启动 |
| 依赖 | 无：0 导出 / 0 导入 / 0 依赖，不调用任何契约槽位，也不依赖其它模块先 `Init` |
| 装载时机 | 挂载系统卷之后，作为清单 `Type=2` 与 `Modules.list` 里的运行期条目；**不是**引导闭包成员，Baleen 不预装，由内核按列表装载 |
| 发行范围 | 只进验证变体集；正式发布集不含本模块（构建链用 `RUNTIME_MODULES` 变量单独控制） |
| 生命周期 | `Init` 返回失败后由装载器走 `Abort`（弱默认实现，空操作）与回收；本模块不自己打印、不复位 |

## 二、模块身份

`Module.mk` 是模块事实的单点；`ModuleId` 按契约层 3.2 派生：

```text
ModuleId = SHA-256("LPMOD01" ‖ "Fault" ASCII)[0..16)
         = 23338874eb5fb448a5f46b5269146595
```

| 项 | 取值 |
| --- | --- |
| `MODULE_NAME` / 产物主名 | `Fault` |
| `MODULE_LOGICAL_NAME` | `Fault`（不是引导闭包成员，不与 `BootClosure.def` 对应） |
| 版本 | `1.0.0`；major `1` 必须等于 `.os` 镜像数据的 `ModuleMajor`（硬门，容器 4.2） |
| `KernelAbiMin/Max` | `1` / `0xFFFF`（`ModuleRules.mk` 默认值，容器 4.3） |
| 容器文件名 | `Fault.1.0.0.osm`（`<逻辑名>.<major>.<minor>.<patch>.osm`） |
| 容器 `ModuleId` 字段 | `0x50` 起 16 字节，等于上面的派生值；装载器按容器内标识校验 |

生成头 `ModuleId.hpp` 只收录 `BootClosure.def` 的 13 个闭包成员，**没有 `kModFault` 常量**；`ModuleId` 由 `PackModule --name Fault` 按同一派生规则写进容器头（`PackModule` 与 `MakeContract` 的派生同源，都是 `SHA-256("LPMOD01"‖逻辑名)`），本 README 记录核对值。派生值从容器头回读与上式独立计算一致。

## 三、构建与产物

```sh
# 前提：契约生成头已存在（仓库根 make contract-generated）
make -C Packages/KernelModules/Fault check      # ELF64/ET_DYN、无未定义符号、重定位仅 RELATIVE、入口在 .text
make -C Packages/KernelModules/Fault package    # 打包并自动 --verify
```

产物路径：

| 用途 | 命令 | 产物 |
| --- | --- | --- |
| 全模块共用产物根（默认） | `make -C Packages/KernelModules/Fault package` | `Packages/KernelModules/Out/Modules/Fault.1.0.0.osm` |
| 单包独立落点（验证变体集取用） | `make -C Packages/KernelModules/Fault package OUT_DIR="$(pwd)/Packages/KernelModules/Fault/Out"` | `Packages/KernelModules/Fault/Out/Modules/Fault.1.0.0.osm` |
| 独立复核 | `Tools/Bin/PackModule --verify <路径>` | 同上，退出 0 即通过 |

两种情况打包流程相同：`MakeContract --module-check`（0 导出/0 导入声明通过）→ `PackModule` 写 `.osm` 并自动 `--verify`。两次打包字节一致：删掉 `.osm` 重跑 `package` 后 `cmp` 相同（见第六节）。

容器事实（`PackModule` 输出与独立复核一致）：

| 字段 | 取值 |
| --- | --- |
| 文件长度 / `TotalBytes` | `4272` 字节 |
| 容器格式 / 头长 | `Version=1`、`HeaderBytes=0x80` |
| 段表 | 1 段（`.text`）；`ImageBytes=0x2000` |
| 入口偏移 | `EntryInit=0x1000`、`EntryActivate=0x11e0`、`EntryAbort=0x11f0`（后两者来自 `Common/ModuleEntry.cpp` 弱默认实现） |
| 导出 / 导入 / 依赖 / 重定位 | `0 / 0 / 0 / 0` |
| sha256 | `c05fe7b5d9ba1eab51d013c411bd8ae1b1678e6ea8cf3ef5127d4b6224e5c49a` |

## 四、失败注入语义

`_Module_Init` 的顺序固定：

1. `context == nullptr` → `kInvalidArgument`；
2. `context->bytes < kKernelAbiContextMinBytes`（4）→ `kInvalidArgument`；
3. 其余情况 → `kCorrupt`。

**失败注入只发生在上下文合法时**：装载器的 detail 与编号按返回码区分，避免把「参数非法」和「模块主动失败」混成一类。`Init` 不登记资源，`Abort` 是弱默认的空操作；模块不输出 `FATAL` 行、不等待、不复位——失败可见性与重启由装载器负责（契约层 7.2）。

与 `Optional` 配合可形成对照证据：同一验证变体集里，`Fault` 触发必需模块失败（`FATAL` + 复位），`Optional` 触发可选模块失败（`WARN`、不重启、继续启动）。

## 五、清单夹具模块侧事实（与 G6d 构建链接口对齐）

运行期模块与引导闭包 `.osm` 共用清单 `Type=2`，**不新增清单类型、不动条目保留字节**；必需/可选由**集内相对路径前缀**承载，容器本身不带该信息（容器规格 1.1：容器不自声明装载层级）。

| 事实 | 取值 |
| --- | --- |
| 清单产物条目类型 | `Type=2`（`.osm`；与闭包模块同类型，清单格式与解析不变） |
| 清单相对路径 | `Modules/Fault.1.0.0.osm` |
| `Modules.list` 条目 | **收录**（列表是内核侧完整模块清单；本模块不属于 `BootClosure.def` 期望集，Baleen 不预装它，内核挂卷后按运行期模块装载） |
| 运行期识别 | 内核按「`Modules.list` 条目不属于引导闭包期望集」识别为运行期模块 |
| 必需/可选承载 | 路径前缀：`Modules/Optional/` 开头为可选，其余路径为必需；本模块落在 `Modules/` 根下，按必需处理 |
| 卷内落点 | `KernelSets/<BuildId>/Modules/Fault.1.0.0.osm`（与闭包模块同一目录树） |
| 版本一致性 | 清单/容器版本 `1.0.0`，major `1` 与 `.os` 的 `ModuleMajor` 硬相等 |
| 发行范围 | 只进验证变体集；正式发布集不登记本条目、不落本文件 |

运行期模块**必需/可选不是容器字段**：同一个 `.osm` 容器字节不区分这两类；构建链把本模块同时登记进变体集的清单 `Type=2` 与 `Modules.list`，内核按「列表条目减引导闭包期望集」识别，并按路径前缀判定必需/可选。

## 六、验证记录

```sh
make -C Packages/KernelModules/Fault check
make -C Packages/KernelModules/Fault package
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/Fault.1.0.0.osm
# 确定性：删 .osm 重跑 package，两次 cmp 相同
sha256sum Packages/KernelModules/Out/Modules/Fault.1.0.0.osm
```

结果：`check` 通过；`PackModule` 打包后自动 `--verify` 通过，独立 `--verify` 通过（1 段、0 导出/0 导入/0 依赖/0 重定位）；两次打包字节一致（`cmp` 无差异）；sha256 见第三节。

## 七、参考

- [模块契约层](../../../Docs/Specs/模块契约层.md)：`ModuleId` 派生（3.2）、`KernelError`（7.1）、入口 ABI（6.3）。
- [内核模块容器](../../../Docs/Specs/内核模块容器.md)：容器字节、入口偏移、模块版本与 `KernelAbiMin/Max`。
- [模块与驱动装载](../../../Docs/Specs/模块与驱动装载.md)：必需/可选模块失败语义与 `Init`/`Abort` 契约。
- [KernelModules README](../README.md)：模块包构建规则与 `PackModule` 接口。
