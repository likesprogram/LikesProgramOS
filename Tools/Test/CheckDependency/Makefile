# Makefile
#    CheckDependency —— 模块依赖表校验、版本区间匹配与稳定拓扑排序的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 内核目标侧源码：依赖图实现，检查的就是将要交付的这份源码
CORE_DIR := ../../../Packages/LikesProgramOS/Core
# 共用头目录（本工具只用到生成物的包含路径约定）
COMMON_DIR := ../../../Packages/Common
# 契约层生成物：ModuleId 常量由 MakeContract 生成，随 contract-generated 目标产出
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：内核目标侧头文件与生成的契约头
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(COMMON_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckDependency
# 目标文件：main 是用例，DependencyGraph 取自内核目标侧源码；无运行库依赖由宿主 C 运行库满足
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/DependencyGraph.o

vpath %.cpp $(SRC_DIR) $(CORE_DIR)/$(SRC_DIR)

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
	@echo "CheckDependency（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
