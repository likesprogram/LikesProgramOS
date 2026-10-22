# UsbHub — M6 USB hub 类驱动（G6c）

`UsbHub` 是引导闭包成员，逻辑名/版本 `UsbHub` 1.0.0（与 `BootClosure.def` 一致）。它是 USB hub 的类驱动：经 `Usb.Host.RegisterClass(0x09)` 登记，UsbCore 枚举到 hub 后回调 Probe/Bind/Unbind；**不导出任何接口**。

| 项 | 值 |
| --- | --- |
| 导出 | 无（类驱动经 RegisterClass 注册） |
| 导入 | `Usb.Host:1:1:4`（AddController/RegisterClass/ControlTransfer/BulkTransfer）、`Core.Log:1:1:4`、`Core.Mem:1:1:4` |
| 依赖 | `UsbCore:1:0` |
| 产物 | `Out/UsbHub/UsbHub.elf`、`Out/Modules/UsbHub.1.0.0.osm` |

实现（`Src/Module.cpp`）：Probe 校验接口类 `0x09` 且子类 `0x00`/`0x01`；Bind 用控制传输读 Hub Class Descriptor（GET_DESCRIPTOR type `0x29`），按端口数逐端口 `SET_PORT_FEATURE(PORT_POWER)`，再 `GET_PORT_STATUS` 统计连接端口，对带 `C_PORT_CONNECTION` 的端口 `CLEAR_PORT_FEATURE`；成功后输出 `usbhub: hub bound ports=N connected=M`，没有 hub 设备时输出 `usbhub: no hub device` 并成功返回；Abort 清模块状态。

**已知限制**：Usb.Host v1 只枚举控制器根端口，`UsbClassOps` 没有 hub 下游枚举/重扫回调，本模块只处理 hub 自身描述符/端口电源/连接变化；下游设备经 hub 的枚举需要 UsbCore 内部支持（不改契约）。

构建：`make -C Packages/KernelModules/UsbHub [check|contract-check|package]`；宿主检查 `Tools/Test/CheckUsbHub`（8 例）。
