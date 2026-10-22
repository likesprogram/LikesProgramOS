# Makefile
#    CheckLoader —— 模块装载编排（11 步）的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 内核实现所在：检查与内核编译的是同一份源码（不复制）
CORE_DIR := ../../../Packages/LikesProgramOS/Core
# 共用契约类型与生成头目录
COMMON_DIR   := ../../../Packages/Common
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract

# C++ 编译器
CXX      ?= g++
# 预处理选项：内核头、共用头与生成的契约头
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(COMMON_DIR)/Include -I$(CONTRACT_DIR)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckLoader
# 目标文件：main 是用例与宿主替身，其余是内核侧的同一份实现
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/ModuleLoader.o $(OBJ_DIR)/ModuleRegistry.o \
        $(OBJ_DIR)/ModuleContainer.o $(OBJ_DIR)/InterfaceRegistry.o $(OBJ_DIR)/DependencyGraph.o \
        $(OBJ_DIR)/Objects.o $(OBJ_DIR)/Memory.o $(OBJ_DIR)/ModuleImage.o $(OBJ_DIR)/ModuleRelocation.o $(OBJ_DIR)/Sha256.o $(OBJ_DIR)/Print.o $(OBJ_DIR)/Tags.o $(OBJ_DIR)/MmioMap.o $(OBJ_DIR)/Paging.o

# 契约生成头是构建输入：缺失时先跑 MakeContract（与顶层 contract-generated 同一工具）
MAKE_CONTRACT := ../../../Tools/Bin/MakeContract
$(MAKE_CONTRACT):
	@$(MAKE) -C ../../../Tools/Build/MakeContract
$(CONTRACT_DIR)/Abi.hpp: $(MAKE_CONTRACT)
	@cd ../../.. && Tools/Bin/MakeContract
$(OBJ_DIR)/main.o $(OBJ_DIR)/ModuleRegistry.o: $(CONTRACT_DIR)/Abi.hpp

vpath %.cpp $(SRC_DIR) $(CORE_DIR)/Src $(COMMON_DIR)/Src

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
	@printf '%s\n' 'CheckLoader：模块装载编排的宿主侧检查' \
	  '  make [all]  构建 $(BIN)' \
	  '  make clean  删除 Obj/ 与 $(BIN)'

-include $(OBJS:.o=.d)
