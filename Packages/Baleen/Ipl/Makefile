# Makefile
#    Baleen 一级引导：512 字节 MBR 与 2048 字节 El Torito 镜像

# 产物根目录
OUT_DIR   ?= Out
# 汇编清单与配置戳目录
OBJ_DIR   := $(OUT_DIR)/Obj
# 产物目录
BIN_DIR   := $(OUT_DIR)/Bin
# 入口源码目录
SRC_DIR   := Src
# 共享片段目录
INC_DIR   := Include

# 汇编器
NASM      ?= nasm
# 汇编选项：-Ox 多趟优化把短跳转收敛到最短，446/510 字节上限依赖它
NASMFLAGS ?= -f bin -Ox -I$(INC_DIR)/
# 0xE9 调试口开关
ENABLE_E9 ?= 1
# 传给汇编器的定义
DEFS      := -DENABLE_E9=$(ENABLE_E9)

# 两种入口共用的片段
SHARED    := $(addprefix $(INC_DIR)/,Init.inc Body.inc Const.inc Error.inc Read.inc)
# 硬盘 / USB-HDD 形态产物
IPL_HDD   := $(BIN_DIR)/BaleenIPL.bin
# El Torito 光盘形态产物
IPL_CD    := $(BIN_DIR)/BaleenIPLCd.bin
# 汇编选项戳：选项变化时触发重新汇编
CONFIG    := $(OBJ_DIR)/nasm.config

.DEFAULT_GOAL := all
.PHONY: all check list clean help FORCE

# 默认目标：两种形态都构建
all: $(IPL_HDD) $(IPL_CD)

# make 不跟踪命令行变量；只在汇编选项实际改变时更新时间戳
$(CONFIG): FORCE | $(OBJ_DIR)
	@printf '%s\n' '$(NASM) $(NASMFLAGS) $(DEFS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

# 汇编 MBR 形态，同时留一份清单
$(IPL_HDD): $(SRC_DIR)/Mbr.asm $(INC_DIR)/Hdd.inc $(SHARED) $(CONFIG) Makefile | $(BIN_DIR)
	$(NASM) $(NASMFLAGS) $(DEFS) -l $(OBJ_DIR)/BaleenIPL.lst -o $@ $<

# 汇编光盘形态，同时留一份清单
$(IPL_CD): $(SRC_DIR)/Cdrom.asm $(INC_DIR)/Cd.inc $(SHARED) $(CONFIG) Makefile | $(BIN_DIR)
	$(NASM) $(NASMFLAGS) $(DEFS) -l $(OBJ_DIR)/BaleenIPLCd.lst -o $@ $<

# 生成汇编清单
list: $(OBJ_DIR)/BaleenIPL.lst $(OBJ_DIR)/BaleenIPLCd.lst

# MBR 形态的清单
$(OBJ_DIR)/BaleenIPL.lst: $(IPL_HDD)
	$(NASM) $(NASMFLAGS) $(DEFS) -l $@ -o /dev/null $(SRC_DIR)/Mbr.asm

# 光盘形态的清单
$(OBJ_DIR)/BaleenIPLCd.lst: $(IPL_CD)
	$(NASM) $(NASMFLAGS) $(DEFS) -l $@ -o /dev/null $(SRC_DIR)/Cdrom.asm

# 代码区域、保留区与最小余量另由入口源码的汇编期断言保护
# 校验产物尺寸与偏移 510 的引导签名
check: all
	@fail=0; \
	for pair in "$(IPL_HDD):512" "$(IPL_CD):2048"; do \
	  f=$${pair%:*}; want=$${pair##*:}; have=$$(wc -c < "$$f" | tr -d ' '); \
	  [ "$$have" = "$$want" ] || { echo "check: $$f 大小 $$have，应为 $$want"; fail=1; }; \
	  [ "$$(od -An -tx1 -j510 -N2 "$$f" | tr -d ' \n')" = "55aa" ] || \
	    { echo "check: $$f 偏移 510 不是 0xAA55"; fail=1; }; \
	done; \
	[ $$fail -eq 0 ] && echo "check: 产物通过（大小与引导签名）" || exit 1

# 建产物与清单目录
$(BIN_DIR) $(OBJ_DIR):
	@mkdir -p $@

# 删除产物目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rvf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'Baleen 一级引导（IPL）' \
	  '  make [all]  构建 MBR 与光盘形态，生成汇编清单' \
	  '  make check  校验产物尺寸与引导签名' \
	  '  make list   构建汇编清单到 $(OBJ_DIR)/' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：ENABLE_E9=0 关闭调试口；OUT_DIR / NASM 可覆盖'
