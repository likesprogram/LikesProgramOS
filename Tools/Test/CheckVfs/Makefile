# Makefile
#    CheckVfs —— Vfs 模块（Block.Registry / Fs.Vfs / Boot.Volume）的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 模块目标侧源码：块设备层、VFS 与系统卷挂载，检查的就是将要交付的这份实现
KMOD_DIR := ../../../Packages/KernelModules
VFS_DIR  := $(KMOD_DIR)/Vfs
# 模块侧契约聚合头与生成契约头
COMMON_DIR   := ../../../Packages/Common
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract
# 共用存储纯逻辑：统一路径语法与分量切分，与模块编译同一份源码
STORAGE_DIR := $(COMMON_DIR)/$(SRC_DIR)/Storage

# C++ 编译器
CXX      ?= g++
# 预处理选项：模块内部头、模块契约聚合头、生成的契约头与共用存储接口
CPPFLAGS ?= -I$(VFS_DIR)/Include -I$(KMOD_DIR)/Common -I$(CONTRACT_DIR) -I$(COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckVfs
# 目标文件：main 是用例与伪替身；模块源码取自 Vfs/Src，Path 取自共用存储纯逻辑
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Module.o $(OBJ_DIR)/BlockRegistry.o $(OBJ_DIR)/Vfs.o \
        $(OBJ_DIR)/BootVolume.o $(OBJ_DIR)/Path.o

vpath %.cpp $(SRC_DIR) $(VFS_DIR)/$(SRC_DIR) $(STORAGE_DIR)

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
	@echo "CheckVfs（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
