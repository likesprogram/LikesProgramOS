# Module.mk
#    Vfs 模块包的构建配置：M5 存储栈核心，唯一提供 Block.Registry / Fs.Vfs / Boot.Volume
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块，清单、预装
#    条目与依赖表全部失配。模块版本 1.0.0 与玩具骨架相同，主版本跟随 .os 发布线
#    闭包单例提供者固定（契约层 4.3）：本模块是这三个接口的唯一提供者，构建期
#    MakeContract --module-check 按生成头 kClosureProviders 兜住

# 模块包名与产物主名：Out/Vfs/Vfs.elf 与 Out/Modules/Vfs.1.0.0.osm
MODULE_NAME := Vfs
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := Vfs
# 模块版本 major.minor.patch：与玩具骨架一致，major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配；入口、块设备层、VFS 与系统卷挂载分文件
MODULE_SRCS := Src/Module.cpp Src/BlockRegistry.cpp Src/Vfs.cpp Src/BootVolume.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口：本模块只提供 _Module_Init 与 _Module_Abort
MODULE_ENTRY_DEFAULTS := 1
# check 要求存在的段：导出槽表在可写段、字符串与代码在只读 / 文本段，没有独立 .data 段
MODULE_REQUIRED_SECTIONS := .text .rodata .bss
# 契约声明：闭包单例提供者固定（槽数由接口定义推出，不写槽数，契约层 2.4）
CONTRACT_EXPORTS := Block.Registry:1 Fs.Vfs:1 Boot.Volume:1
# 导入：内核核心服务与 Dev.Model（块设备登记来源）；槽数按实际使用
CONTRACT_IMPORTS := Core.Boot:1:1:2 Core.Log:1:1:4 Core.Mem:1:1:4 Core.Page:1:1:5 Dev.Model:1:1:6
# 依赖：Init 时必须能从 Dev.Model 取到驱动绑定事实（预装序 DevModel 在 Vfs 之前）
MODULE_DEPENDS := DevModel:1:0
