# UsbCore — USB 主机核心（`Usb.Host` v1 提供者）

M6 G6c 真实实现（M2 骨架已退役，逻辑名 `UsbCore`、版本 `1.0.0`、`ModuleId` 不变）。预装序 6：在 Xhci 之前 Init，提供 Usb.Host v1 六槽，供 Xhci 登记控制器、UsbHub/UsbStorage 登记类驱动（晚注册），并给内核编排提供 Poll 推进枚举。

## 组成

| 文件 | 职责 |
| --- | --- |
| `Src/Module.cpp` | 入口与 Usb.Host 六槽的转发；导出槽表 `_Contract_Export_UsbHost_v1`（`LP_CONTRACT_EXPORT(UsbHost, 1, UsbHostSlotsV1)`） |
| `Src/UsbCore.cpp` | 控制器/类驱动/设备三张定长表、枚举状态机、控制/批量传输路由 |
| `Src/UsbCore.hpp` | 表结构、端口状态位约定、平台注入与 API |

## 契约面

| 项 | 值 |
| --- | --- |
| `CONTRACT_EXPORTS` | `Usb.Host:1`（6 槽，闭包提供者固定为 UsbCore） |
| `CONTRACT_IMPORTS` | `Core.Log:1:1:3`（Write/Line/Hex） |
| `MODULE_DEPENDS` | 无（USB 链路的提供者，先于 Xhci/UsbHub/UsbStorage Init） |

## Usb.Host v1 六槽

| 槽 | 行为 |
| --- | --- |
| `AddController` | 存控制器 `UsbControllerOps + context`，按 `PortCount` 记录端口数；控制器表 2 条 |
| `RegisterClass` | 存类驱动（接口类码 → `UsbClassOps`）；**晚注册**：对已枚举未绑定设备立即重扫 Probe/Bind；类表 4 条 |
| `ControlTransfer` | 按设备句柄路由到所属控制器的 `SubmitControl`（设备地址由枚举时的 `EnableDevice` 分配） |
| `BulkTransfer` | 按设备句柄与端点数路由到 `SubmitBulk` |
| `Poll` | 推进每个控制器的事件（`ops.Poll`）与端口状态机；**可多次调用、幂等**，参数是本次预算（0 = 默认 16），没有进展也返回 `kOk` |
| `DeviceFacts` | 返回 `UsbDeviceFacts`（句柄、控制器、接口类码/子类/协议、VID/PID、速度、根端口） |

枚举状态机：端口出现连接 → `PortReset` → `EnableDevice(port, address)` → `GET_DESCRIPTOR(Device)` → `GET_DESCRIPTOR(Configuration)` → 解析 `bInterfaceClass/SubClass/Protocol` → 按接口类码匹配类驱动 → `Probe` 接管后 `Bind`。设备表 16 条，句柄 = 下标 + 1。

端口状态位约定（本模块与 Xhci 共用，见 `UsbCore.hpp`）：bit0 连接、bit1 变化（读清）、bit2 已启用、bit3 复位中、bit4..7 速度（1 低速 / 2 全速 / 3 高速 / 4 超速）。

## 标识行（供集成断言）

```text
[ USB ] usbcore: ready
[ USB ] usbcore: controller added
usbcore: controller ports=0x<端口数>
[ USB ] usbcore: device enumerated
usbcore: device class=0x<接口类码>
[ USB ] usbcore: class bound
usbcore: bound class=0x<接口类码>
[ USB ] usbcore: class registered
[ USB ] usbcore: device removed
[ WARN ] usbcore: device table is full
```

## 宿主检查

`Tools/Test/CheckUsbCore`（12 项）：假控制器 ops + 假类驱动，覆盖控制器/类表容量、枚举状态机与设备事实、晚注册重扫、Probe 拒绝、控制/批量路由、设备移除（Unbind + DisableDevice）、Poll 幂等、设备表满、多控制器路由、复位与描述符失败。

## 已知限制

- 设备表 16、类驱动表 4、控制器表 2（定长 POD，无动态分配）。
- 枚举只处理根端口（hub 下游端口由 UsbHub 类驱动经 ControlTransfer 逐级枚举，属 UsbHub 任务）。
- 无中断/异步：Poll 由内核编排在模块 Init 之间调用，事件靠控制器 `Poll` 推进。
- 类码匹配只看接口类码字节；子类/协议在 `UsbDeviceFacts` 里给出，必要时由类驱动 Probe 自行细分。
- QEMU 端到端（qemu-xhci + usb-storage，UEFI 到挂卷）由 baleen-integration 统一验证。
