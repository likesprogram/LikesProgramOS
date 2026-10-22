# Makefile
#    CheckMemory —— 引导期内存图可用性判定、弹跳窗口搜索与内核页帧预留的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 判定实现所在：Stub 与 BaleenCore 链接的是同一份，检查的就是它
BALEEN_COMMON_SRC := ../../../Packages/Baleen/Common/Src
# 判定与内存图条目的头文件目录
BALEEN_COMMON_INC := ../../../Packages/Baleen/Common/Include
COMMON_INC        := ../../../Packages/Common/Include
# 内核页帧分配器的交付源码与头文件目录：本工具直接编译这一份，不复制实现
KERNEL_SRC := ../../../Packages/LikesProgramOS/Core/Src
KERNEL_INC := ../../../Packages/LikesProgramOS/Core/Include
# 宿主侧替身头目录：Paging.hpp 的直映射在宿主上按恒等换算，见 Include/Paging.hpp
SHIM_INC   := Include

# C++ 编译器
CXX      ?= g++
# 预处理选项：替身头必须排在内核头之前，否则 Memory.cpp 会取到真正的 Paging.hpp
CPPFLAGS ?= -I$(SHIM_INC) -I$(KERNEL_INC) -I$(BALEEN_COMMON_INC) -I$(COMMON_INC)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckMemory
# 目标文件：main 是用例，BootMemory 取自 Baleen 共用源码，Memory 与 Libc 取自内核交付实现
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/BootMemory.o $(OBJ_DIR)/Memory.target.o $(OBJ_DIR)/Libc.target.o

vpath %.cpp $(SRC_DIR) $(BALEEN_COMMON_SRC)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 内核交付实现：与上一条同源编译，对象另起名字避免与 Baleen 侧同名源撞车
# -ffreestanding 与交付构建一致，同时防止 GCC 把 Libc 的循环识别成对同名函数的调用（宿主模式
# 下 -ftree-loop-distribute-patterns 会生成自递归的 memset），交付构建本来就用 -ffreestanding
$(OBJ_DIR)/Memory.target.o: $(KERNEL_SRC)/Memory.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -ffreestanding -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Libc.target.o: $(KERNEL_SRC)/Libc.cpp | $(OBJ_DIR)
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
	@echo "CheckMemory（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
