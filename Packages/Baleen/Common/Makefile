# Makefile
#    Packages/Baleen/Common：BIOS 路径引导期控制台、摘要算法与镜像完整性头的目标文件，供 Stub 与 Core 共用

# 产物根目录
OUT_DIR   ?= Out
# 对象目录
OBJ_DIR   := $(OUT_DIR)/Obj
# 源码目录
SRC_DIR   := Src
# 本包头文件目录
INC_DIR   := Include
# 上游共享包头文件目录
COMMON_DIR := ../../Common

# C++ 编译器
CXX       ?= g++
# 编译选项：32 位保护模式、无宿主运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护，警告视为错误
CXXFLAGS  ?= -m32 -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP \
             -I$(INC_DIR) -I$(COMMON_DIR)/Include

# 汇编器与选项：描述符段输出扁平二进制，-I 让 Src/Descs 里的 %INCLUDE 找到本包 Include
NASM      ?= nasm
NASMFLAGS ?= -f bin -Werror -I$(INC_DIR)/

# 源与产物
CPP_OBJS  := $(addprefix $(OBJ_DIR)/,PrintTarget.o VgaTextTarget.o Sha256.o ImageHeader.o)
# 描述符段的产物：头部与各段分别输出，不合并；顺序即段表顺序，位置字段待写入程序回填
DESC_DIR  := $(OUT_DIR)/Descs
DESC_BINS := $(DESC_DIR)/Head.bin $(DESC_DIR)/BootDescriptor.bin $(DESC_DIR)/CoreDescriptor.bin

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(CPP_OBJS) $(DESC_BINS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 描述符段：每个段一个独立产物，位置字段留 0 待写入程序回填
$(DESC_DIR)/%.bin: $(SRC_DIR)/Descs/%.asm $(INC_DIR)/Desc.inc
	@mkdir -p $(DESC_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# 删除产物目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'Packages/Baleen/Common：BIOS 路径引导期控制台、摘要算法、镜像完整性头与描述符段' \
	  '  make [all]  构建 $(CPP_OBJS) 与 $(DESC_BINS)' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / CXXFLAGS / NASM / NASMFLAGS 可覆盖'

-include $(CPP_OBJS:.o=.d)
