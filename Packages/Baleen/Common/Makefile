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

# 源与产物
CPP_OBJS  := $(addprefix $(OBJ_DIR)/,PrintTarget.o VgaTextTarget.o Sha256.o ImageHeader.o)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(CPP_OBJS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 删除产物目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'Packages/Baleen/Common：BIOS 路径引导期控制台、摘要算法与镜像完整性头' \
	  '  make [all]  构建 $(CPP_OBJS)' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / CXXFLAGS 可覆盖'

-include $(CPP_OBJS:.o=.d)
