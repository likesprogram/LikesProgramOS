# Module.mk
#    UsbStorage 模块包的构建配置：M6 引导闭包成员（真实实现，逻辑名与版本保持 1.0.0）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生；导出为空（类驱动经 Usb.Host.RegisterClass 注册，不导出单例接口）

# 模块包名与产物主名
MODULE_NAME := UsbStorage
# 参与 ModuleId 派生的逻辑名
MODULE_LOGICAL_NAME := UsbStorage
# 模块版本 major.minor.patch：保持玩具骨架的 1.0.0
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：入口与类驱动实现同文件
MODULE_SRCS := Src/Module.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口
MODULE_ENTRY_DEFAULTS := 1
# check 要求存在的段：代码 / 常量在只读与文本段，状态与导入回填区在 BSS
MODULE_REQUIRED_SECTIONS := .text .rodata .bss
# 契约声明：不导出；导入 Usb.Host 前 4 槽（AddController/RegisterClass/ControlTransfer/BulkTransfer）
CONTRACT_EXPORTS :=
CONTRACT_IMPORTS := Usb.Host:1:1:4 Dev.Model:2:2:7 Core.Log:1:1:4 Core.Mem:1:1:4 Core.Page:1:1:5
# 依赖：UsbCore 先装载（提供 Usb.Host）；Dev.Model v2 提供 Register/Claim/Binding
MODULE_DEPENDS := UsbCore:1:0
