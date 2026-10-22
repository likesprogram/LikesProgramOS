# Makefile
#    CheckEfiBlockDevice —— Baleen UEFI 块设备适配与固件内存图的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# Baleen UEFI 的目标侧源码目录：本工具直接编译它，检查的就是将要交付的这份源码
UEFI_DIR := ../../../Packages/Baleen/Uefi
# 存储抽象的接口
COMMON_INC := ../../../Packages/Common/Include
# Baleen 引导期公共部件的头文件：诊断编号与 BootInfo 契约，UefiMemory.cpp 直接包含
BALEEN_COMMON_DIR := ../../../Packages/Baleen/Common
# BaleenCore 的预装机制头：UefiMemory 的 UefiAllocator 实现 SpanAllocator，需要它的接口
CORE_DIR := ../../../Packages/Baleen/Core
# 契约层生成物：BootClosure.hpp 与生成的引导闭包期望集头
CONTRACT_DIR := $(COMMON_INC)/../Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：固件侧结构定义、被测类的头文件、共用存储接口、引导期公共头文件与预装机制接口
CPPFLAGS ?= -I$(UEFI_DIR)/Include -I$(COMMON_INC) -I$(BALEEN_COMMON_DIR)/Include -I$(CORE_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckEfiBlockDevice
# 目标文件：main 是用例与假协议，EfiBlockDevice 与 UefiMemory 取自 Baleen UEFI 的目标侧源码
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/EfiBlockDevice.o $(OBJ_DIR)/UefiMemory.o

vpath %.cpp $(SRC_DIR) $(UEFI_DIR)/$(SRC_DIR)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

# Bin 目录与其它工具共用，clean 只删本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" || { echo "clean: OBJ_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	@test -n "$(BIN)" || { echo "clean: BIN 为空，拒绝执行"; exit 1; }
	rm -f $(BIN)

help:
	@echo "CheckEfiBlockDevice（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
