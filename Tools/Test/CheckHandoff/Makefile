# Makefile
#    CheckHandoff —— 内核交接包 9 步核对的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 内核目标侧源码：交接包核对与清单副本解析，检查的就是将要交付的这份实现
CORE_DIR := ../../../Packages/LikesProgramOS/Core
# 共用实现：内核镜像数据校验（ReadU16 / ReadU32）、清单字节表与 SHA-256（路径派生模块标识）
COMMON_DIR := ../../../Packages/Common
# 契约层生成物：引导闭包期望集头与 ModuleId 常量
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：内核目标侧头文件、共用头与生成的契约头
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(COMMON_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckHandoff
# 目标文件：main 是用例与宿主替身；Handoff 与 ManifestView 取自 Core 的目标侧源码；
# KernelImage 与 SHA-256 取自 Common 的共用源码；Libc.cpp 不链接，宿主 C 运行库提供 memcmp / memcpy
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Handoff.o $(OBJ_DIR)/ManifestView.o $(OBJ_DIR)/KernelImage.o $(OBJ_DIR)/Sha256.o

vpath %.cpp $(SRC_DIR) $(CORE_DIR)/$(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)

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
	@echo "CheckHandoff（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
