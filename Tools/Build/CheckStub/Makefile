# Makefile
#    CheckStub —— Stub 阶段的宿主侧回归工具（C++20）：组装镜像、注入故障、在 QEMU 里核对

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 与 PackImage / MakeImage / TestInstaller / MakePayloads 共享的代码目录
COMMON_DIR := ../Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：夹具源码在本工具的 Src 下；探针的汇编常量与镜像头布局取自 Baleen 的公共头，避免取值在本工具里重写一遍
CPPFLAGS ?= -I$(SRC_DIR) -I$(COMMON_DIR)/Include -I../../../Packages/Baleen/Common/Include -I../../../Packages/Common/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckStub
# 目标文件：镜像身份填充与摘要算法在 ../Common 共享
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/BaleenImage.o $(OBJ_DIR)/Sha256.o $(OBJ_DIR)/HostIo.o

vpath %.cpp $(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)

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
	@echo "CheckStub（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
