# DriverLoader — M5 驱动加载器最小实现

`DriverLoader` 是引导闭包里的驱动加载器唯一实例，提供 `Drv.Loader` v1 四槽。当前落地范围是内置 `DriverDesc` 的注册、通配匹配、绑定、启动与停止，以及把绑定事实同步给 `DevModel`；外部 `.drv` 装载与热替换属阶段 3 之后。

| 项 | 值 |
| --- | --- |
| `MODULE_NAME` / `MODULE_LOGICAL_NAME` | `DriverLoader`（与 `Packages/Common/Contract/BootClosure.def` 的 `closure` 行逐字一致） |
| `ModuleId` | `SHA-256("LPMOD01" ‖ "DriverLoader")[0..16)` = `071f6c9ba783ded5f58d3307f54a3685` |
| 模块版本 | `1.0.0` |
| 导出 | `Drv.Loader:1`（四槽，符号 `_Contract_Export_DrvLoader_v1`） |
| 导入 | `Core.Log:1:1:4`、`Core.Mem:1:1:4`、`Dev.Model:1:1:6` |
| 依赖 | `DevModel:1:0`（`Dev.Model` 的唯一提供者必须先装载） |
| ELF 产物 | `Out/DriverLoader/DriverLoader.elf`（`PackModule` 的输入，不 strip） |
| 容器产物 | `Out/Modules/DriverLoader.1.0.0.osm` |

## 源码布局

| 文件 | 职责 |
| --- | --- |
| `Src/DriverRegistry.hpp` / `Src/DriverRegistry.cpp` | 驱动描述表、绑定记录与四槽实现；`Dev.Model` 槽表与日志回调经 `Init` 注入 |
| `Src/Module.cpp` | 入口与契约表接线：导入回填区、导出槽表、`_Module_Init` / `_Module_Abort` |

## 语义要点

- **匹配规则**：`busKind` 0、`classCode` 0xFFFFFFFF、`vendorId` 0xFFFF、`deviceId` 0xFFFFFFFF 为通配，其余字段必须与 `DeviceFacts` 相等；匹配用的驱动描述必须已经 `RegisterDriver` 登记过
- **登记校验**：`bytes` 不小于 64、`driveSdkVersion == kDriveSdkVersion`（不等返回 `kVersionMismatch`）、`opsKind == 1`（块设备）、`flags == 0`、`start` / `stop` / `ops` 非空、字段宽度合法；同一描述重复登记返回 `kAlreadyExists`，表满返回 `kOutOfResources`
- **绑定顺序**：`Bind` 先经 `Dev.Model.Claim` 认领唯一控制权，再写绑定记录并同步 `state = Bound`（`ops` / `context` 为空）；一格设备同时只允许一条绑定
- **启动与停止**：`Start` 调用驱动的 `start` 回调，成功后同步 `state = Started` 与 `ops` / `context`；`start` 失败按 `KernelErrorFromStorage` 映射返回并回滚绑定与认领；`Stop` 在已启动时调用 `stop`，随后清除绑定事实、归还认领并释放记录（绑定但未启动时只回收，不调用 `stop`）
- **模块标识**：`DriverDesc` v1 不带模块标识，`BindingFacts.driverModule` 当前写零；等契约追加字段后再填真实驱动模块
- **`.drv`**：`LoadExternal` 返回 `kNotSupported` 并写固定 WARN 行；DriverLoader 当前没有对外槽位承载它，宿主检查直接调用该函数

## 已知限制（M5 裁决接受）

- `DriverDesc` v1 不带驱动模块标识，`BindingFacts.driverModule` 当前写零；契约 v1 已冻结，尾部追加字段属 M6/T7 决定
- `.drv` 没有内核侧触发路径：`LoadExternal` 只在宿主检查里被直接调用，返回 `kNotSupported` 并写 WARN，真实装载属阶段 3
- 绑定登记经 `Dev.Model.Binding` 的三种模式承载（查询 / 登记更新 / 清除），契约 v1 不新增登记槽，见 `../DevModel/README.md`

## 可观测标记

| 场景 | 串口行 |
| --- | --- |
| Init 成功 | `[ INFO ] drvloader: driver loader ready` |
| `.drv` 装载尝试 | `[ WARN ] drvloader: external .drv loading is not supported` |

## 构建与核对

```sh
make -C Packages/KernelModules/DriverLoader            # 只建 ELF
make -C Packages/KernelModules/DriverLoader check      # ELF 形态 / 重定位 / 入口约束
make -C Packages/KernelModules/DriverLoader package    # 打成 Out/Modules/DriverLoader.1.0.0.osm
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/DriverLoader.1.0.0.osm
```

契约声明在 `Module.mk`：`CONTRACT_EXPORTS := Drv.Loader:1`、`CONTRACT_IMPORTS := Core.Log:1:1:4 Core.Mem:1:1:4 Dev.Model:1:1:6`、`MODULE_DEPENDS := DevModel:1:0`。`make package` 依赖 T5.1 的 MakeContract 模块校验把槽数翻译成 PackModule 参数；该接线未就绪前可用等价的显式命令行打包（`--export Drv.Loader:1:4` 等）。

宿主检查见 `Tools/Test/CheckDevModel`：用指向 `DeviceModel` 免费函数的 `Dev.Model` 槽表驱动注册、匹配、启停与回滚用例。
