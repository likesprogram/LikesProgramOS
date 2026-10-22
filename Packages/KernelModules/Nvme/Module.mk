# Module.mk
#    Nvme 模块包的构建配置：M6 G6a NVMe 读路径驱动（ABAR、Admin/I/O 队列、Identify、READ）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块，清单、预装
#    条目与依赖表全部失配，玩具闭包也不例外
#    驱动不导出接口：只经 Drv.Loader 登记 DriverDesc 并把 BlockDeviceOps 随绑定事实交给
#    DriverLoader/DevModel；Block.Registry.Publish 由 Vfs 模块按绑定事实调用（契约层 8.6）

# 模块包名与产物主名：Out/Nvme/Nvme.elf 与 Out/Modules/Nvme.1.0.0.osm
MODULE_NAME := Nvme
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := Nvme
# 模块版本 major.minor.patch：玩具闭包统一 1.0.0，major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配；新增 .cpp 必须加进这里
# Module.cpp 是入口与契约表，Nvme.cpp 是可在宿主上复用的控制器逻辑
MODULE_SRCS := Src/Module.cpp Src/Nvme.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口，玩具模块只需提供 _Module_Init
MODULE_ENTRY_DEFAULTS := 1
# 骨架要求四段齐全：容器段表允许省略空段，样板保留四段便于复核页对齐与段权限
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明：驱动不导出接口
CONTRACT_EXPORTS :=
# 导入项 <稳定名>:<最小版本>:<最大版本>:<使用槽数>
#   Core.Log 3 槽（Write/Line/Hex）；Core.Mem 4 槽（Copy/Zero）；Core.Mmio 2 槽（Map/Unmap）
#   Core.Page 3 槽（PageBytes/Allocate/Free）；Core.PhysMap 1 槽（ToVirt）
#   Dev.Model 5 槽（Enumerate 槽 0、Resources 槽 1、BootCandidates 槽 4；认领由 Drv.Loader.Bind 负责）
#   Drv.Loader 3 槽（RegisterDriver/Bind/Stop）
#   Core.Time 未声明：TicksPerMs 可能为 0，超时全部用有界轮询计数，没有实际调用
CONTRACT_IMPORTS := Core.Log:1:1:3 Core.Mem:1:1:4 Core.Mmio:1:1:2 Core.Page:1:1:3 Core.PhysMap:1:1:1 Dev.Model:1:1:5 Drv.Loader:1:1:3
# 模块依赖：提供者必须先装载（装载器按依赖拓扑排序，不依赖 Modules.list 预装顺序）
#   驱动只依赖 DevModel 与 DriverLoader；Vfs 不在驱动的依赖里（契约层 8.6）
MODULE_DEPENDS := DevModel:1:0 DriverLoader:1:0
