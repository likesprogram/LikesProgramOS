# Module.mk
#    UsbCore 模块包的构建配置：Usb.Host v1 的提供者（USB 主机核心）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块
#    提供者唯一：Usb.Host v1 的闭包提供者固定为 UsbCore（生成头 kClosureProviders）

# 模块包名与产物主名：Out/UsbCore/UsbCore.elf 与 Out/Modules/UsbCore.1.0.0.osm
MODULE_NAME := UsbCore
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := UsbCore
# 模块版本 major.minor.patch：major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配
MODULE_SRCS := Src/Module.cpp Src/UsbCore.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口，模块提供 _Module_Init 与 _Module_Abort
MODULE_ENTRY_DEFAULTS := 1
# 骨架要求四段齐全：容器段表允许省略空段，样板保留四段便于复核页对齐与段权限
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明：UsbCore 唯一提供 Usb.Host v1（6 槽）
CONTRACT_EXPORTS := Usb.Host:1
# 导入项 <稳定名>:<最小版本>:<最大版本>:<使用槽数>
#   Core.Log 3 槽（Write/Line/Hex）：控制器/设备/绑定的标识行；不依赖其它模块
CONTRACT_IMPORTS := Core.Log:1:1:3
# 无模块依赖：UsbCore 是 USB 链路的提供者，先于 Xhci/UsbHub/UsbStorage Init
