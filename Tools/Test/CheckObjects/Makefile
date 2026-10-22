# Makefile
#    CheckObjects —— 内核对象与归属登记最小面（T4.0）的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 内核交付实现所在：本工具直接编译这一份 Objects.cpp，不复制实现
CORE_DIR     := ../../../Packages/LikesProgramOS/Core
# 生成契约头与基础类型：Core.Object 槽表类型、ModuleId、KernelError
COMMON_DIR   := ../../../Packages/Common
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract
# 仓库根：契约生成器按仓库相对路径写 Out/Contract，缺失生成头时在这里补跑
REPO_DIR     := ../../..

# C++ 编译器
CXX      ?= g++
# 预处理选项：内核交付头、契约生成头与基础类型
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(COMMON_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckObjects
# 目标文件：main 是用例，Objects 是内核交付实现
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Objects.target.o

vpath %.cpp $(SRC_DIR)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 内核交付实现：与上一条同源编译、对象另起名字；-ffreestanding 与交付构建一致
$(OBJ_DIR)/Objects.target.o: $(CORE_DIR)/$(SRC_DIR)/Objects.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -ffreestanding -MMD -MP -c -o $@ $<

# 契约生成头不提交：缺失时按顶层 Makefile 的 contract-generated 口径先构建生成器并生成一次
$(CONTRACT_DIR)/Slots.hpp:
	@$(MAKE) -C $(REPO_DIR)/Tools/Build/MakeContract
	cd $(REPO_DIR) && Tools/Bin/MakeContract

$(BIN): | $(CONTRACT_DIR)/Slots.hpp

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

# Bin 目录与其它工具共用，clean 只删本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" || { echo "clean: OBJ_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	@test -n "$(BIN)" || { echo "clean: BIN 为空，拒绝执行"; exit 1; }
	rm -f $(BIN)

help:
	@echo "CheckObjects（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
