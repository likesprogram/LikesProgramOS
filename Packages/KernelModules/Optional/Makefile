# Makefile
#    Optional 夹具模块包的独立构建入口：配置在 Module.mk、规则在 Common/ModuleRules.mk
#
#    默认构建失败版（Init 返回 kCorrupt），用于「可选运行期模块失败 WARN 不重启」注入
#    成功版用 Makefile.ok 一键构建（同一源码加 -DOPTIONAL_INIT_OK），见 README 第三节
#    单包构建：make -C Packages/KernelModules/Optional [all|check|package|clean]

include $(CURDIR)/../Common/ModuleRules.mk
