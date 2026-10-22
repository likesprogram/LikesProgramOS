# Module.mk
#    Hello 模块包的构建配置：模块名、版本与骨架开关
#
#    逻辑名参与 ModuleId 派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节），决定清单
#    条目与依赖表；闭包模块的逻辑名必须与 Packages/Common/Contract/BootClosure.def 逐字一致
#    Hello 是骨架样板，不在引导闭包里，逻辑名只用于产物命名与自描述表
#
#    契约声明（CONTRACT_EXPORTS / CONTRACT_IMPORTS）由 MakeContract --module-check 校验，
#    package 时输出 PackModule 的 --export/--import 参数：Hello 演示一条导入（Core.Log 的
#    4 个槽用于日志），不导出接口——导出样例留给真实闭包模块

# 模块包名与产物主名：Out/Hello/Hello.elf
MODULE_NAME := Hello
# 参与 ModuleId 派生的逻辑名；与 MODULE_NAME 分开是为了产物名可独立于契约逻辑名
MODULE_LOGICAL_NAME := Hello
# 模块版本 major.minor.patch：major 跟随 .os 发布线且必须非零（容器 4.2）
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
# 模块源码：显式登记，不靠通配；新增 .cpp 必须加进这里
MODULE_SRCS := Src/Module.cpp
# 链接 Common/ModuleEntry.cpp 的弱符号默认入口，模块只需提供 _Module_Init
MODULE_ENTRY_DEFAULTS := 1
# 骨架要求四段齐全：容器段表允许省略空段，样板保留四段便于复核页对齐与段权限
MODULE_REQUIRED_SECTIONS := .text .rodata .data .bss
# 契约声明：导出项 <稳定名>:<版本>（槽数由接口定义推出），导入项 <稳定名>:<Min>:<Max>:<使用槽数>
# Hello 演示导入：Core.Log v1 的 4 个槽；Module.cpp 里用 LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1)
CONTRACT_EXPORTS :=
CONTRACT_IMPORTS := Core.Log:1:1:4
