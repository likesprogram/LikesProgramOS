# Makefile
#    Packages/Common：Print 前端、平台原语、存储抽象与引导状态记录的目标文件，供 Baleen 各阶段与内核共用
#
#    存储抽象（Storage/）是接口与无设备依赖的纯逻辑：块设备、偏移视图、统一路径与
#    文件系统；两侧的块设备实现与卷解析各自持有，不在这里编译
#    引导状态记录（BootState）是引导期写入方与运行期确认方共用的同一份读写协议：
#    放在这里是为了让"同一后端、同一格式"成为一份实现，而不是两份约定
#    吊销记录（Revocation）同理是两侧共用的字节表、签名规则与判定；验签实现由调用方链入

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
# 优化级别取 -Os：Baleen 的 32 位阶段在低 64KiB 静态窗口里运行，代码体积优先于指令数；
# 宿主工具与 64 位内核各自按 -O2 编译，不受这里影响
CXXFLAGS  ?= -m32 -std=c++20 -Os -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP \
             -I$(INC_DIR)
# 汇编选项：32 位保护模式目标文件，警告视为错误
NASMFLAGS ?= -f elf32 -Werror

# C++ 源与产物
CPP_OBJS  := $(addprefix $(OBJ_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Cpu.o Descriptor.o KernelImage.o)
# 存储抽象的产物：错误模型、统一路径、偏移视图与文件系统抽象
STORAGE_OBJS := $(addprefix $(OBJ_DIR)/,Error.o Path.o SubDevice.o FileSystem.o)
# 引导状态记录与吊销记录的产物：引导期选择器、运行期写入方（Updater / Init）与维护工具共用同一份协议
BOOT_OBJS := $(addprefix $(OBJ_DIR)/,BootState.o Revocation.o)
# 汇编源与产物；与同名 C++ 源以 _asm 后缀区分
ASM_OBJS  := $(addprefix $(OBJ_DIR)/,Cpu_asm.o Descriptor_asm.o)
# 全部产物
OBJS      := $(CPP_OBJS) $(STORAGE_OBJS) $(BOOT_OBJS) $(ASM_OBJS)

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

-include $(CPP_OBJS:.o=.d) $(STORAGE_OBJS:.o=.d) $(BOOT_OBJS:.o=.d)
