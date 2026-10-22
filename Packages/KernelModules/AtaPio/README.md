# AtaPio — 传统 ATA / ATAPI 的 PIO 只读驱动

M5 起真实驱动（M2 骨架已退役，逻辑名 `AtaPio`、版本 `1.0.0`、`ModuleId` 不变）。提供启动期与运行期共用的同一实例：初始化时探测设备并发布块设备，读路径同步轮询，不启用中断与异步。

## 组成

| 文件 | 职责 |
| --- | --- |
| `Src/Module.cpp` | 模块入口：导入契约、探测、选盘、`Drv.Loader` 登记与绑定、`_Module_Abort` 撤销 |
| `Src/AtaPio.cpp` | 端口 I/O（`in`/`out` 内联汇编）、IDENTIFY DEVICE / IDENTIFY PACKET、READ SECTORS / READ SECTORS EXT、ATAPI READ(10)/(12) |
| `Src/AtaPio.hpp` | 盘位事实（通道、盘位、类型、扇区大小、容量、型号）与探测/读接口 |

## 契约面

| 项 | 值 |
| --- | --- |
| `CONTRACT_EXPORTS` | 空（驱动不导出接口） |
| `CONTRACT_IMPORTS` | `Core.Boot:1:1:1`、`Core.Log:1:1:3`、`Dev.Model:1:1:5`、`Drv.Loader:1:1:3` |
| `MODULE_DEPENDS` | `DevModel:1:0`、`DriverLoader:1:0`（提供者先装载；驱动不依赖 Vfs——块设备由 Vfs 按绑定事实登记，契约层 8.6） |

## 发现与注册流程

1. `Init`：探测主/从通道的四个盘位（先试 IDENTIFY PACKET 0xA1，失败退回 IDENTIFY DEVICE 0xEC）；每盘位读型号、逻辑扇区大小与容量。
2. 用 `Core.Boot.Info` 的 `media` 选启动盘：`Hdd` 优先 ATA、`Cdrom` 优先 ATAPI；介质未知时先 ATA 后 ATAPI。
3. 在 `Dev.Model` 里找 IDE 控制器：`BootCandidates` 给出的启动候选优先，`Enumerate` 全量枚举兜底；按类码匹配（大容量存储 0x01 / IDE 子类 0x01）拿设备句柄；认领由 `Drv.Loader.Bind` 负责——驱动不自行 `Claim`，否则 `Bind` 内部再 Claim 会返回 `kBusy`。
4. `Drv.Loader.RegisterDriver(&DriverDesc)` 登记驱动描述，`Bind` 绑定设备句柄；`DriverDesc.ops/context` 随绑定事实写进 `DevModel`，由 `Vfs` 在它自己的 `Init` 里按绑定事实调用 `Block.Registry.Publish`（契约层 8.6：驱动不调用 Publish、不依赖 Vfs）。
5. `_Module_Abort` 只做 `Drv.Loader.Stop`（认领与释放都归 Bind/Stop，驱动不另调 `Dev.Model.Release`）。

没有设备、或 `Dev.Model` 里找不到控制器时 `Init` 仍返回成功并打 `ata: no device` / 带 `WARN` 的诊断：闭包模块是必需模块，缺硬件不应让整集装载失败；挂卷失败由挂载阶段的诊断编号报告。

## 块设备能力

- `BlockBytes` = 设备逻辑扇区大小（ATA 512；ATAPI 按 READ CAPACITY 的块长，通常 2048）
- `CapacityBytes` = 扇区数 × 扇区大小
- `Writable`：ATA 盘为 1、ATAPI 为 0；本阶段只有读路径，`Write` 返回 `ReadOnly`，`Flush` 返回 `Ok`，能力位为 0（没有真实刷新原语）
- `Read`：偏移与长度必须按逻辑扇区对齐且落在容量内，否则 `InvalidArgument`；底层超时/错误返回 `Io`；参数与成功路径不分配内存（缓冲由调用方提供）

## 标识行（供 T5.8 断言）

串口输出（`Core.Log.Write` 原样行，CRLF 结尾）：

```text
ata: <型号> ata sectors=0x<扇区数> block=0x200
ata: <型号> atapi sectors=0x<扇区数> block=0x800
ata: found=0x<n>
ata: boot drive index=0x<idx> media=0x<media>
[ ATA ] ata: probing legacy channels
[ ATA ] ata: driver started
[ OK ] ata: driver bound, block device handed to devmodel
```

## 已知限制

- 中断与异步不启用（本阶段刻意）；无写路径、无 FLUSH CACHE 原语，`Flush` 只报成功。
- 一次只发布一个块设备（Dev.Model 的设备句柄是控制器粒度）：按 `BootInfo.media` 选启动盘。
- 端口轮询用有界次数而不是时间基（`Core.Time.TicksPerMs` 尚为 0），超时判定是"次数上限"。
- `DeviceFacts.classCode` 的编码按"完整 class 寄存器"与"三段编码"两种形态兼容匹配。
- 块设备登记由 Vfs 按绑定事实完成，驱动不做 Publish（契约层 8.6）；`BindingFacts` 的 `Start` 成功后 ops/context 才有效。
- QEMU 实机读扇区由 baleen-integration 的 M5 集成阶段统一验证，本模块只保证宿主侧构建与契约打包。
