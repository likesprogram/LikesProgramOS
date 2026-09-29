# Makefile
#    PackImage —— 宿主侧 Baleen 镜像打包工具（C++20）：填完整性头里的 BuildId 与 Digest 并校验

# 中间对象目录
OBJ_DIR    ?= Obj
# 可执行文件输出目录
BIN_DIR    ?= ../../Bin
# 本工具的源目录
SRC_DIR    := Src
# 与 MakeIso / MakeHdd 共享的代码目录
COMMON_DIR := ../Common

# C++ 编译器
CXX      ?= g++
# 预处理选项
CPPFLAGS ?= -I$(COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/PackImage
# 目标文件：镜像视图与摘要算法在 ../Common 共享，宿主文件读写在同处
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/BaleenImage.o $(OBJ_DIR)/Sha256.o $(OBJ_DIR)/HostIo.o
# 头文件依赖清单
DEPS := $(OBJS:.o=.d)

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

# Bin 目录与 MakeIso / MakeHdd 共用，clean 只删本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" || { echo "clean: OBJ_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	@test -n "$(BIN)" || { echo "clean: BIN 为空，拒绝执行"; exit 1; }
	rm -f $(BIN)

help:
	@echo "PackImage（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(DEPS)
