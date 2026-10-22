# UsbStorage — M6 USB 大容量存储类驱动（G6c）

`UsbStorage` 是引导闭包成员，逻辑名/版本 `UsbStorage` 1.0.0。它是 USB BOT/SCSI 透明命令集类驱动：经 `Usb.Host.RegisterClass(0x08)` 登记，UsbCore 枚举到接口类 `0x08`/子类 `0x06`/协议 `0x50` 的设备后回调 Probe/Bind；**不导出任何接口**，也不导入 `Block.Registry`。

| 项 | 值 |
| --- | --- |
| 导出 | 无（块设备经 Dev.Model 绑定事实交给 Vfs 登记） |
| 导入 | `Usb.Host:1:1:4`、`Dev.Model:2:2:7`（Register/Claim/Binding/Release）、`Core.Log:1:1:4`、`Core.Mem:1:1:4`、`Core.Page:1:1:5` |
| 依赖 | `UsbCore:1:0` |
| 产物 | `Out/UsbStorage/UsbStorage.elf`、`Out/Modules/UsbStorage.1.0.0.osm` |

实现（`Src/Module.cpp`）：Bind 读配置描述符找 Bulk IN/OUT 端点与最大包长；BOT 探测 = CBW（`USBC` 签名）→ CSW（`USBS` 签名、标签、残差、状态校验）的 INQUIRY 与 READ CAPACITY(10)（支持 512 与 4096 字节块）；构造模块内静态 `BlockDeviceOps`（只读、`capabilities=0`、Flush=Ok），READ(10) 分包读；随后 `Dev.Model.Register`（busKind=2 USB、classCode=`0x080650`、vendor/product、busAddress 自选编码）→ `Claim` → `Binding` 登记/更新模式写 `state=Started(2)`+ops/context；Abort/Unbind 清绑定并 `Release`。无设备输出 `usbstorage: no device`，成功输出 `usbstorage: device bound block=0x<块大小> sectors=<块数>`。

构建：`make -C Packages/KernelModules/UsbStorage [check|contract-check|package]`；宿主检查 `Tools/Test/CheckUsbStorage`（15 例）。
