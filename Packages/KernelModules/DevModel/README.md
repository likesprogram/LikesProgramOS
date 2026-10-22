# DevModel — M5 设备模型最小实现

`DevModel` 是引导闭包里的设备模型唯一实例，提供 `Dev.Model` v1 六槽与 v2 七槽（v2 只追加登记槽，v1 槽位原序不变）。落地范围是 PCI 配置空间枚举、BAR 解码、设备 / 资源记录、认领与绑定事实，以及 M6 起供 UsbCore 等外部枚举方登记设备的 `Register`（v2 追加槽，v1 槽位原序不变）；平台设备与热插拔属后续阶段。

| 项 | 值 |
| --- | --- |
| `MODULE_NAME` / `MODULE_LOGICAL_NAME` | `DevModel`（与 `Packages/Common/Contract/BootClosure.def` 的 `closure` 行逐字一致） |
| `ModuleId` | `SHA-256("LPMOD01" ‖ "DevModel")[0..16)` = `44caf57b18cba2a640b99a5367b27f4a` |
| 模块版本 | `1.0.0` |
| 导出 | `Dev.Model:1`（六槽，`_Contract_Export_DevModel_v1`）与 `Dev.Model:2`（七槽，`_Contract_Export_DevModel_v2`） |
| 导入 | `Core.Boot:1:1:2`、`Core.Log:1:1:4`、`Core.Mem:1:1:4` |
| ELF 产物 | `Out/DevModel/DevModel.elf`（`PackModule` 的输入，不 strip） |
| 容器产物 | `Out/Modules/DevModel.1.0.0.osm` |

## 源码布局

| 文件 | 职责 |
| --- | --- |
| `Src/Pci.hpp` / `Src/Pci.cpp` | PCI 配置空间枚举与 BAR 解码；配置访问经 `PciAccess` 注入，逻辑可在宿主上逐项断言 |
| `Src/PciPorts.cpp` | 目标侧 0xCF8/0xCFC 机制 1 访问，只在模块 ELF 里参与链接 |
| `Src/DeviceModel.hpp` / `Src/DeviceModel.cpp` | 设备记录与六槽实现：枚举、资源、认领、归还、启动候选、绑定事实 |
| `Src/Module.cpp` | 入口与契约表接线：导入回填区、导出槽表、`_Module_Init` / `_Module_Abort` |

## 设备模型约定（T5.2 定义）

- **总线类别**：`DeviceFacts.busKind` 当前只有 `1` = PCI（`0` 保留给驱动匹配通配）
- **总线地址**：`busAddress` 低 16 位为 `bus << 8 | device << 3 | function`，即 PCI 设备在总线拓扑里的紧凑坐标
- **类码**：`classCode = class << 16 | subclass << 8 | programInterface`，与配置空间 0x08 寄存器的类码三字节一致；大容量存储控制器是主类 `0x01`
- **BAR 标志位**（`DeviceBar.flags`）：`bit0` I/O 空间、`bit1` 64 位 BAR、`bit2` 可预取、`bit3` 未分配（固件没有分配基址）；长度用写全 1 回读法探测，探测后写回原值
- **句柄**：`deviceHandle = 记录下标 + 1`，0 无效；v1 的认领句柄与设备句柄同值
- **绑定事实的三种模式**：契约 v1 的六槽是固定槽、没有单独的绑定登记槽，`Dev.Model.Binding` 用一个槽承载三种模式

| 模式 | 入参 | 行为 |
| --- | --- | --- |
| 查询 | `facts` 零初始化，`bytes == 0` | 未绑定返回 `kNotFound`；已绑定填回事实并返回 `kOk`；调用方（Vfs 块设备层）必须零初始化结构体 |
| 登记 / 更新 | `bytes == sizeof(BindingFacts)` 且 `state != 0` | 设备必须已被认领，否则返回 `kState`；写侧只由 `DriverLoader` 调用，`state` 为 1（Bound，ops / context 为空）或 2（Started，带 ops / context） |
| 清除 | `bytes == sizeof(BindingFacts)` 且 `state == 0` | 清空该设备的绑定事实，设备仍保持认领；`DriverLoader.Stop` 先清除绑定再归还认领 |

其他 `bytes` 取值返回 `kInvalidArgument`。契约 v1 不再新增登记槽；若后续需要显式登记面，按契约 v2 追加（T7.5 记录）。

## Dev.Model v2：设备登记（槽 6）

`Register(const DeviceFacts* facts, uint32_t* deviceHandle)` 把外部枚举发现的设备（USB 等）登记进设备树，供驱动按统一句柄认领与绑定：

| 输入 | 校验 | 结果 |
| --- | --- | --- |
| `facts` / 输出句柄为空 | `kInvalidArgument` | 不改变设备树 |
| `facts->bytes` 小于 `sizeof(DeviceFacts)` | `kInvalidArgument` | 同上 |
| `busKind == 0`、`classCode` 超过 24 位、`vendorId` / `deviceId` 超过 16 位 | `kInvalidArgument` | 同上 |
| 已有记录的 `busKind + busAddress` 相同 | `kAlreadyExists` | 不覆盖既有记录 |
| 设备表已满 32 条 | `kOutOfResources` | 同上 |
| Init 之前调用 | `kState` | 同上 |

成功时句柄 = 记录下标 + 1 写回，事实按 v1 结构原样保存，记录初始未认领、未绑定；启动候选按当前线索规则重新判定。登记后消费者按 v1 语义 `Claim` → 写 `Binding`（`state` 非零，Started 时带 ops / context）→ Vfs 用 v1 `Binding` 查询取到事实并 `Publish`，因此 Vfs 不需要改导入版本。

## 启动线索规则

| `BootInfo.deviceKind` | 候选规则 | 报告 |
| --- | --- | --- |
| `BiosDrive` 且线索字节可读 | 按 `BootInfo.media` 过滤存储控制器子类：硬盘 = IDE / ATA / SATA / NVMe，光盘 = IDE / ATA / SATA | `[ INFO ] devmodel: boot clue matched` |
| `UefiDevicePath`（当前不解析）、`None`、线索不可读 | 退回全部大容量存储控制器，只报候选、不猜设备，最终身份由卷标识核对 | `[ WARN ] devmodel: boot clue fallback to full enumeration` |

设备记录超过 32 条时截断并写 `[ WARN ] devmodel: device table is full`；枚举完成后固定写 `[ INFO ] devmodel: pci enumeration done`。

## 构建与核对

```sh
make -C Packages/KernelModules/DevModel            # 只建 ELF
make -C Packages/KernelModules/DevModel check      # ELF 形态 / 重定位 / 入口约束
make -C Packages/KernelModules/DevModel package    # 打成 Out/Modules/DevModel.1.0.0.osm
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/DevModel.1.0.0.osm
```

契约声明在 `Module.mk`：`CONTRACT_EXPORTS := Dev.Model:1`、`CONTRACT_IMPORTS := Core.Boot:1:1:2 Core.Log:1:1:4 Core.Mem:1:1:4`。`make package` 依赖 T5.1 的 MakeContract 模块校验把槽数翻译成 PackModule 参数；该接线未就绪前可用等价的显式命令行打包（`--export Dev.Model:1:6` 等）。

宿主检查见 `Tools/Test/CheckDevModel`：伪 PCI 配置空间、假 BootInfo 与日志收集器覆盖枚举、BAR、启动候选回退与绑定流程。
