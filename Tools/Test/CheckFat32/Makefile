# Makefile
#    CheckFat32 —— 内核 FAT32File 模块只读实现与 FileSystemOps 九槽表的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 被检查的模块：只读卷实现与 FileSystemOps 表，检查的就是模块将要登记的那两份源码
MODULE_DIR := ../../../Packages/KernelModules/FAT32File
# 模块公共头：最小 C 运行库声明与契约聚合头
KMOD_COMMON_DIR := ../../../Packages/KernelModules/Common
# 公共类型与生成的契约头：FileSystemOps / BlockDeviceOps / Storage::Error 的单点定义
COMMON_DIR := ../../../Packages/Common
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：模块源码目录、公共头与生成契约头
CPPFLAGS ?= -I$(MODULE_DIR)/Include -I$(KMOD_COMMON_DIR) -I$(CONTRACT_DIR) -I$(COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 模块源码的编译选项：与模块包一致加 -ffreestanding（Common/Libc.hpp 的 hidden 声明与宿主内建冲突）
MODULE_CXXFLAGS ?= $(CXXFLAGS) -ffreestanding
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckFat32
# 目标文件：main 是用例与夹具；Fat32Volume/Fat32Ops/Module 取自模块源码；
# Common/Libc.cpp 是模块内最小运行库（符号 hidden），宿主侧同样编进来满足模块源码的引用
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Fat32Volume.o $(OBJ_DIR)/Fat32Ops.o $(OBJ_DIR)/Module.o $(OBJ_DIR)/ModuleLibc.o

vpath %.cpp $(SRC_DIR) $(MODULE_DIR)/$(SRC_DIR) $(KMOD_COMMON_DIR)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

# 模块源码与模块运行库：显式规则，按目标侧条件（-ffreestanding）编译
$(OBJ_DIR)/Fat32Volume.o: $(MODULE_DIR)/$(SRC_DIR)/Fat32Volume.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(MODULE_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Fat32Ops.o: $(MODULE_DIR)/$(SRC_DIR)/Fat32Ops.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(MODULE_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Module.o: $(MODULE_DIR)/$(SRC_DIR)/Module.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(MODULE_CXXFLAGS) -DMODULE_LOGICAL_NAME='"FAT32File"' -DMODULE_MAJOR=1 -DMODULE_MINOR=0 -DMODULE_PATCH=0 -MMD -MP -c -o $@ $<

$(OBJ_DIR)/ModuleLibc.o: $(KMOD_COMMON_DIR)/Libc.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(MODULE_CXXFLAGS) -MMD -MP -c -o $@ $<

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
	@echo "CheckFat32（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
