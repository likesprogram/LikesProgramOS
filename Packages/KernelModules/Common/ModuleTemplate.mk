# ModuleTemplate.mk
#    模块包 Module.mk 的模板：复制到新模块目录后改变量即可，规则由 Common/ModuleRules.mk 提供
#
#    新建模块包的最小步骤：
#      cp -r Packages/KernelModules/Hello Packages/KernelModules/<新名>
#      改 <新名>/Module.mk 的 MODULE_NAME / MODULE_LOGICAL_NAME / 版本
#      改 <新名>/Src/Module.cpp 的 _Module_Init 实现，删掉 Hello 的自描述表与工作区
#      顶层 make -C Packages/KernelModules 自动发现（以 */Module.mk 是否存在识别）
#    模块包的 Makefile 只有一行 include，不要往里面加规则：
#      include $(CURDIR)/../Common/ModuleRules.mk
#
#    变量约定：
#      MODULE_NAME            模块包名与产物主名，产物落 Out/<MODULE_NAME>/<MODULE_NAME>.elf
#      MODULE_LOGICAL_NAME    参与 ModuleId 派生的逻辑名，闭包模块必须与
#                             Packages/Common/Contract/BootClosure.def 逐字一致
#      MODULE_MAJOR/MINOR/PATCH  模块版本；major 跟随 .os 发布线且必须非零（容器 4.2）
#      MODULE_SRCS            模块源码清单，显式登记，不靠通配
#      MODULE_ENTRY_DEFAULTS  非空时链接 Common/ModuleEntry.cpp 的弱符号默认入口
#      MODULE_REQUIRED_SECTIONS  check 必须存在的段，默认 .text；骨架样板要求四段齐全
#      CONTRACT_EXPORTS       导出项 <稳定名>:<版本>（槽数由接口定义推出，不写槽数）
#      CONTRACT_IMPORTS       导入项 <稳定名>:<MinVersion>:<MaxVersion>:<使用槽数>
#                             两个变量由 package 时的 MakeContract --module-check 校验，
#                             通过后输出 PackModule 的 --export/--import 参数
#
#    导出/导入表的定义（源码里）：
#      LP_CONTRACT_EXPORT(DevModel, 1, DevModelSlotsV1) = { 函数地址, ... };   // 非 const，落可写段
#      LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};                    // 零初始化，落 .bss
#      宏第一个参数是稳定名删点写法：Dev.Model -> DevModel、Core.Log -> CoreLog、Fs.Vfs -> FsVfs
#      （与 PackModule 的 StripDots 一致；这两个宏来自 Common/ModuleAbi.hpp 转发的 Common/Contract.hpp）
#
#    以下为可直接复制的变量块；没有导出的模块把两个 CONTRACT_ 变量留空
#
# MODULE_NAME              := <逻辑名>
# MODULE_LOGICAL_NAME      := <逻辑名>
# MODULE_MAJOR             := 1
# MODULE_MINOR             := 0
# MODULE_PATCH             := 0
# MODULE_SRCS              := Src/Module.cpp
# MODULE_ENTRY_DEFAULTS    := 1
# MODULE_REQUIRED_SECTIONS := .text
# CONTRACT_EXPORTS         :=
# CONTRACT_IMPORTS         := Core.Log:1:1:4
