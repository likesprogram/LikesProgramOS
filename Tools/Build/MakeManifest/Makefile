# Makefile
#    MakeManifest —— 内核集清单（LPMS）生成工具（C++20）：组装头部、条目与留零的签名区

# 中间对象目录
OBJ_DIR    ?= Obj
# 可执行文件输出目录
BIN_DIR    ?= ../../Bin
# 本工具的源目录
SRC_DIR    := Src
# 构建期工具共享的代码目录（内核镜像数据的识别与 BuildId 文本）
COMMON_DIR := ../Common
# 宿主工具跨分类共享的代码目录（SHA-256 与宿主文件读写）
ROOT_COMMON_DIR := ../../Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：清单与内核镜像数据的字段布局取自 Packages/Common 的单点定义
CPPFLAGS ?= -I$(COMMON_DIR)/Include -I$(ROOT_COMMON_DIR)/Include -I../../../Packages/Common/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/MakeManifest
# 目标文件：内核镜像视图在 ../Common 共享，摘要与宿主文件读写在 ../../Common 共享
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/KernelImage.o $(OBJ_DIR)/Sha256.o $(OBJ_DIR)/HostIo.o
# 头文件依赖清单
DEPS := $(OBJS:.o=.d)

vpath %.cpp $(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR) $(ROOT_COMMON_DIR)/$(SRC_DIR)

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
	@echo "MakeManifest（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(DEPS)
