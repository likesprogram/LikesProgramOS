# Makefile
#    MakeKeys —— 开发期 Ed25519 密钥生成与信任锚源导出（宿主工具）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 共享源码与头文件目录：Ed25519 与 SHA-512 的宿主侧实现
COMMON_SRC := ../../Common/Src
COMMON_INC := ../../Common/Include

# C++ 编译器
CXX      ?= g++
# 预处理选项：宿主侧密码学实现
CPPFLAGS ?= -I$(COMMON_INC)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/MakeKeys
# 目标文件：main 是本工具，Ed25519 与 SHA-512 取自共享源码，HostIo 提供严格的数字解析
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Ed25519.o $(OBJ_DIR)/Sha512.o $(OBJ_DIR)/HostIo.o

vpath %.cpp $(SRC_DIR) $(COMMON_SRC)

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
	@echo "MakeKeys（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
