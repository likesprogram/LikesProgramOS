# Makefile
#    CheckScreen —— 内核自绘屏幕目标与 8×16 字模的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 内核目标侧源码：屏幕目标与内嵌字模，检查的就是将要交付的这份实现
CORE_DIR := ../../../Packages/LikesProgramOS/Core
# 共用头：BootInfo 屏幕事实与 Print 目标接口
COMMON_DIR := ../../../Packages/Common

# C++ 编译器
CXX      ?= g++
# 预处理选项：内核目标侧头文件与共用头
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(COMMON_DIR)/Include
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckScreen
# 目标文件：main 是用例与宿主替身；ScreenTarget 与 Font 取自 Core 的目标侧源码
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/ScreenTarget.o $(OBJ_DIR)/Font.o

vpath %.cpp $(SRC_DIR) $(CORE_DIR)/$(SRC_DIR)

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
	@echo "CheckScreen（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
