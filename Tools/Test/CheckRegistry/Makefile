# Makefile
#    CheckRegistry —— 接口注册表的宿主侧检查（C++20）：导出登记、提供者唯一、版本选择、槽位回填与 ABI 版本

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 注册表实现所在：内核与检查工具编译的是同一份源码
CORE_DIR := ../../../Packages/LikesProgramOS/Core
# 共用契约类型与生成头目录
COMMON_DIR   := ../../../Packages/Common
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：内核侧注册表头、共用契约类型与生成的契约头
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(COMMON_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckRegistry
# 目标文件：main 是用例与替身，InterfaceRegistry 是内核注册表的同一份实现
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/InterfaceRegistry.o

# 契约生成头是构建输入：缺失时先跑 MakeContract（与顶层 contract-generated 同一工具）
MAKE_CONTRACT := ../../../Tools/Bin/MakeContract
$(MAKE_CONTRACT):
	@$(MAKE) -C ../../../Tools/Build/MakeContract
$(CONTRACT_DIR)/Abi.hpp: $(MAKE_CONTRACT)
	@cd ../../.. && Tools/Bin/MakeContract
$(OBJ_DIR)/InterfaceRegistry.o: $(CONTRACT_DIR)/Abi.hpp
$(OBJ_DIR)/main.o: $(CONTRACT_DIR)/Abi.hpp

vpath %.cpp $(SRC_DIR) $(CORE_DIR)/Src

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
	@echo "CheckRegistry（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
