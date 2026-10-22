# Xhci — xHCI 主机控制器驱动

M6 G6c 真实实现（M2 骨架已退役，逻辑名 `Xhci`、版本 `1.0.0`、`ModuleId` 不变）。预装序 7：在 UsbCore 之后 Init，找 PCI 上的 xHCI 控制器、映射 ABAR、初始化命令环/事件环，并把 `UsbControllerOps` 交给 `Usb.Host.AddController`。

## 组成

| 文件 | 职责 |
| --- | --- |
| `Src/Module.cpp` | 入口：Dev.Model 找控制器 → Claim → BAR0 映射（Core.Mmio / PhysMap）→ 初始化 → Usb.Host.AddController |
| `Src/Xhci.cpp` | 寄存器层、TRB 编码、命令环/事件环、EP0 控制传输、批量端点（按需 Configure Endpoint）、端口启停与复位 |
| `Src/Xhci.hpp` | 寄存器偏移与位域、`Trb`、CAP/PORTSC 解析、控制器状态、平台注入 |

## 契约面

| 项 | 值 |
| --- | --- |
| `CONTRACT_EXPORTS` | 空（驱动不导出接口） |
| `CONTRACT_IMPORTS` | `Core.Log:1:1:3`、`Core.Mem:1:1:3`、`Core.Page:1:1:3`、`Core.PhysMap:1:1:2`、`Core.Mmio:1:1:2`、`Dev.Model:1:1:6`、`Usb.Host:1:1:2` |
| `MODULE_DEPENDS` | `UsbCore:1:0`、`DevModel:1:0` |

## Init 流程

1. `Dev.Model.Enumerate` 按类码 `0x0C0330`（class 0x0C / subclass 0x03 / progIf 0x30）找控制器，`Claim` 标记控制。
2. `Dev.Model.Resources` 取 BAR0（内存空间；64 位 BAR 由 DevModel 合并），优先 `Core.Mmio.Map`，取不到退回 `Core.PhysMap.ToVirt`（M4 恒等映射）。
3. 校验 CAP：CAPLENGTH/HCIVERSION/HCSPARAMS1（MaxSlots/MaxPorts）/HCCPARAMS1（AC64/CSZ）与 DBOFF/RTSOFF；不成立返回 `kNotSupported`。
4. HCRST → CONFIG.MaxSlotsEn → DCBAAP → CRCR（命令环，RCS=1）→ 事件环（ERSTSZ/ERSTBA/ERDP）→ 关中断 → `USBCMD.RS=1`；失败返回 `kTimeout`。
5. 无控制器时打 `xhci: no device` 并返回成功（闭包模块缺硬件不失败整集）。

DMA 布局：命令环、事件环、事件环段表、DCBAAP、输入上下文、数据弹跳页各一页，均经 `Core.Page.Allocate` 申请（物理地址直写 TRB）；端点环按需在 `EnableDevice`（EP0）与首次批量传输（EP1 IN/OUT）时申请。Abort 时全部归还并释放设备控制权。

## UsbControllerOps 八回调

| 回调 | 行为 |
| --- | --- |
| `SubmitControl` | Setup（8 字节请求内嵌 TRB）+ 可选 Data + Status 三 TRB，门铃后等 Transfer Event，检查完成码与剩余长度 |
| `SubmitBulk` | 端点环按需 `Configure Endpoint`（EP 类型/最大包长按速度），Normal TRB 分块（≤ 一页）搬运 |
| `Poll` | 读事件环（预算内），处理 Port Status Change 事件，返回 `kOk` |
| `PortCount` | `HCSPARAMS1.MaxPorts`（截断到 32） |
| `PortStatus` | 读 PORTSC，映射到 UsbCore 的端口状态位，变化位写 1 清 |
| `PortReset` | 置 PR、等 PR 清零与 PED 置位，清变化位 |
| `EnableDevice` | Enable Slot → 分配 EP0 环与设备上下文 → 填输入上下文 → Address Device |
| `DisableDevice` | Disable Slot → 归还 EP0/端点/设备上下文页 |

## 标识行（供集成断言）

```text
[ XHCI ] xhci: probing controller
[ XHCI ] xhci: no device
[ XHCI ] xhci: controller running
xhci: controller added handle=0x1 ports=0x8 slots=0x10
```

## 宿主检查

`Tools/Test/CheckXhci`（13 项）：CAP 解析与非法输入、Setup/Data/Status/Normal/Link/四类命令 TRB 的位域、PORTSC 解析与速度映射、端口偏移、TRB 字段与完成码（DW2 bit31:24）、输入上下文布局与 Add Context Flags 位（A(n) = DCI n，不是 n+1）、Ops 表非空。

## QEMU 实测修复记录（M6 根端口 usb-storage）

首版在 QEMU 上卡在 Enable Slot/Address Device 超时，用 `-trace enable="usb_xhci_*"` 与 QEMU 源码逐条比对后定位并修复：

| 缺陷 | 现象 | 修复 |
| --- | --- | --- |
| 64 位寄存器拆写顺序 | CRCR/DCBAAP 读回正确但控制器按 0 取命令环 | 先写低 32 位再写高 32 位（低字锁存、高字提交） |
| 运行寄存器少了中断器偏移 | ERSTSZ/ERSTBA/ERDP 写进 MFINDEX 区，事件环未注册 | 中断器 0 从运行寄存器基址 +0x20 起（MFINDEX 占 0x00） |
| 输入控制上下文 Drop/Add 写反 | Address Device 完成码 `CC_TRB_ERROR` | DW0 = Drop Flags（须为 0）、DW1 = Add Flags（A0/A1） |
| Configure Endpoint 的 Add 位偏移 | 端点被使能但 ring 不取 TRB，批量传输超时 | A(n) 对应上下文索引 n（DCI n），即 `1 << dci` |
| 完成码取错 DWord | 把命令完成事件的槽位字段当完成码，TRB 错误被当成功 | 完成码在事件 TRB 的 DW2 bit31:24 |
| EP0 上下文 CErr/Interval | — | CErr = 3、Interval = 0、MPS 按速度（超速 512，其余 64） |
| ERDP 的 EHB | 初始化写 EHB=1 语义不对 | 写 EHB=0；每批事件消费后写新出队指针 |

QEMU 验证（UEFI 从 xHCI 根端口 usb-storage 启动，无 IDE 盘）：

```text
[ OK ] usbstorage: device bound block=0x200 sectors=114688   # 512e
[ OK ] usbstorage: device bound block=0x1000 sectors=14336   # 4Kn（logical_block_size=4096）
[ USB ] usbcore: class bound
[ OK ] system volume mounted: uuid=4c50534f534f4b018000000000000001
[ OK ] system manifest read: bytes=32 digest=ca40278b66583dfc
```

注意：若同一镜像同时挂在 IDE 与 USB 上，Boot.Volume 的候选集合是全部已登记根设备，会命中两个相同卷而按「无法唯一定位」返回 NotFound（E-KRN-71）——xHCI 验证请只挂 USB 盘（UEFI 启动）。

## 已知限制

- 同步轮询、无中断；`Core.Time.TicksPerMs` 为 0，超时判定是次数上限。
- 端点按默认假设配置（超速 max packet 512、其余 64），未读端点描述符；M6 的 usb-storage BOT 端点够用，完整端点配置属后续阶段。
- 只处理根端口上的设备；hub 下游端口由 UsbHub 类驱动逐级枚举。
- QEMU（qemu-xhci + usb-storage 512e/4Kn，UEFI 到挂卷）由 baleen-integration 统一验证。
