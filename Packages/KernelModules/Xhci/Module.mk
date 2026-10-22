# Module.mk
#    Xhci 模块包的构建配置：xHCI 主机控制器驱动（把 UsbControllerOps 交给 Usb.Host）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块
#    驱动不导出接口：控制器经 Usb.Host.AddController 交给 UsbCore，设备登记由 UsbCore 负责

# 模块包名与产物主名：Out/Xhci/Xhci.elf 与 Out/Modules/Xhci.1.0.0.osm
MODULE_NAME := Xhci
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := Xhci
# 模块版本 major.minor.patch：major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配
MODULE_SRCS := Src/Module.cpp Src/Xhci.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口，模块提供 _Module_Init 与 _Module_Abort
MODULE_ENTRY_DEFAULTS := 1
# 骨架要求四段齐全：容器段表允许省略空段，样板保留四段便于复核页对齐与段权限
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明：驱动不导出接口
CONTRACT_EXPORTS :=
# 导入项 <稳定名>:<最小版本>:<最大版本>:<使用槽数>
#   Core.Log 3 槽（Write/Line/Hex）；Core.Mem 3 槽（Copy/CopyOverlap/Zero）
#   Core.Page 3 槽（PageBytes/Allocate/Free）；Core.PhysMap 2 槽（ToVirt/ToPhys）
#   Core.Mmio 2 槽（Map/Unmap）；Dev.Model 6 槽（v1：Enumerate/Resources/Claim/Release/BootCandidates/Binding）
#   Usb.Host 2 槽（AddController 与余量）
CONTRACT_IMPORTS := Core.Log:1:1:3 Core.Mem:1:1:3 Core.Page:1:1:3 Core.PhysMap:1:1:2 Core.Mmio:1:1:2 Dev.Model:1:1:6 Usb.Host:1:1:2
# 模块依赖：UsbCore（Usb.Host 提供者）与 DevModel（PCI 事实提供者）必须先装载
MODULE_DEPENDS := UsbCore:1:0 DevModel:1:0
