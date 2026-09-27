# Baleen 一级引导（IPL）构建
#
# 产物：
#   Out/Bin/BaleenIPL.bin    硬盘 / USB-HDD 形态：BIOS 装入 0x7C00，恰好 512 字节
#   Out/Bin/BaleenIPLCd.bin  El Torito 无仿真光盘形态：引导镜像恰好 2048 字节
#
# 本目录只产出一级引导。BootDescriptor、BaleenLayout.bin 与介质镜像由写盘 /
# 镜像组装工具生成，见 Docs/Specs/Baleen引导器.md 第五、八节。
# 约定：在本目录内执行（或 make -C Packages/Baleen/Ipl）。

OUT_DIR   ?= Out
OBJ_DIR   := $(OUT_DIR)/Obj
BIN_DIR   := $(OUT_DIR)/Bin
SRC_DIR   := Src
INC_DIR   := Include

NASM      ?= nasm
# -Ox 多趟优化把短跳转收敛到最短：Mbr 的 446 字节与 Cdrom 的 510 字节上限
# 依赖它，实测 -O0 / -O1 会把其中一个撑过上限，由汇编期 %ERROR 拦下。
NASMFLAGS ?= -f bin -Ox -I$(INC_DIR)/
# 0xE9 调试口开关，Body.inc 默认开：make ENABLE_E9=0 关闭
ENABLE_E9 ?= 1
DEFS      := -DENABLE_E9=$(ENABLE_E9)

INCS      := $(INC_DIR)/Body.inc $(INC_DIR)/Const.inc $(INC_DIR)/Read.inc
IPL_HDD   := $(BIN_DIR)/BaleenIPL.bin
IPL_CD    := $(BIN_DIR)/BaleenIPLCd.bin

.DEFAULT_GOAL := all
.PHONY: all check list clean help

all: $(IPL_HDD) $(IPL_CD)

$(IPL_HDD): $(SRC_DIR)/Mbr.asm $(INCS) | $(BIN_DIR)
	@$(NASM) $(NASMFLAGS) $(DEFS) -o $@ $<

$(IPL_CD): $(SRC_DIR)/Cdrom.asm $(INCS) | $(BIN_DIR)
	@$(NASM) $(NASMFLAGS) $(DEFS) -o $@ $<

# NASM 汇编清单，供 16 位代码体积与跳转形态排查
list: $(OBJ_DIR)/BaleenIPL.lst $(OBJ_DIR)/BaleenIPLCd.lst

$(OBJ_DIR)/BaleenIPL.lst: $(SRC_DIR)/Mbr.asm $(INCS) | $(OBJ_DIR)
	@$(NASM) $(NASMFLAGS) $(DEFS) -l $@ -o /dev/null $<

$(OBJ_DIR)/BaleenIPLCd.lst: $(SRC_DIR)/Cdrom.asm $(INCS) | $(OBJ_DIR)
	@$(NASM) $(NASMFLAGS) $(DEFS) -l $@ -o /dev/null $<

# 引导扇区的两个静默失败点：长度不对、偏移 510 不是 0xAA55
check: all
	@fail=0; \
	for pair in "$(IPL_HDD):512" "$(IPL_CD):2048"; do \
	  f=$${pair%:*}; want=$${pair##*:}; have=$$(wc -c < "$$f" | tr -d ' '); \
	  [ "$$have" = "$$want" ] || { echo "check: $$f 大小 $$have，应为 $$want"; fail=1; }; \
	  [ "$$(od -An -tx1 -j510 -N2 "$$f" | tr -d ' \n')" = "55aa" ] || \
	    { echo "check: $$f 偏移 510 不是 0xAA55"; fail=1; }; \
	done; \
	[ $$fail -eq 0 ] && echo "check: 产物通过（大小与引导签名）" || \
	  { echo "check: 失败"; exit 1; }

$(BIN_DIR) $(OBJ_DIR):
	@mkdir -p $@

clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rvf $(OUT_DIR)

help:
	@echo "Baleen 一级引导（IPL）"
	@echo "  make [all]        构建全部产物（默认）"
	@echo "  make check        构建并校验产物大小与 0xAA55 签名"
	@echo "  make list         生成汇编清单到 $(OBJ_DIR)/"
	@echo "  make clean        删除 $(OUT_DIR)/"
	@echo "  变量：ENABLE_E9=0 关闭 0xE9 调试口；OUT_DIR / NASM 可覆盖"
