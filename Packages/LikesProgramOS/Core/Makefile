# Makefile
#    LikesProgramOS/Core —— 内核侧第一阶段：内核映像 LikesProgram.os 与内核镜像数据
#
#    链接本包与 Common 的 64 位目标文件，产出扁平镜像后由 Tools/Bin/PackImage
#    填数据里的 BuildId 与 Digest 并复核；映像装入物理 0x1000000，数据排在文件起点，
#    0x80 起是长模式入口；未初始化区不落盘，由引导器在交权前按数据里的 MemoryBytes 清零

# 产物根目录
OUT_DIR   ?= Out
# 对象目录：本包源码与共享源码分开，后者按内核的编译选项重编一份
OBJ_DIR        := $(OUT_DIR)/Obj
OBJ_COMMON_DIR := $(OUT_DIR)/ObjCommon
# 产物目录
BIN_DIR   := $(OUT_DIR)/Bin
# 源码与头文件目录
SRC_DIR   := Src
INC_DIR   := Include
# 上游包：Common 提供 Print 前端、串口与调试口目标、描述符表机制与内核镜像数据契约
COMMON_DIR     := ../../Common
COMMON_SRC_DIR := $(COMMON_DIR)/Src

# C++ 编译器、汇编器、链接器与镜像转换
CXX       ?= g++
NASM      ?= nasm
LD        ?= ld
OBJCOPY   ?= objcopy

# 编译选项：64 位长模式、无宿主运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护、不要红区，警告视为错误
CXXFLAGS  ?= -m64 -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP \
             -I$(INC_DIR) -I$(COMMON_DIR)/Include
# 汇编选项：64 位长模式目标文件；-I 让 Src 里的 %INCLUDE 找到本包 Include；警告视为错误
NASMFLAGS ?= -f elf64 -Werror -I$(INC_DIR)/
# 链接选项：无运行库并按 Core.ld 的固定布局；build-id 是额外的落盘节，不要，警告视为错误
LDFLAGS   ?= -m elf_x86_64 -nostdlib -T Core.ld --build-id=none --fatal-warnings

# 本包目标文件：Core.cpp 是主流程，Console 装配早期控制台，Tags 写诊断标签，
# Cpu/Tables/Paging/Memory 是平台初始化与内存层，Core_asm.o 与同名 C++ 源以 _asm 后缀区分
LOCAL_OBJS := $(addprefix $(OBJ_DIR)/,Core.o Console.o Tags.o Cpu.o Tables.o Paging.o Memory.o Libc.o Core_asm.o)
# 共享目标文件：Print 前端、串口与调试口目标、描述符表机制、内核镜像数据校验
COMMON_OBJS := $(addprefix $(OBJ_COMMON_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Descriptor.o Descriptor_asm.o KernelImage.o)

# 链接产物与交付映像
KERNEL_ELF := $(OUT_DIR)/LikesProgram.elf
KERNEL_BIN := $(BIN_DIR)/LikesProgram.os
# 宿主侧打包工具：填充数据里的 BuildId 与 Digest，并核对字段与摘要
PACK_IMAGE := ../../../Tools/Bin/PackImage

.DEFAULT_GOAL := all
.PHONY: all check clean help

# 顶层与系统卷组装取用的是 LikesProgram.os
all: $(KERNEL_BIN)

# 链接：共享源码由本 Makefile 以 64 位选项编译，不引用 Common 包的 32 位对象
$(KERNEL_ELF): $(LOCAL_OBJS) $(COMMON_OBJS) Core.ld
	$(LD) $(LDFLAGS) -o $@ $(LOCAL_OBJS) $(COMMON_OBJS)

# 建产物目录
$(BIN_DIR):
	@mkdir -p $@

# 转扁平镜像并按内核镜像数据打包：未初始化区不落盘
$(KERNEL_BIN): $(KERNEL_ELF) $(PACK_IMAGE) | $(BIN_DIR)
	$(OBJCOPY) -O binary $< $@
	$(PACK_IMAGE) $@

# 打包工具放在 Tools/Bin 随源码交付，缺失时现建一次
$(PACK_IMAGE):
	@$(MAKE) -C ../../../Tools/Build/PackImage

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 汇编产物与同名 C++ 源以 _asm 后缀区分，逐条列出
$(OBJ_DIR)/Core_asm.o: $(SRC_DIR)/Core.asm $(INC_DIR)/Const.inc
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# 共享源码按内核的编译选项重编一份；vpath 让 Platform 子目录里的源也取平铺名
vpath %.cpp $(COMMON_SRC_DIR) $(COMMON_SRC_DIR)/Platform

$(OBJ_COMMON_DIR)/%.o: %.cpp
	@mkdir -p $(OBJ_COMMON_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_COMMON_DIR)/Descriptor_asm.o: $(COMMON_SRC_DIR)/Platform/Descriptor.asm
	@mkdir -p $(OBJ_COMMON_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# 校验镜像大小、入口偏移、ELF 依赖与内核镜像数据：字段与摘要由 PackImage --verify 复核
check: all
	@limit=$$((0x800000)); \
	bytes=$$(wc -c < "$(KERNEL_BIN)" | tr -d ' '); \
	[ "$$bytes" -gt 0 ] || { echo "check: $(KERNEL_BIN) 为空"; exit 1; }; \
	[ "$$bytes" -le "$$limit" ] || { echo "check: $(KERNEL_BIN) 大小 $$bytes，超出 0x800000"; exit 1; }; \
	entry=$$(od -An -tu4 -j 24 -N 4 "$(KERNEL_BIN)" | tr -d ' '); \
	[ "$$entry" = "128" ] || { echo "check: 数据里的入口偏移 $$entry 不是约定的 128"; exit 1; }; \
	echo "check: $(KERNEL_BIN) 大小 $$bytes，入口偏移 128"; \
	$(PACK_IMAGE) --verify "$(KERNEL_BIN)"
	@if readelf -S "$(KERNEL_ELF)" | grep -q '\.init_array'; then echo "check: ELF 含 .init_array，静态构造依赖未定义"; exit 1; fi
	@if readelf -d "$(KERNEL_ELF)" 2>/dev/null | grep -q NEEDED; then echo "check: ELF 含动态依赖"; exit 1; fi

# 删除产物目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'LikesProgramOS/Core：内核侧第一阶段' \
	  '  make [all]  构建 $(KERNEL_BIN)' \
	  '  make check  校验镜像大小、入口偏移、ELF 依赖与内核镜像数据' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / NASM / LD / OBJCOPY 与各 FLAGS 可覆盖'

-include $(LOCAL_OBJS:.o=.d) $(COMMON_OBJS:.o=.d)
