# Makefile
#    CheckSelect —— BaleenCore 选版与引导状态记录的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# BaleenCore 的目标侧源码：选版与状态记录，检查的就是将要交付的这两份
CORE_DIR := ../../../Packages/Baleen/Core
# 存储抽象的接口与共用实现
COMMON_DIR := ../../../Packages/Common
# Baleen 共用头：诊断编号与打印目标标签
BALEEN_COMMON_DIR := ../../../Packages/Baleen/Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：目标侧头文件与共用存储接口
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(COMMON_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckSelect
# 目标文件：main 是用例与替身，SelectSet 取自 BaleenCore，BootState 与 Revocation 取自共用包；
# FileSystem 的共用接线与错误名取自存储抽象，Libc.cpp 不链接（宿主 C 运行库提供同名符号）
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/SelectSet.o $(OBJ_DIR)/BootState.o $(OBJ_DIR)/Revocation.o \
        $(OBJ_DIR)/Error.o $(OBJ_DIR)/Path.o $(OBJ_DIR)/FileSystem.o

vpath %.cpp $(SRC_DIR) $(CORE_DIR)/$(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)/Storage

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
	@echo "CheckSelect（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
