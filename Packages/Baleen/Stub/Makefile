# Makefile
#    Baleen Stub：低内存固定桩，链接本包与 Common 的目标文件，产出 BaleenStub.bin
#
#    镜像装载在物理 0x7E00，整体必须留在低 64KiB（实模式弹跳按物理地址寻址）；
#    未初始化区不落盘，由入口代码按链接脚本给出的边界清零

# 产物根目录
OUT_DIR   ?= Out
# 对象目录
OBJ_DIR   := $(OUT_DIR)/Obj
# 产物目录
BIN_DIR   := $(OUT_DIR)/Bin
# 源码与头文件目录
SRC_DIR   := Src
INC_DIR   := Include
# 上游包：Common 提供 Print 与平台原语，Baleen/Common 提供 BIOS 路径控制台与交权契约常量
COMMON_DIR        := ../../Common
BALEEN_COMMON_DIR := ../Common
SHARE_DIR         := $(BALEEN_COMMON_DIR)/Include

# C++ 编译器、汇编器、链接器与镜像转换
CXX       ?= g++
NASM      ?= nasm
LD        ?= ld
OBJCOPY   ?= objcopy

# 编译选项：32 位保护模式、无宿主运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护
CXXFLAGS  ?= -m32 -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP \
             -I$(INC_DIR) -I$(COMMON_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include
# 汇编选项：32 位保护模式；-I 让 Src 里的 %INCLUDE 找到本包与共享的 Include
NASMFLAGS ?= -f elf32 -I$(INC_DIR)/ -I$(SHARE_DIR)/
# 链接选项：无运行库并按 Stub.ld 的固定布局；build-id 是额外的落盘节，不要
LDFLAGS   ?= -m elf_i386 -nostdlib -T Stub.ld --build-id=none

# 本包目标文件：LoadCore.cpp 是 Core 装载机制，Stub_asm.o 与同名 C++ 源以 _asm 后缀区分
LOCAL_OBJS := $(addprefix $(OBJ_DIR)/,Stub.o LoadCore.o Stub_asm.o Bios.o)
# 上游目标文件
COMMON_OBJS := $(addprefix $(COMMON_DIR)/$(OBJ_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Cpu.o Cpu_asm.o Descriptor.o Descriptor_asm.o)
BALEEN_COMMON_OBJS := $(addprefix $(BALEEN_COMMON_DIR)/$(OBJ_DIR)/,PrintTarget.o VgaTextTarget.o)

# 链接产物与交付镜像
STUB_ELF  := $(BIN_DIR)/BaleenStub.elf
STUB_BIN  := $(BIN_DIR)/BaleenStub.bin

.DEFAULT_GOAL := all
.PHONY: all upstream check clean help

# 顶层取用的是 BaleenStub.bin
all: $(STUB_BIN)

# 链接：上游对象由各自包的 Makefile 生成，子 make 自己判断是否需要重编
$(STUB_ELF): $(LOCAL_OBJS) $(COMMON_OBJS) $(BALEEN_COMMON_OBJS) Stub.ld | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $@ $(LOCAL_OBJS) $(COMMON_OBJS) $(BALEEN_COMMON_OBJS)

# 建产物目录
$(BIN_DIR):
	@mkdir -p $@

$(COMMON_OBJS) $(BALEEN_COMMON_OBJS): upstream

upstream:
	@$(MAKE) -C $(COMMON_DIR)
	@$(MAKE) -C $(BALEEN_COMMON_DIR)

# 转平坦镜像：未初始化区不落盘，装载后由入口代码清零
$(STUB_BIN): $(STUB_ELF)
	$(OBJCOPY) -O binary $< $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 汇编产物与同名 C++ 源以 _asm 后缀区分，逐条列出
$(OBJ_DIR)/Stub_asm.o: $(SRC_DIR)/Stub.asm $(INC_DIR)/Const.inc $(SHARE_DIR)/Contract.inc
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

$(OBJ_DIR)/Bios.o: $(SRC_DIR)/Bios.asm
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# 校验镜像非空且不越过光盘入口的 0x8000 上限（同一文件用于两种入口）
# 上限与窗口都用 shell 的算术展开写：POSIX 的 test 不认十六进制字面量
check: all
	@limit=$$((0x8000)); window=$$((0x7E00)); \
	bytes=$$(wc -c < "$(STUB_BIN)" | tr -d ' '); \
	[ "$$bytes" -gt 0 ] || { echo "check: $(STUB_BIN) 为空"; exit 1; }; \
	[ "$$bytes" -le "$$limit" ] || { echo "check: $(STUB_BIN) 大小 $$bytes，超出 0x8000"; exit 1; }; \
	echo "check: $(STUB_BIN) 大小 $$bytes，落在 [$$window,0x10000) 窗口内"

# 删除产物目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'Baleen Stub：低内存固定桩' \
	  '  make [all]  构建 $(STUB_BIN)' \
	  '  make check  校验镜像大小落在低内存窗口内' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / NASM / LD / OBJCOPY 与各 FLAGS 可覆盖'

-include $(LOCAL_OBJS:.o=.d)
