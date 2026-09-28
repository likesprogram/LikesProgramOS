# Makefile
#    CheckIpl —— IPL 静态与启动回归校验工具（C++20）；默认仅编译，运行验证需显式调用 Tools/Bin/CheckIpl

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# C++ 编译器
CXX      ?= g++
# 预处理选项
CPPFLAGS ?=
# 编译选项
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
# 链接选项
LDFLAGS  ?=

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckIpl
# 目标文件
OBJS := $(OBJ_DIR)/main.o

.DEFAULT_GOAL := all
.PHONY: all clean help

# 默认目标：构建本工具
all: $(BIN)

# 链接
$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

# 编译源文件（含夹具头）
$(OBJ_DIR)/main.o: Src/main.cpp Src/Fixtures.h | $(OBJ_DIR)
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
	@echo "CheckIpl：make 仅构建；使用 --layout-only <IplOut> 或 --ipl-dir <IplOut/Bin> 手工执行验证"

# 头文件依赖
-include $(OBJS:.o=.d)
