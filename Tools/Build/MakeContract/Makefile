# Makefile
#    MakeContract —— 模块契约层生成器（C++20）：读 Interfaces.def 与 BootClosure.def 生成契约头与常量表

# 中间对象目录
OBJ_DIR    ?= Obj
# 可执行文件输出目录
BIN_DIR    ?= ../../Bin
# 本工具的源目录
SRC_DIR    := Src
# 宿主工具跨分类共享的代码目录（SHA-256 与宿主文件读写）
ROOT_COMMON_DIR := ../../Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：标识派生共用 Tools/Common 的 SHA-256，文件读取共用 HostIo
CPPFLAGS ?= -I$(ROOT_COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/MakeContract
# 目标文件：解析校验、生成与宿主共享库
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Contract.o $(OBJ_DIR)/Generate.o $(OBJ_DIR)/Sha256.o $(OBJ_DIR)/HostIo.o
# 头文件依赖清单
DEPS := $(OBJS:.o=.d)

vpath %.cpp $(SRC_DIR) $(ROOT_COMMON_DIR)/$(SRC_DIR)

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
	@echo "MakeContract（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(DEPS)
