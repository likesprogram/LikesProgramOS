# Makefile
#    Packages/Common：Print 前端、平台原语与存储抽象的目标文件，供 Baleen 各阶段与内核共用
#
#    存储抽象（Storage/）是接口与无设备依赖的纯逻辑：块设备、偏移视图、统一路径与
#    文件系统；两侧的块设备实现与卷解析各自持有，不在这里编译

# 产物根目录
OUT_DIR   ?= Out
# 对象目录
OBJ_DIR   := $(OUT_DIR)/Obj
# 源码目录；Platform 与 Storage 子目录由 vpath 一并为产物取平铺名
SRC_DIR   := Src
# 头文件目录
INC_DIR   := Include

# C++ 编译器
CXX       ?= g++
# 汇编器
NASM      ?= nasm
# 编译选项：32 位保护模式、无宿主运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护，警告视为错误
CXXFLAGS  ?= -m32 -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP \
             -I$(INC_DIR)
# 汇编选项：32 位保护模式目标文件，警告视为错误
NASMFLAGS ?= -f elf32 -Werror

# C++ 源与产物
CPP_OBJS  := $(addprefix $(OBJ_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Cpu.o Descriptor.o)
# 存储抽象的产物：错误模型、统一路径、偏移视图与文件系统抽象
STORAGE_OBJS := $(addprefix $(OBJ_DIR)/,Error.o Path.o SubDevice.o FileSystem.o)
# 汇编源与产物；与同名 C++ 源以 _asm 后缀区分
ASM_OBJS  := $(addprefix $(OBJ_DIR)/,Cpu_asm.o Descriptor_asm.o)
# 全部产物
OBJS      := $(CPP_OBJS) $(STORAGE_OBJS) $(ASM_OBJS)

vpath %.cpp $(SRC_DIR) $(SRC_DIR)/Platform $(SRC_DIR)/Storage

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(OBJS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 汇编产物与同名 C++ 源以 _asm 后缀区分，逐条列出
$(OBJ_DIR)/Cpu_asm.o: $(SRC_DIR)/Platform/Cpu.asm
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

$(OBJ_DIR)/Descriptor_asm.o: $(SRC_DIR)/Platform/Descriptor.asm
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# 删除产物目录
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'Packages/Common：Print 前端、平台原语与存储抽象' \
	  '  make [all]  构建 $(OBJS)' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / NASM / CXXFLAGS / NASMFLAGS 可覆盖'

-include $(CPP_OBJS:.o=.d) $(STORAGE_OBJS:.o=.d)
