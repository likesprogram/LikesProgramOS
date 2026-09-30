# Makefile
#    Baleen Core：BIOS 路径的第二阶段，链接本包与 Common 的目标文件，产出 BaleenCore.bin
#
#    镜像装入在物理 0x100000，由 Stub 按头里的入口偏移跳入；文件起点是入口前缀与
#    128 字节完整性头，0x90 起是 32 位保护模式代码；未初始化区不落盘，
#    由 Stub 在交权前按头里的 MemoryBytes 清零；
#    链接后由 Tools/Bin/PackImage 填头里的 BuildId 与 Digest，并校验头字段

# 产物根目录
OUT_DIR   ?= Out
# 对象目录
OBJ_DIR   := $(OUT_DIR)/Obj
# 产物目录
BIN_DIR   := $(OUT_DIR)/Bin
# 源码与头文件目录
SRC_DIR   := Src
INC_DIR   := Include
# 上游包：Common 提供 Print 与平台原语，Baleen/Common 提供 BIOS 路径控制台、镜像完整性头与交权契约
COMMON_DIR        := ../../Common
BALEEN_COMMON_DIR := ../Common
SHARE_DIR         := $(BALEEN_COMMON_DIR)/Include

# C++ 编译器、汇编器、链接器与镜像转换
CXX       ?= g++
NASM      ?= nasm
LD        ?= ld
OBJCOPY   ?= objcopy

# 编译选项：32 位保护模式、无宿主运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护，警告视为错误
CXXFLAGS  ?= -m32 -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP \
             -I$(INC_DIR) -I$(COMMON_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include
# 汇编选项：32 位保护模式；-I 让 Src 里的 %INCLUDE 找到本包与共享的 Include；警告视为错误
NASMFLAGS ?= -f elf32 -Werror -I$(INC_DIR)/ -I$(SHARE_DIR)/
# 链接选项：无运行库并按 Core.ld 的固定布局；build-id 是额外的落盘节，不要，链接器警告视为错误
LDFLAGS   ?= -m elf_i386 -nostdlib -T Core.ld --build-id=none --fatal-warnings

# 本包目标文件：Core.cpp 是主流程，Console.cpp 是控制台装配，Core_asm.o 与同名 C++ 源以 _asm 后缀区分
LOCAL_OBJS := $(addprefix $(OBJ_DIR)/,Core.o Console.o Core_asm.o)
# 上游目标文件
COMMON_OBJS := $(addprefix $(COMMON_DIR)/$(OBJ_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Cpu.o Cpu_asm.o Descriptor.o Descriptor_asm.o)
BALEEN_COMMON_OBJS := $(addprefix $(BALEEN_COMMON_DIR)/$(OBJ_DIR)/,PrintTarget.o VgaTextTarget.o Sha256.o ImageHeader.o)

# 链接产物与交付镜像
CORE_ELF  := $(BIN_DIR)/BaleenCore.elf
CORE_BIN  := $(BIN_DIR)/BaleenCore.bin
# 宿主侧打包工具：填充头里的 BuildId 与 Digest，并核对头字段与摘要
PACK_IMAGE := ../../../Tools/Bin/PackImage

.DEFAULT_GOAL := all
.PHONY: all upstream check clean help

# 顶层取用的是 BaleenCore.bin
all: $(CORE_BIN)

# 链接：上游对象由各自包的 Makefile 生成，子 make 自己判断是否需要重编
$(CORE_ELF): $(LOCAL_OBJS) $(COMMON_OBJS) $(BALEEN_COMMON_OBJS) Core.ld | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $@ $(LOCAL_OBJS) $(COMMON_OBJS) $(BALEEN_COMMON_OBJS)

# 建产物目录
$(BIN_DIR):
	@mkdir -p $@

$(COMMON_OBJS) $(BALEEN_COMMON_OBJS): upstream

upstream:
	@$(MAKE) -C $(COMMON_DIR)
	@$(MAKE) -C $(BALEEN_COMMON_DIR)

# 转平坦镜像并按头格式打包：未初始化区不落盘，交权前由 Stub 清零
$(CORE_BIN): $(CORE_ELF) $(PACK_IMAGE)
	$(OBJCOPY) -O binary $< $@
	$(PACK_IMAGE) $@

# 打包工具放在 Tools/Bin 随源码交付，缺失时现建一次
$(PACK_IMAGE):
	@$(MAKE) -C ../../../Tools/Build/PackImage

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 汇编产物与同名 C++ 源以 _asm 后缀区分，逐条列出
$(OBJ_DIR)/Core_asm.o: $(SRC_DIR)/Core.asm $(INC_DIR)/Const.inc $(INC_DIR)/CoreHeader.inc $(SHARE_DIR)/Contract.inc
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# 校验镜像非空且不超过 0x400000 上限，头字段与摘要由 PackImage --verify 复核
check: all
	@limit=$$((0x400000)); \
	bytes=$$(wc -c < "$(CORE_BIN)" | tr -d ' '); \
	[ "$$bytes" -gt 0 ] || { echo "check: $(CORE_BIN) 为空"; exit 1; }; \
	[ "$$bytes" -le "$$limit" ] || { echo "check: $(CORE_BIN) 大小 $$bytes，超出 0x400000"; exit 1; }; \
	echo "check: $(CORE_BIN) 大小 $$bytes"; \
	$(PACK_IMAGE) --verify "$(CORE_BIN)"

# 删除产物目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'Baleen Core：BIOS 路径的第二阶段' \
	  '  make [all]  构建 $(CORE_BIN)' \
	  '  make check  校验镜像大小、完整性头字段与摘要' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / NASM / LD / OBJCOPY 与各 FLAGS 可覆盖'

-include $(LOCAL_OBJS:.o=.d)
