# Module.mk
#    Optional 夹具模块包的构建配置：可选运行期模块失败/成功两版注入
#
#    逻辑名参与 ModuleId 派生（SHA-256("LPMOD01" || 逻辑名 ASCII) 前 16 字节），派生值与
#    容器头里的 ModuleId 一致；Optional 不是引导闭包成员，不写进 Modules.list
#    本模块不导出、不导入接口，也没有模块依赖：默认 Init 返回 kCorrupt，用于触发
#    「可选运行期模块失败只 WARN、不重启」路径；成功版用 Makefile.ok 加
#    -DOPTIONAL_INIT_OK 构建，Init 返回 kOk，作为对照证据
#    清单夹具约定（与 baleen-integration 对齐）：清单条目 Type=2、路径
#    Modules/Optional/Optional.1.0.0.osm（Modules/Optional/ 前缀即可选）、不在预装列表；
#    模块容器不自声明必需/可选；必需/可选由发行清单决定

MODULE_NAME := Optional
MODULE_LOGICAL_NAME := Optional
MODULE_MAJOR := 1
MODULE_MINOR := 0
MODULE_PATCH := 0
MODULE_SRCS := Src/Module.cpp
MODULE_ENTRY_DEFAULTS := 1
MODULE_REQUIRED_SECTIONS := .text
CONTRACT_EXPORTS :=
CONTRACT_IMPORTS :=
MODULE_DEPENDS :=
