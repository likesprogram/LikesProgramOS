# Module.mk
#    Fault 夹具模块包的构建配置：必需运行期模块 Init 失败注入
#
#    逻辑名参与 ModuleId 派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节），派生值与
#    容器头里的 ModuleId 一致；Fault 不是引导闭包成员，不写进 Modules.list
#    本模块不导出、不导入接口，也没有模块依赖：Init 只核对 KernelAbiContext 后返回
#    kCorrupt，用来触发「必需运行期模块 Init 失败」路径，不依赖任何契约槽位
#    清单夹具约定（与 baleen-integration 对齐）：清单条目 Type=2、路径
#    Modules/Fault.1.0.0.osm、不在预装列表；模块容器不自声明必需/可选，
#    「无 Modules/Optional/ 前缀」即必需

MODULE_NAME := Fault
MODULE_LOGICAL_NAME := Fault
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
MODULE_SRCS := Src/Module.cpp
MODULE_ENTRY_DEFAULTS := 1
MODULE_REQUIRED_SECTIONS := .text
CONTRACT_EXPORTS :=
CONTRACT_IMPORTS :=
MODULE_DEPENDS :=
