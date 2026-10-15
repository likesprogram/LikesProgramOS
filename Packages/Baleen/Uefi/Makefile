# Makefile
#    Packages/Baleen/Uefi —— IA32 侧的不支持提示件：产出 PE32 EFI 应用 BOOTIA32.EFI
#
#    为什么只做这一件：内核入口是长模式、UEFI 侧引导器是 x64 固件应用，32 位固件上没有任何
#    可引导的东西；这一件只把"为什么起不来"打到固件控制台，见 Src/BootIa32.cpp
#    x64 侧的固件应用按 Docs/Specs/Baleen/README.md 3.1 的实现次序随后实施
#
#    EFI 应用是 PE32（IA32）形态、子系统 10：先按 32 位保护模式链成 ELF，再用 objcopy 转 PE32

# 产物根目录
OUT_DIR   ?= Out
# 对象目录与产物目录
OBJ_DIR   := $(OUT_DIR)/Obj
BIN_DIR   := $(OUT_DIR)/Bin
# 源码目录
SRC_DIR   := Src

# C++ 编译器与链接器
CXX       ?= g++
LD        ?= ld
# 编译选项：32 位、无运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护，警告视为错误
# -fno-ident 去掉 .comment：它落在映像基址之下，PE 链接会为它报 "section below image base"
CXXFLAGS  ?= -m32 -std=c++20 -Os -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident -Wall -Wextra -Werror
# 链接选项：直接链成 PE32 的 EFI 应用（-m i386pe）。三项是装配侧实测出来的硬要求：
#   --image-base 非零：PE 头里 ImageBase 为 0 的镜像会被固件判为不受支持
#   --enable-reloc-section：生成 .reloc；固件把映像装到别处时要靠它重定位，缺了同样加载失败
#   --subsystem 10：EFI 应用
LDFLAGS   ?= -m i386pe --image-base 0x400000 --enable-reloc-section --subsystem 10 -nostdlib -e efi_main

# 交付产物：ESP 的 \EFI\BOOT\BOOTIA32.EFI
EFI32     := $(BIN_DIR)/BOOTIA32.EFI
# 中间产物
OBJ       := $(OBJ_DIR)/BootIa32.o

.DEFAULT_GOAL := all
.PHONY: all check clean help

all: $(EFI32)

$(OBJ): $(SRC_DIR)/BootIa32.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 直接链成 PE32 的 EFI 应用
$(EFI32): $(OBJ) | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $@ $<

$(OBJ_DIR) $(BIN_DIR):
	@mkdir -p $@

# 复核产物形态：PE32 的 EFI 应用、基址非零、带重定位段、提示文本在位
check: all
	@objdump -f "$(EFI32)" | grep -q "pei-i386" || { echo "check: $(EFI32) 不是 PE32（pei-i386）"; exit 1; }
	@objdump -p "$(EFI32)" | grep -qE "ImageBase[[:space:]]+00400000" || { echo "check: $(EFI32) 的 ImageBase 不是 00400000（固件会判为不受支持）"; exit 1; }
	@objdump -h "$(EFI32)" | grep -q "\.reloc" || { echo "check: $(EFI32) 缺 .reloc 段（固件重定位会失败）"; exit 1; }
	@strings -el "$(EFI32)" | grep -q "32-bit UEFI (IA32)" || { echo "check: $(EFI32) 里没有提示文本"; exit 1; }
	@strings -el "$(EFI32)" | grep -q "E-UEFI-11" || { echo "check: $(EFI32) 里没有登记编号 E-UEFI-11"; exit 1; }
	@echo "check: $(EFI32) 是 PE32 EFI 应用（ImageBase 00400000、带 .reloc），提示文本在位（$$(wc -c < "$(EFI32)") 字节）"

clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

help:
	@printf '%s\n' 'Packages/Baleen/Uefi：IA32 侧的不支持提示件' \
	  '  make [all]  构建 $(EFI32)' \
	  '  make check  复核产物是 PE32 EFI 应用且提示文本在位' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / LD / OBJCOPY / CXXFLAGS / LDFLAGS 可覆盖'
