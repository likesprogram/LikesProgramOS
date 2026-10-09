# Makefile
#    RunBootCase —— 宿主侧模拟器进程与启动标记监测器（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# C++ 编译器
CXX      ?= g++
# 预处理选项
CPPFLAGS ?=
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/RunBootCase
# 目标文件
OBJS := $(OBJ_DIR)/main.o

.DEFAULT_GOAL := all
.PHONY: all clean help

# 默认目标：构建本工具
all: $(BIN)

# 链接
$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

# 编译源文件
$(OBJ_DIR)/main.o: Src/main.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 建输出目录
$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

# 删除对象目录与本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" && test -n "$(BIN)" || { echo "clean: 路径为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	rm -f $(BIN)

# 打印用法
help:
	@echo "RunBootCase：make 构建；$(BIN) --timeout 秒 --marker 标记 --console 文件 --log 文件 --cwd 目录 -- 命令"

# 头文件依赖
-include $(OBJS:.o=.d)
