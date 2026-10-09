# Makefile
#    CheckStorage —— S1 存储抽象的宿主侧检查（C++20）：用例、32 位双编译与链接检查

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# Storage 源码目录：本工具直接编译它，检查的就是将要交付的这份源码
STORAGE_DIR := ../../../Packages/Common/Src/Storage
# Storage 头文件目录
STORAGE_INC := ../../../Packages/Common/Include

# C++ 编译器
CXX      ?= g++
# 预处理选项：存储抽象的接口与共用实现在 Packages/Common
CPPFLAGS ?= -I$(STORAGE_INC)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckStorage
# 目标文件：main 是本工具，其余四个是 Storage 的共用实现
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Error.o $(OBJ_DIR)/Path.o $(OBJ_DIR)/SubDevice.o $(OBJ_DIR)/FileSystem.o

vpath %.cpp $(SRC_DIR) $(STORAGE_DIR)

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
	@echo "CheckStorage（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
