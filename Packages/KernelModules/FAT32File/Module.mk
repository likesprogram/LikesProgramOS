# Module.mk
#    FAT32File 模块包的构建配置：引导闭包成员，M6 起为真实只读实现（逻辑名/版本保持 1.0.0）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块
#    实现：Src/Fat32Volume.cpp（BPB/FAT 链/目录项 8.3 与 LFN/读取）+ Src/Fat32Ops.cpp
#    （FileSystemOps 九槽表）+ Src/Module.cpp（入口、导入回填区与登记）

# 模块包名与产物主名：Out/FAT32File/FAT32File.elf 与 Out/Modules/FAT32File.1.0.0.osm
MODULE_NAME := FAT32File
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := FAT32File
# 模块版本 major.minor.patch：major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配；新增 .cpp 必须加进这里
#   Module.cpp       入口、导入回填区、向 Fs.Vfs 登记
#   Fat32Volume.cpp  只读卷解析（BPB / FAT 链 / 目录项 / LFN / 读取）
#   Fat32Ops.cpp     FileSystemOps 九槽表与模块内静态卷实例
MODULE_SRCS := Src/Module.cpp Src/Fat32Volume.cpp Src/Fat32Ops.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口：模块提供 _Module_Init / _Module_Abort
MODULE_ENTRY_DEFAULTS := 1
# 四段齐全：卷解析的常量与格式名在 .rodata，自描述表在 .data（RELATIVE），
# 导入回填区与卷实例在 .bss
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明（由 MakeContract --module-check 校验后传给 PackModule）
#   导出：无——文件系统实例不导出单例接口，登记由 Vfs 的 Fs.Vfs 持有
#   导入：Fs.Vfs 8 槽、Core.Log 4 槽（Init 失败原因）、Core.Mem 4 槽（内存服务）
CONTRACT_EXPORTS :=
CONTRACT_IMPORTS := Fs.Vfs:1:1:8 Core.Log:1:1:4 Core.Mem:1:1:4
# 依赖：Vfs 先装载（提供者先于消费者，装载器按依赖拓扑排序）
MODULE_DEPENDS := Vfs:1:0
