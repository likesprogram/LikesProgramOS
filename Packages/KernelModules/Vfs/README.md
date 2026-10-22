# Vfs — M5 存储栈核心（Block.Registry / Fs.Vfs / Boot.Volume）

`Vfs` 是引导闭包的存储栈核心模块：它是 `Block.Registry`、`Fs.Vfs`、`Boot.Volume` 三个闭包单例的**唯一提供者**（契约层 4.3 固定提供者表），驱动与文件系统模块只导入这些接口并注册 ops，不导出同名接口。

| 项 | 值 |
| --- | --- |
| `MODULE_NAME` / `MODULE_LOGICAL_NAME` | `Vfs`（与 `Packages/Common/Contract/BootClosure.def` 的 `closure` 行逐字一致） |
| `ModuleId` | `SHA-256("LPMOD01" ‖ "Vfs")[0..16)` |
| 模块版本 | `1.0.0`（主版本跟随 `.os` 发布线；M2 骨架已退役） |
| 导出 | `Block.Registry:1`（4 槽）、`Fs.Vfs:1`（8 槽）、`Boot.Volume:1`（1 槽） |
| 导入 | `Core.Boot:1:1:2`、`Core.Log:1:1:4`、`Core.Mem:1:1:4`、`Core.Page:1:1:5`、`Dev.Model:1:1:6` |
| 依赖表 | `DevModel:1:0`：Init 时必须能从 Dev.Model 取到驱动绑定事实（预装序 DevModel 在 Vfs 之前） |
| ELF 产物 | `Out/Vfs/Vfs.elf`（`PackModule` 的输入，不 strip） |
| 容器产物 | `Out/Modules/Vfs.1.0.0.osm` |

## 实现划分（`Src/`，按 `Module.mk` 的 `MODULE_SRCS` 登记）

| 文件 | 内容 |
| --- | --- |
| `Module.cpp` | 三个入口、导入回填区与三个导出槽表；`Init` 复位注册表、核对导入面，并按规格 8.6 从 Dev.Model 绑定事实登记设备（可撤销、不申请资源） |
| `BlockRegistry.cpp` | 块设备登记（`RegisterDevicesFromBindings` 是 `Publish` 的唯一调用点）、句柄查询、`Acquire`/`Release` 引用计数与偏移视图子设备（对齐 / 零长度 / 越界 / 64 位回绕判定） |
| `Vfs.cpp` | 文件系统登记、挂载表、统一路径解析（最长挂载点匹配 + 挂载点裁剪）、文件引用、目录遍历与错误码透传 |
| `BootVolume.cpp` | 系统卷定位与挂载：`BootInfo` 卷区段 + 候选设备 + ext4 `VolumeIdentity` 核对，唯一候选才挂载 |

共用存储纯逻辑直接编译 `Packages/Common/Src/Storage/Path.cpp`（统一路径语法与分量切分），不复制第二份实现；`Vfs/Makefile` 把它补成模块 ELF 的显式前置依赖。

## 行为要点（口径见 `Docs/Specs/Common/文件系统抽象与VFS.md`）

- **设备登记的调用链（规格 8.6）**：驱动只经 `Drv.Loader.Bind` 把 `BlockDeviceOps` + `context` 登记进 Dev.Model，**不导入 `Block.Registry`、不调用 `Publish`**；Vfs `Init` 在复位注册表后调 `Dev.Model.Enumerate` 遍历设备句柄，对每个已绑定设备用**零初始化**（`bytes == 0`，查询模式）的 `BindingFacts` 调 `Dev.Model.Binding`；查询返回 `kNotFound` 表示未绑定、跳过；返回 `kOk` 时必须 `state == Started(2)` 且 `ops` 非空才经内部块设备层 `Publish` 登记（`flags = 0`），仍是 `Bound(1)` 未启动或 `ops` 为空说明编排缺失，按 `kCorrupt` 失败上报（`E-KRN-59`），不静默跳过；拉取可重入（已登记句柄跳过），`Abort` 经 `Reset` 撤销全部登记。
- **块设备层**：`Publish` 检查句柄非零、ops 必需槽非空、逻辑块大小为 2 的幂且不小于 512、`flags` 为 0、同一句柄不重复；子设备与根设备共用 `DeviceHandle` 命名空间（子视图从高位段分配），起点按父设备逻辑块对齐、长度非零、区间不越界；子设备的块大小 / 可写 / 刷新原语随父设备。`Acquire` 返回可直接发 I/O 的 ops 表与 `context`。
- **VFS**：登记时检查 9 个 `FileSystemOps` 槽全部非空；公共 `Mount` 按 v1 只接受 `/` 与只读模式，内部按最长挂载点匹配解析（分量边界命中，不做字符串前缀猜测），裁掉挂载点后交给文件系统的卷内路径仍是绝对路径；`Open` 返回 VFS 自有的不透明 `u64` 引用（表下标 + 1，0 非法），`Read`/`Close`/`ReadDirectory` 按引用透传 ops；挂载期间持有设备引用，`Unmount` 在仍有打开引用时拒绝，成功后释放设备。
- **Boot.Volume**：候选为块设备注册表里已登记的整介质根设备；卷标识全零、卷区段缺失 / 回绕 / 越 `mediumBytes`、没有 ext4、候选块大小非法或区段未对齐、偏移视图失败、挂载失败、卷标识取不到或与 `BootInfo.VolumeUuid` 不等都跳过；**唯一**候选全通过才挂载并输出事实，0 个或多个命中都返回 `NotFound`（不猜卷、不降级）。成功后输出 `[ OK ] system volume mounted: uuid=<32 位小写十六进制>`（T5.8 的唯一标记），失败静默返回错误、由内核编排汇总 FATAL。

## 构建

```sh
make -C Packages/KernelModules/Vfs             # 只建 ELF
make -C Packages/KernelModules/Vfs check       # ELF 形态 / 段对齐 / 重定位 / 入口符号
make -C Packages/KernelModules/Vfs contract-check  # 导出/导入/依赖声明按生成契约校验
make -C Packages/KernelModules/Vfs package     # 打包 Out/Modules/Vfs.1.0.0.osm
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/Vfs.1.0.0.osm
```

宿主侧检查在 `Tools/Test/CheckVfs/`（伪块设备 / 伪文件系统 / 伪 Dev.Model / 伪 Core.* 导入槽），不进入模块构建。

## 已知限制

- `BootInfo.layoutOffset` 的布局描述重读与 `DeviceKind` 候选收窄仍未实现（VFS 规格 4.5 暂缓项）：身份只由卷区段事实 + `VolumeIdentity` 与 `BootInfo.VolumeUuid` 逐字节核对。光盘档的 ISO9660 第二来源核对在 M6 由内核核心的最小解析实现（读 PVD/根目录推出 `/SystemVolume.img` 区段，不一致 `E-KRN-77`、不可用 `WARN E-KRN-78`），不在 Vfs 内部，也不改变本模块的挂载判定。
- `SystemVolumeFacts` v1 按契约只有 `bytes` / `volumeUuid` / `buildId` 三个字段；任务里提到的设备句柄与挂载名不在该结构里（不能改契约），设备句柄只出现在日志与内部路径，挂载名固定为 `/`。
