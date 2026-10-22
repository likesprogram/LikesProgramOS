# Module.mk
#    DriverLoader 模块包的构建配置：M5 驱动加载器最小实现（注册 / 匹配 / 绑定 / 启停）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块，清单、预装
#    条目与依赖表全部失配，玩具闭包也不例外
#    契约声明按契约层 2.4：导出 Drv.Loader v1 四槽（唯一提供者），导入 Core.Log / Core.Mem /
#    Dev.Model（设备事实与绑定记录）；驱动拓扑上先装载 DevModel，因此声明依赖 DevModel:1:0

# 模块包名与产物主名：Out/DriverLoader/DriverLoader.elf 与 Out/Modules/DriverLoader.1.0.0.osm
MODULE_NAME := DriverLoader
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := DriverLoader
# 模块版本 major.minor.patch：玩具闭包统一 1.0.0，major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配；新增 .cpp 必须加进这里
# Module.cpp 是入口与契约表，DriverRegistry.cpp 是可在宿主上复用的注册 / 绑定逻辑
MODULE_SRCS := Src/Module.cpp Src/DriverRegistry.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口，玩具模块只需提供 _Module_Init
MODULE_ENTRY_DEFAULTS := 1
# 骨架要求四段齐全：容器段表允许省略空段，样板保留四段便于复核页对齐与段权限
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明（契约层 2.4）：导出项 <稳定名>:<版本>，导入项 <稳定名>:<Min>:<Max>:<使用槽数>
CONTRACT_EXPORTS := Drv.Loader:1
CONTRACT_IMPORTS := Core.Log:1:1:4 Core.Mem:1:1:4 Dev.Model:1:1:6
# 驱动拓扑：DevModel 是 Dev.Model 的唯一提供者，必须先装载
MODULE_DEPENDS := DevModel:1:0
