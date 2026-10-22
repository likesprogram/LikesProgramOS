# Ahci — AHCI 读路径驱动

M5 起真实驱动（M2 骨架已退役，逻辑名 `Ahci`、版本 `1.0.0`、`ModuleId` 不变）。提供启动期与运行期共用的同一实例：初始化时找控制器、映射 ABAR、初始化端口并发布块设备；命令完成用轮询，不启用中断与异步。

## 组成

| 文件 | 职责 |
| --- | --- |
| `Src/Module.cpp` | 模块入口：导入契约、`Dev.Model` 找控制器与 BAR、ABAR 映射、DMA 页申请、登记与发布、`_Module_Abort` 撤销 |
| `Src/Ahci.cpp` | 端口选择与初始化、命令列表 + PRDT、IDENTIFY、READ DMA EXT、完成轮询、超时与端口复位 |
| `Src/Ahci.hpp` | 控制器/端口事实（ABAR、命令列表/FIS/命令表/数据弹跳页的物理与虚拟地址）与初始化/读接口 |

AHCI 命令头与 LBA/扇区换算复用 `Packages/Common/Include/Storage/AhciCommand.hpp`（`Storage::AhciCommand`，Baleen 刷盘路径同一份语义），模块不复制编码规则。

## 契约面

| 项 | 值 |
| --- | --- |
| `CONTRACT_EXPORTS` | 空（驱动不导出接口） |
| `CONTRACT_IMPORTS` | `Core.Log:1:1:3`、`Core.Mmio:1:1:2`、`Core.Page:1:1:3`、`Core.PhysMap:1:1:1`、`Dev.Model:1:1:5`、`Drv.Loader:1:1:3` |
| `MODULE_DEPENDS` | `DevModel:1:0`、`DriverLoader:1:0`（驱动不依赖 Vfs——块设备由 Vfs 按绑定事实登记，契约层 8.6） |

## ABAR 映射的选择

优先 `Core.Mmio.Map(barBase, barBytes, 0)`；M5 的 `Core.Mmio` 在直映射上实现（返回 `PhysToVirt` 指针，无缓存策略），取不到时退回 `Core.PhysMap.ToVirt(barBase)` 的直映射（M4 恒等映射存在）。两条路径都失败时打 `WARN` 并跳过发布，不猜地址。

## DMA 缓冲布局

`Core.Page.Allocate(1)` 申请两页：第 0 页放命令列表（页首，1KiB 对齐）、FIS 接收区（`+0x400`，256 字节对齐）、命令表（`+0x800`，128 字节对齐）；第 1 页是数据弹跳页。物理地址经 `Core.PhysMap.ToVirt` 取虚拟地址；单条命令的传输量不超过一页，`Read` 按页分块并把数据拷到调用方缓冲。

## 发现与注册流程

1. `Init`：在 `Dev.Model` 里找 AHCI 控制器（`BootCandidates` 候选优先、`Enumerate` 兜底），按类码匹配（大容量存储 0x01 / SATA 子类 0x06）；认领由 `Drv.Loader.Bind` 负责——驱动不自行 `Claim`，否则 `Bind` 内部再 Claim 会返回 `kBusy`。
2. `Dev.Model.Resources` 取 BAR 表，优先 `index == 5` 的内存 BAR 作为 ABAR（没有就取第一个内存 BAR），长度缺省按 0x2000。
3. 找第一个 `PxSSTS.DET == 3` 且 `PxSIG == 0x101` 的端口；停端口 → 装命令列表与 FIS 接收区 → 启 FRE/ST → IDENTIFY（失败先端口复位再试一次）。
4. `Drv.Loader.RegisterDriver` + `Bind`；`DriverDesc.ops/context` 随绑定事实写进 `DevModel`，由 `Vfs` 在它自己的 `Init` 里按绑定事实调用 `Block.Registry.Publish`（契约层 8.6：驱动不调用 Publish、不依赖 Vfs）。
5. `_Module_Abort`：`Drv.Loader.Stop`（认领与释放都归 Bind/Stop）→ 停端口 → 归还 DMA 页。

没有控制器时 `Init` 仍返回成功并打 `ahci: no device`；初始化端口失败打 `WARN` 并归还 DMA 页：闭包模块是必需模块，缺硬件不应让整集装载失败。

## 命令与错误处理

- 命令完成：置 `PxCI` 本位后轮询该位清零，同时检查 `PxIS.TFES` 与 `PxTFD.ERR`；超时上限是次数而不是时间基。
- 超时或任务文件错误：走端口复位（停 ST/FRE → `PxSCTL.DET = 1` 保持 → 等设备重新就绪 → 重装命令列表与 FIS），复位后再发一次；仍失败返回 `Storage::Error::Io`。
- `BlockBytes` = 512；`CapacityBytes` = 扇区数 × 512；`Writable` = 1（本阶段只实现读，`Write` 返回 `ReadOnly`）；能力位 0。

## 标识行（供 T5.8 断言）

```text
ahci: port=0x<端口> <型号> sectors=0x<扇区数> block=0x200
[ AHCI ] ahci: probing controller
[ AHCI ] ahci: driver started
[ OK ] ahci: driver bound, block device handed to devmodel
```

## 已知限制

- 中断与异步不启用；无写路径、无 FLUSH CACHE 原语。
- 只处理 ATA 设备（签名 0x0101），ATAPI 端口跳过；一次只发布一个块设备（设备句柄是控制器粒度）。
- 端口复位用有界空转近似 COMRESET 保持时间（`Core.Time.TicksPerMs` 尚为 0）。
- `DeviceFacts.classCode` 与 `DeviceBar.flags` 的编码按两种常见形态兼容匹配。
- 块设备登记由 Vfs 按绑定事实完成，驱动不做 Publish（契约层 8.6）。
- QEMU 实机读扇区由 baleen-integration 的 M5 集成阶段统一验证，本模块只保证宿主侧构建与契约打包。
