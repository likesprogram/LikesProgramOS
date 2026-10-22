# Makefile
#    CheckDevModel —— DevModel 与 DriverLoader 模块实现的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 模块根目录：Common/ 与各模块包所在
KMOD_DIR          := ../../../Packages/KernelModules
# 模块公共头：模块侧最小 C 运行库与契约聚合头
KMOD_COMMON_DIR   := $(KMOD_DIR)/Common
# DevModel 模块源码：PCI 枚举与 Dev.Model 六槽
DEV_MODEL_DIR     := $(KMOD_DIR)/DevModel
# DriverLoader 模块源码：Drv.Loader 四槽
DRV_LOADER_DIR    := $(KMOD_DIR)/DriverLoader
# 共用头与契约生成物
COMMON_PKG_DIR    := ../../../Packages/Common
COMMON_INCLUDE    := $(COMMON_PKG_DIR)/Include
CONTRACT_DIR      := $(COMMON_PKG_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：模块源码目录、模块公共头、公共类型与生成的契约头
CPPFLAGS ?= -I$(SRC_DIR) -I$(DEV_MODEL_DIR)/Src -I$(DRV_LOADER_DIR)/Src \
            -I$(KMOD_COMMON_DIR) -I$(COMMON_INCLUDE) -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckDevModel
# 目标文件：main 是用例与伪配置空间；其余三份是模块交付源码
# PciPorts.cpp 与两个 Module.cpp 不参与：前者是 0xCF8/0xCFC 访问，后者是契约表接线
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Pci.o $(OBJ_DIR)/DeviceModel.o $(OBJ_DIR)/DriverRegistry.o

vpath %.cpp $(SRC_DIR) $(DEV_MODEL_DIR)/Src $(DRV_LOADER_DIR)/Src

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 模块交付源码带 -ffreestanding：模块侧 Libc.hpp 的 hidden 可见性声明与宿主内建函数声明
# 冲突（-Werror=attributes），而模块本来就在 freestanding 语义下构建；主程序不需要
$(OBJ_DIR)/Pci.o: Pci.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -ffreestanding -MMD -MP -c -o $@ $<

$(OBJ_DIR)/DeviceModel.o: DeviceModel.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -ffreestanding -MMD -MP -c -o $@ $<

$(OBJ_DIR)/DriverRegistry.o: DriverRegistry.cpp | $(OBJ_DIR)
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
	@echo "CheckDevModel（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
