# Makefile
#    MakeRevocation —— 内核集签名吊销记录的生成与签名工具（C++20）

# 中间对象目录
OBJ_DIR    ?= Obj
# 可执行文件输出目录
BIN_DIR    ?= ../../Bin
# 本工具的源目录
SRC_DIR    := Src
# 宿主工具跨分类共享的代码目录（Ed25519、SHA-512 与宿主文件读写）
ROOT_COMMON_DIR := ../../Common
# 记录字段布局、结构检查与位图判定的单点定义，与引导侧同源
COMMON_DIR := ../../../Packages/Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：记录布局与判定取自 Packages/Common 的单点定义
CPPFLAGS ?= -I$(ROOT_COMMON_DIR)/Include -I$(COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/MakeRevocation
# 目标文件：Ed25519、SHA-512 与宿主文件读写取自 ../../Common；
# Revocation 取自 Packages/Common 的目标侧源码——结构检查与位图判定是引导侧的同一份实现
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Ed25519.o $(OBJ_DIR)/Sha512.o $(OBJ_DIR)/HostIo.o $(OBJ_DIR)/Revocation.o
# 头文件依赖清单
DEPS := $(OBJS:.o=.d)

vpath %.cpp $(SRC_DIR) $(ROOT_COMMON_DIR)/$(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)

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
	@echo "MakeRevocation（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(DEPS)
