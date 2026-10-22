# Makefile
#    CheckPreload —— 引导闭包预装共享机制的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 预装实现所在：BaleenCore 与 UEFI 侧链接的是同一份，检查的就是它
CORE_DIR          := ../../../Packages/Baleen/Core
# Baleen 共用实现：SHA-256 与最小 C 运行库声明
BALEEN_COMMON_DIR := ../../../Packages/Baleen/Common
# 共用存储抽象：文件系统接口与错误名
COMMON_DIR        := ../../../Packages/Common
# 契约层生成物：引导闭包期望集头
CONTRACT_DIR      := $(COMMON_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：目标侧头文件与生成的契约头
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include -I$(COMMON_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckPreload
# 目标文件：main 是用例与替身；BootClosure 与 ScreenFacts 来自 BaleenCore 的目标侧源码，
# SHA-256 与存储抽象（文件系统接口、错误名、统一路径）取自共用包；Libc.cpp 不链接，
# 宿主 C 运行库提供同名符号
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/BootClosure.o $(OBJ_DIR)/ScreenFacts.o \
        $(OBJ_DIR)/Sha256.o $(OBJ_DIR)/FileSystem.o $(OBJ_DIR)/Path.o $(OBJ_DIR)/Error.o

vpath %.cpp $(SRC_DIR) $(CORE_DIR)/Src $(BALEEN_COMMON_DIR)/Src $(COMMON_DIR)/Src $(COMMON_DIR)/Src/Storage

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
	@echo "CheckPreload（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
