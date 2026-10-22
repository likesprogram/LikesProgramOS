# Module.mk
#    Ext4 模块包的构建配置：引导闭包成员，M5 起为真实只读实现（逻辑名/版本保持 1.0.0）
#
#    逻辑名与 Packages/Common/Contract/BootClosure.def 的 closure 行逐字一致，参与 ModuleId
#    派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节）；改名等于换一个模块，清单、预装
#    条目与依赖表全部失配
#    实现：Src/Ext4Volume.cpp（只读卷解析）+ Src/Ext4Ops.cpp（FileSystemOps 九槽表）+
#    Src/Module.cpp（入口、导入回填区与登记）；数据结构与界限见 Include/ 下的两个模块内头

# 模块包名与产物主名：Out/Ext4/Ext4.elf 与 Out/Modules/Ext4.1.0.0.osm
MODULE_NAME := Ext4
# 参与 ModuleId 派生的逻辑名：与 BootClosure.def 的成员名逐字一致
MODULE_LOGICAL_NAME := Ext4
# 模块版本 major.minor.patch：major 跟随 .os 发布线且非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配；新增 .cpp 必须加进这里
#   Module.cpp     入口、导入回填区、向 Fs.Vfs 登记
#   Ext4Volume.cpp 只读卷解析（superblock / 组描述符 / inode / extent / 目录项）
#   Ext4Ops.cpp    FileSystemOps 九槽表与模块内静态卷实例
MODULE_SRCS := Src/Module.cpp Src/Ext4Volume.cpp Src/Ext4Ops.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口：模块提供 _Module_Init / _Module_Abort，
# _Module_Activate 用弱默认实现（阶段 3 才调用）
MODULE_ENTRY_DEFAULTS := 1
# 四段齐全：卷解析的常量与格式名在 .rodata，自描述表在 .data（RELATIVE），
# 导入回填区与卷实例（约 9 KiB）在 .bss，便于复核页对齐与段权限
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明（由 MakeContract --module-check 校验后传给 PackModule）
#   导出：无——文件系统实例不导出单例接口，登记由 Vfs 的 Fs.Vfs 持有
#   导入：Fs.Vfs 8 槽（RegisterFileSystem 必需；槽表整表回填由装载器完成）、
#         Core.Log 4 槽（Init 失败的稳定原因写日志）；使用槽数不得超过提供者槽数
CONTRACT_EXPORTS :=
CONTRACT_IMPORTS := Fs.Vfs:1:1:8 Core.Log:1:1:4
# 依赖：Vfs 先装载（提供者先于消费者，装载器按依赖拓扑排序，不依赖预装顺序）
MODULE_DEPENDS := Vfs:1:0
