# Makefile
# MakeIso —— 宿主侧 ISO 组装器（C++20）

# 中间对象目录
OBJ_DIR    ?= Obj
# 可执行文件输出目录
BIN_DIR    ?= ../../Bin
# 本工具的源目录
SRC_DIR    := Src
# 本工具的头文件目录
INC_DIR    := Include
# 与 MakeHdd 共享的代码目录
COMMON_DIR := ../Common

# C++ 编译器
CXX      ?= g++
# 预处理选项
CPPFLAGS ?= -I$(INC_DIR) -I$(COMMON_DIR)/Include
# 编译选项
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
# 链接选项
LDFLAGS  ?=

# 本工具的可执行文件
BIN  := $(BIN_DIR)/MakeIso
# 目标文件
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Iso9660.o $(OBJ_DIR)/Image.o $(OBJ_DIR)/Baleen.o $(OBJ_DIR)/HostIo.o $(OBJ_DIR)/Fat.o
# 头文件依赖清单
DEPS := $(OBJS:.o=.d)

# 镜像封装、引导结构、宿主文件工具与 FAT 校验在 ../Common 共享
vpath %.cpp $(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)

.DEFAULT_GOAL := all
.PHONY: all clean help

# 默认目标：构建本工具
all: $(BIN)

# 链接
$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

# 编译单个源文件
$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 建输出目录
$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

# 删除对象目录与本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" || { echo "clean: OBJ_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	@test -n "$(BIN)" || { echo "clean: BIN 为空，拒绝执行"; exit 1; }
	rm -f $(BIN)

# 打印用法
help:
	@echo "MakeIso（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

# 头文件依赖
-include $(DEPS)
