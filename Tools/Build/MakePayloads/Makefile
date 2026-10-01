# Makefile
#    MakePayloads —— 宿主侧占位载荷生成器（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 与 PackImage / MakeImage / TestInstaller 共享的代码目录
COMMON_DIR := ../Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：占位 Core 的汇编常量与镜像头布局取自 Baleen 的公共头，避免取值在本工具里重写一遍
CPPFLAGS ?= -I$(COMMON_DIR)/Include -I../../../Packages/Baleen/Common/Include -I../../../Packages/Common/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/MakePayloads
# 目标文件：镜像身份填充与摘要算法在 ../Common 共享
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/BaleenImage.o $(OBJ_DIR)/Sha256.o $(OBJ_DIR)/HostIo.o

vpath %.cpp $(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)

.DEFAULT_GOAL := all
.PHONY: all clean help

# 默认目标：构建本工具
all: $(BIN)

# 链接
$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

# 编译源文件
$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
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
	@echo "MakePayloads：make 构建；make clean 清理；$(BIN) <输出目录> 生成明确标记的占位件"

# 头文件依赖
-include $(OBJS:.o=.d)
