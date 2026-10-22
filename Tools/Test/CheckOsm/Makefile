# Makefile
#    CheckOsm —— 内核模块容器（.osm）的宿主侧检查（C++20）：正例、逐字段负例与确定性

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# PackModule：容器写出器与解析校验器在这里，头文件与源文件都按相对路径引用，不复制实现
PACK_DIR := ../../Build/PackModule
# 宿主工具跨分类共享的代码目录（SHA-256）
COMMON_DIR := ../../Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：PackModule 的容器头文件与宿主共享头
CPPFLAGS ?= -I$(PACK_DIR)/Include -I$(COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckOsm
# 目标文件：用例与 PackModule 的容器实现同源编译
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Osm.o $(OBJ_DIR)/Sha256.o
# 头文件依赖清单
DEPS := $(OBJS:.o=.d)

vpath %.cpp $(SRC_DIR) $(PACK_DIR)/$(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)

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
	@echo "CheckOsm（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(DEPS)
