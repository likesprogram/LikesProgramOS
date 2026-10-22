# Module.mk
#    DevModel 模块包的构建配置：M5 设备模型最小实现（PCI 枚举 / BAR / 启动线索 / 设备记录）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块，清单、预装
#    条目与依赖表全部失配，闭包成员也不例外
#    契约声明按契约层 2.4：导出 Dev.Model v1 六槽与 v2 七槽（同一提供者、两个版本并存），
#    导入 Core.Boot / Core.Log / Core.Mem；槽数由 MakeContract 从 Interfaces.def 推出后交给 PackModule

# 模块包名与产物主名：Out/DevModel/DevModel.elf 与 Out/Modules/DevModel.1.0.0.osm
MODULE_NAME := DevModel
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := DevModel
# 模块版本 major.minor.patch：玩具闭包统一 1.0.0，major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配；新增 .cpp 必须加进这里
# Module.cpp 是入口与契约表，Pci.cpp / DeviceModel.cpp 是可在宿主上复用的枚举与记录逻辑，
# PciPorts.cpp 只在目标侧提供 0xCF8/0xCFC 访问（宿主检查不编译）
MODULE_SRCS := Src/Module.cpp Src/Pci.cpp Src/PciPorts.cpp Src/DeviceModel.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口，玩具模块只需提供 _Module_Init
MODULE_ENTRY_DEFAULTS := 1
# 骨架要求四段齐全：容器段表允许省略空段，样板保留四段便于复核页对齐与段权限
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明（契约层 2.4）：导出项 <稳定名>:<版本>，导入项 <稳定名>:<Min>:<Max>:<使用槽数>
CONTRACT_EXPORTS := Dev.Model:1 Dev.Model:2
CONTRACT_IMPORTS := Core.Boot:1:1:2 Core.Log:1:1:4 Core.Mem:1:1:4
