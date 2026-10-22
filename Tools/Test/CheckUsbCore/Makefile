# Makefile
#    CheckUsbCore —— Usb.Host 核心实现（UsbCore.cpp）的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# UsbCore 模块源码：直接编译交付实现，不复制
CORE_DIR     := ../../../Packages/KernelModules/UsbCore
# 生成契约头与基础类型
COMMON_DIR   := ../../../Packages/Common
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：模块头、契约生成头与基础类型
CPPFLAGS ?= -I$(CORE_DIR)/$(SRC_DIR) -I$(COMMON_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckUsbCore
# 目标文件：用例与交付实现同源编译
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/UsbCore.o

vpath %.cpp $(SRC_DIR)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 交付实现：与上一条同源编译，对象另起名字
$(OBJ_DIR)/UsbCore.o: $(CORE_DIR)/$(SRC_DIR)/UsbCore.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -ffreestanding -MMD -MP -c -o $@ $<

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

# Bin 目录与其它工具共用，clean 只删本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" || { echo "clean: OBJ_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	@test -n "$(BIN)" || { echo "clean: BIN 为空，拒绝执行"; exit 1; }
	rm -f $(BIN)

help:
	@echo "CheckUsbCore（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
