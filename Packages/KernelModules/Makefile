# Makefile
#    Packages/KernelModules —— 全部 .osm 模块包的构建入口
#
#    每个含 Module.mk 的子目录是一个模块包；本文件只负责遍历，编译/链接/校验规则统一在
#    Common/ModuleRules.mk，模块自己的变量在各自的 Module.mk
#    产物落 Out/<模块名>/<模块名>.elf，由 PackModule（T1.3）打成 .osm；本包不产出 .osm
#    新增模块：cp -r Hello <新名>，改 Module.mk 的 MODULE_NAME / MODULE_LOGICAL_NAME 与版本，
#    本文件自动纳入；没有模块包时构建成功（空集合合法）
#    纳入顶层构建（TOOL_DIRS / 顶层 Makefile）由 T1.4 做，本包不改顶层 Makefile

# 模块包目录：以 Module.mk 是否存在识别，不靠通配源码
MODULE_DIRS := $(patsubst %/Module.mk,%,$(wildcard */Module.mk))

# 产物根目录：各模块包共用；命令行覆盖时原样传给子 make
OUT_DIR ?= $(CURDIR)/Out

.DEFAULT_GOAL := all
.PHONY: all check package clean help $(MODULE_DIRS)

# 构建全部模块包；-j 时各包并行，互不共享写目标
all: $(MODULE_DIRS)

# 每个模块包跑自己的 Makefile，规则来自同一份 Common/ModuleRules.mk
$(MODULE_DIRS):
	@$(MAKE) -C $@ OUT_DIR="$(OUT_DIR)"

# 校验：先构建全部模块包，再逐个跑包内 check
check: all
	@for dir in $(MODULE_DIRS); do $(MAKE) -C "$$dir" OUT_DIR="$(OUT_DIR)" check || exit 1; done
	@echo "check: 全部模块包通过"

# 打包：把全部模块包打成 .osm，产物在 $(OUT_DIR)/Modules/（系统卷包按该目录取模块）
package: all
	@for dir in $(MODULE_DIRS); do $(MAKE) -C "$$dir" OUT_DIR="$(OUT_DIR)" package || exit 1; done
	@echo "package: 全部模块包已打成 .osm（$(OUT_DIR)/Modules/）"

# 删除产物根目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'Packages/KernelModules：.osm 模块包的构建入口' \
	  '  make [all]  构建 $(MODULE_DIRS) 到 $(OUT_DIR)/<模块名>/<模块名>.elf' \
	  '  make check  逐个模块包复核 ELF 形态、段对齐、重定位与入口符号' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / LD / OBJCOPY 与各 FLAGS 可覆盖'
