# Nvme — M6 G6a NVMe 读路径驱动

`Nvme` 是引导闭包里的 NVMe 驱动：从 `Dev.Model` 取 PCI 事实与 BAR0，经 `Core.Mmio` 映射 ABAR，初始化控制器、Admin 队列与 I/O 队列，`Identify` 后按 LBA 格式上报逻辑块大小（4Kn 4096 / 512e 512），读路径用 PRP1 加数据弹跳页分块、轮询完成队列阶段位。中断与异步属阶段 3 之后。

| 项 | 值 |
| --- | --- |
| `MODULE_NAME` / `MODULE_LOGICAL_NAME` | `Nvme`（与 `Packages/Common/Contract/BootClosure.def` 的 `closure` 行逐字一致） |
| `ModuleId` | `SHA-256("LPMOD01" ‖ "Nvme")[0..16)` = `a9aa07d07b3b63e259b9026c5965f450` |
| 模块版本 | `1.0.0` |
| 导出 | 无（驱动不导出接口，只经 `Drv.Loader` 登记 `DriverDesc`） |
| 导入 | `Core.Log:1:1:3`、`Core.Mem:1:1:4`、`Core.Mmio:1:1:2`、`Core.Page:1:1:3`、`Core.PhysMap:1:1:1`、`Dev.Model:1:1:5`、`Drv.Loader:1:1:3` |
| 依赖 | `DevModel:1:0 DriverLoader:1:0` |
| ELF 产物 | `Out/Nvme/Nvme.elf`；容器产物 `Out/Modules/Nvme.1.0.0.osm` |

`Core.Time` 未声明导入：`TicksPerMs` 可能为 0，超时全部用有界轮询计数（寄存器与命令各一个固定上界），没有实际调用。

## 源码布局

| 文件 | 职责 |
| --- | --- |
| `Src/NvmeRegisters.hpp` | 寄存器偏移与位域、队列条目、命令操作码/状态码、Identify 字节表（唯一来源，宿主夹具共用） |
| `Src/Nvme.hpp` / `Src/Nvme.cpp` | 控制器初始化、Admin/I/O 队列、Identify 解析、READ、错误映射、`BlockDeviceOps` 表 |
| `Src/Module.cpp` | 入口与契约表接线：找控制器、映射 BAR0、申请四页缓冲、登记 `DriverDesc` 与绑定、`Abort` |

## 控制器与队列事实

- **寄存器**（BAR0，按 32 位访问）：`CAP` 0x00（MQES、TO、DSTRD、CSS 的 NVM 位、MPSMIN）、`VS` 0x08（只接受主版本 1）、`CC` 0x14（EN、CSS=0、MPS=0、IOSQES=6、IOCQES=4）、`CSTS` 0x1C（RDY/CFS）、`AQA` 0x24、`ASQ` 0x28、`ACQ` 0x30；门铃区 0x1000 起，步长 `4 << CAP.DSTRD`
- **队列**：Admin 深度 `min(CAP.MQES+1, 16)`、I/O 深度 `min(CAP.MQES+1, 64)`，至少两条；**每张队列独占一页**——Admin SQ、Admin CQ、I/O SQ、I/O CQ、数据弹跳页共五页。NVMe 要求 ASQ/ACQ 按内存页对齐，I/O 队列基址随创建命令的 PRP1 给出：QEMU 的 `nvme_start_ctrl` 会以 `startfail_acq_misaligned` 拒绝未对齐的完成队列，缺 PRP1 时设备会从物理地址 0 取命令（表现为全零条目被当成 FLUSH 并回 0x400b）。完成队列在使用前清零
- **Identify**：Controller（CNS=1）取 `MN`（0x18..0x3F）、`MDTS`（0x4D）、`SQES`/`CQES`（0x200/0x201）、`NN`（0x204）；Namespace（CNS=0，NSID=1）取 `NSZE`（0x00）、`NLBAF`（0x19）、`FLBAS`（0x1A）与 `LBAF[16]`（0x80..0xBF，`LBADS` 在项内 bit23..16）。偏移经 Linux `nvme.h` 与 NVMe 规范核对（[nvme_id_ctrl](https://manpages.ubuntu.com/manpages/jammy/man2/nvme_id_ctrl.2.html)、[NVME_IDENTIFY_NAMESPACE_DATA](https://learn.microsoft.com/en-us/windows/win32/api/nvme/ns-nvme-nvme_identify_namespace_data)）
- **读路径**：`Read`（0x02），SLBA 在 CDW10/11、NLB 在 CDW12 低 16 位（0 基）；每条命令一个 PRP1，按数据页分块；完成轮询阶段位，期望阶段从 1 起、绕队列一圈翻转；完成条目由设备写，按 `volatile` 读（每圈都真正访问内存）
- **错误映射**：状态码经 `MapStatus` 落到 `Storage::Error`：非法字段/LBA 越界/容量超限 → `InvalidArgument`，非法操作码/命名空间未就绪 → `NotSupported`，保留冲突/写只读 → `ReadOnly`，其余（数据传输、内部错误、格式进行中、未知）→ `Io`；超时与队列不可用按 `Io` 上报

## 初始化结果与失败语义

| 情况 | 返回值 | 诊断行 |
| --- | --- | --- |
| 没有 NVMe 控制器 | `kOk`（非 NVMe 启动档不失败） | `[ WARN ] nvme: no device` |
| BAR0 缺失或不可映射、页帧不足 | `kOk`（跳过发布，不猜地址） | `[ WARN ] nvme: BAR0 not found in devmodel resources` / `[ WARN ] nvme: BAR0 not mappable` / `[ WARN ] nvme: DMA pages not available` |
| `CSTS.RDY` 未就绪 | `kTimeout` | `[ WARN ] nvme: controller not ready` |
| VS/命令集/队列深度/条目大小不支持 | `kNotSupported` | `[ WARN ] nvme: controller not supported` |
| Identify 数据非法（容量为零、LBA 格式下标越界） | `kCorrupt` | `[ WARN ] nvme: identify data invalid` |
| 其它控制器错误 | `kIo` | `[ WARN ] nvme: controller init failed` |

## 可观测标记

| 场景 | 串口行 |
| --- | --- |
| 探测开始 | `[ NVME ] nvme: probing controller` |
| 初始化完成 | `[ NVME ] nvme: controller ready` |
| 设备标识行（无标签） | `nvme: QEMU HARDDISK sectors=0x400000 block=0x1000`（512e 档为 `block=0x200`） |
| 登记与绑定 | `[ NVME ] nvme: driver descriptor registered`、`[ NVME ] nvme: device bound`、`[ OK ] nvme: driver bound, block device handed to devmodel` |
| 驱动启用 | `[ NVME ] nvme: driver started` |

`BlockDeviceOps` 随绑定事实交给 `DevModel`，由 Vfs 的块设备层调用 `Block.Registry.Publish`；驱动不导入 `Block.Registry`、不依赖 Vfs（契约层 8.6）。

## 构建与核对

```sh
make -C Packages/KernelModules/Nvme contract-check   # 契约声明与提供者一致
make -C Packages/KernelModules/Nvme check            # ELF 形态 / 重定位 / 入口约束
make -C Packages/KernelModules/Nvme package          # 打成 Out/Modules/Nvme.1.0.0.osm
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/Nvme.1.0.0.osm
```

宿主检查见 `Tools/Test/CheckNvme`：伪寄存器阵列 + 伪设备命令执行覆盖初始化、Identify 解析、命令编码、阶段位翻转、超时与错误映射、4Kn/512e 上报。
