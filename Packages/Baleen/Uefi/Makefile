# Makefile
#    Packages/Baleen/Uefi —— UEFI 侧的两件固件应用
#
#      BOOTX64.EFI  x64 侧的引导器：本项目的 UEFI 引导路径，PE32+ 固件应用
#      BOOTIA32.EFI IA32 侧的不支持提示件：32 位固件上打印一行提示后停机，不引导
#
#    x64 侧复用 BIOS 路径的共享实现：存储抽象、布局描述、Ext4 只读实现、诊断编号与 Print 前端。
#    Packages/Common 的默认构建面向 32 位阶段（-m32），因此本包按 x64 自己再编一份，
#    对象落在本包 Out/Obj 下，不与其它的 32 位对象混链
#
#    EFI 应用是 PE 形态、子系统 10：ImageBase 非零与 .reloc 段是固件加载与重定位的硬要求
#    （IA32 用 -m i386pe，x64 用 -m i386pep）

# 产物根目录
OUT_DIR   ?= Out
# 对象目录与产物目录
OBJ_DIR   := $(OUT_DIR)/Obj
BIN_DIR   := $(OUT_DIR)/Bin
# 源码与头文件目录
SRC_DIR   := Src
INC_DIR   := Include

# C++ 编译器、链接器与汇编器
CXX       ?= g++
LD        ?= ld
NASM      ?= nasm

# 上游源码目录：Common 是共用的存储抽象与 Print 前端，Baleen/Common 是诊断编号写法，
# Core 是 Ext4 只读实现与它引用的最小 C 运行库
COMMON_DIR        := ../../Common
BALEEN_COMMON_DIR := ../Common
CORE_DIR          := ../Core

# 公共编译选项：无宿主运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护，警告视为错误
# -fno-ident 去掉 .comment：它落在映像基址之下，PE 链接会为它报 "section below image base"
COMMON_FLAGS := -std=c++20 -Os -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
                -fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident -Wall -Wextra -Werror

# 32 位提示件的编译选项
CXXFLAGS32 ?= -m32 $(COMMON_FLAGS)
# 链接选项：直接链成 PE32 的 EFI 应用。三项是装配侧实测出来的硬要求：
#   --image-base 非零：PE 头里 ImageBase 为 0 的镜像会被固件判为不受支持
#   --enable-reloc-section：生成 .reloc；固件把映像装到别处时要靠它重定位，缺了同样加载失败
#   --subsystem 10：EFI 应用
LDFLAGS32  ?= -m i386pe --image-base 0x400000 --enable-reloc-section --subsystem 10 -nostdlib -e efi_main

# x64 引导器的编译选项：本包与上游源码都按这套选项编 x64
# -mno-red-zone 是固件环境的要求：本阶段中断是开着的，红色区域会被固件的中断处理打断
# -DBALEEN_STAGE_PREFIX：共享实现的诊断编号前缀随阶段换（同号异前缀），序号不变
# -I$(CORE_OUT)：ManifestVerify.cpp 引用 Core 包生成的内嵌最低安全版本头
CXXFLAGS64 ?= -m64 -mno-red-zone $(COMMON_FLAGS) -MMD -MP -DBALEEN_STAGE_PREFIX='"E-UEFI"' \
              -I$(INC_DIR) -I$(COMMON_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include -I$(CORE_DIR)/Include -I$(CORE_OUT)
# 链接选项与 32 位同一套约束，只是目标格式换成 PE32+
LDFLAGS64  ?= -m i386pep --image-base 0x400000 --enable-reloc-section --subsystem 10 -nostdlib -e efi_main
# 汇编选项：交接入口是 64 位代码，链进同一个 PE
NASMFLAGS ?= -f elf64 -Werror

# —— 32 位提示件 ——
# 交付产物：ESP 的 \EFI\BOOT\BOOTIA32.EFI
EFI32     := $(BIN_DIR)/BOOTIA32.EFI
# 中间产物
OBJ32     := $(OBJ_DIR)/BootIa32.o

# —— x64 引导器 ——
# 交付产物：ESP 的 \EFI\BOOT\BOOTX64.EFI
EFI64     := $(BIN_DIR)/BOOTX64.EFI
# 本包自己的对象：主流程、控制台装配、块设备适配、内存图、引导计时与文本目标，外加交接入口的汇编
LOCAL_OBJS64 := $(addprefix $(OBJ_DIR)/,BootX64.o Console.o EfiBlockDevice.o EfiTextTarget.o UefiMemory.o UefiClock.o EnterKernel.o)
# 上游对象：存储抽象、Print 前端与诊断写法，Ext4 只读实现与最小 C 运行库，
# 以及选版、清单验签、内核镜像校验与信任锚——后四项是与 BaleenCore 共用的同一份源码，
# 按 x64 再编一份，诊断编号前缀由 CXXFLAGS64 里的 BALEEN_STAGE_PREFIX 换成 E-UEFI
# 各源文件的对象名互不相同，因此与本地对象共用一个对象目录不会撞名
SHARED_OBJS64 := $(addprefix $(OBJ_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Error.o Path.o SubDevice.o FileSystem.o Diagnostics.o Ext4.o Libc.o \
                 LoadKernel.o SelectSet.o ManifestVerify.o TrustAnchor.o TrustAnchors.o \
                 BootState.o Revocation.o KernelImage.o Sha256.o Sha512.o Ed25519.o ImageHeader.o BootMemory.o)
OBJS64    := $(LOCAL_OBJS64) $(SHARED_OBJS64)

# Core 包的生成物：信任锚源与内嵌最低安全版本头。两个包各编一份，生成只有一处
CORE_OUT          := $(CORE_DIR)/Out
TRUST_ANCHORS_SRC := $(CORE_OUT)/TrustAnchors.cpp
SECURITY_FLOOR_HPP := $(CORE_OUT)/SecurityFloor.hpp

# —— ESP 镜像 ——
# 顶层按 EFI_IMAGE 的通配符优先取这一份，取不到才退到占位件
ESP_IMAGE := $(BIN_DIR)/Efi.img
# ESP 的组装工具：FAT16 布局只有 MakePayloads 一份实现，本包不另写一份
MAKE_PAYLOADS := ../../../Tools/Bin/MakePayloads

.DEFAULT_GOAL := all
.PHONY: all check clean help

all: $(ESP_IMAGE) $(EFI64) $(EFI32)

# 从本包、上游与 Core 生成物的目录里找源文件
vpath %.cpp $(SRC_DIR) $(COMMON_DIR)/Src $(COMMON_DIR)/Src/Storage $(BALEEN_COMMON_DIR)/Src $(CORE_DIR)/Src $(CORE_OUT)

# x64 侧：本包源码与上游源码共用一条编译规则
$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS64) -c -o $@ $<

# 交接入口：64 位汇编，与 C++ 对象一起链进同一个 PE
$(OBJ_DIR)/EnterKernel.o: $(SRC_DIR)/EnterKernel.asm | $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# Core 包的生成物：缺了就现建一次（Core 的 Makefile 会连密钥与锚源一起处理）
$(TRUST_ANCHORS_SRC) $(SECURITY_FLOOR_HPP):
	@$(MAKE) -C $(CORE_DIR)

# 首次构建时不等 .d：显式声明对生成物的依赖
$(OBJ_DIR)/TrustAnchors.o: $(TRUST_ANCHORS_SRC)
$(OBJ_DIR)/ManifestVerify.o: $(SECURITY_FLOOR_HPP)

# 32 位提示件单独一条规则：它不引用任何上游头文件
$(OBJ32): $(SRC_DIR)/BootIa32.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS32) -c -o $@ $<

# 直接链成 PE32 的 EFI 应用
$(EFI32): $(OBJ32) | $(BIN_DIR)
	$(LD) $(LDFLAGS32) -o $@ $<

# 直接链成 PE32+ 的 EFI 应用
$(EFI64): $(OBJS64) | $(BIN_DIR)
	$(LD) $(LDFLAGS64) -o $@ $(OBJS64)

$(OBJ_DIR) $(BIN_DIR):
	@mkdir -p $@

# ESP：把两件产物装进 \EFI\BOOT\，交给 MakePayloads 组装——它持有唯一的 FAT16 布局实现
$(ESP_IMAGE): $(EFI64) $(EFI32) $(MAKE_PAYLOADS) | $(BIN_DIR)
	$(MAKE_PAYLOADS) --esp-image --out $@ --bootx64 $(EFI64) --bootia32 $(EFI32)

# 组装工具放在 Tools/Bin 随源码交付，缺失时现建一次（与其它包的规则一致）
$(MAKE_PAYLOADS):
	@$(MAKE) -C ../../../Tools/Build/MakePayloads

# 复核两件产物的形态：PE 位宽、基址非零、带重定位段、关键文本在位
check: all
	@objdump -f "$(EFI32)" | grep -q "pei-i386" || { echo "check: $(EFI32) 不是 PE32（pei-i386）"; exit 1; }
	@objdump -p "$(EFI32)" | grep -qE "ImageBase[[:space:]]+00400000" || { echo "check: $(EFI32) 的 ImageBase 不是 00400000（固件会判为不受支持）"; exit 1; }
	@objdump -h "$(EFI32)" | grep -q "\.reloc" || { echo "check: $(EFI32) 缺 .reloc 段（固件重定位会失败）"; exit 1; }
	@strings -el "$(EFI32)" | grep -q "32-bit UEFI (IA32)" || { echo "check: $(EFI32) 里没有提示文本"; exit 1; }
	@strings -el "$(EFI32)" | grep -q "E-UEFI-11" || { echo "check: $(EFI32) 里没有登记编号 E-UEFI-11"; exit 1; }
	@objdump -f "$(EFI64)" | grep -q "pei-x86-64" || { echo "check: $(EFI64) 不是 PE32+（pei-x86-64）"; exit 1; }
	@objdump -p "$(EFI64)" | grep -qE "ImageBase[[:space:]]+0*400000[[:space:]]*$$" || { echo "check: $(EFI64) 的 ImageBase 不是 0x400000（固件会判为不受支持）"; exit 1; }
	@objdump -h "$(EFI64)" | grep -q "\.reloc" || { echo "check: $(EFI64) 缺 .reloc 段（固件重定位会失败）"; exit 1; }
	@strings "$(EFI64)" | grep -q "BALEENUEFI" || { echo "check: $(EFI64) 里没有阶段标签"; exit 1; }
	@strings "$(EFI64)" | grep -q "E-UEFI-17" || { echo "check: $(EFI64) 里没有登记编号 E-UEFI-17"; exit 1; }
	@strings "$(EFI64)" | grep -q "E-UEFI-78" || { echo "check: $(EFI64) 里没有登记编号 E-UEFI-78"; exit 1; }
	@echo "check: $(EFI32) 是 PE32 EFI 应用（$$(wc -c < "$(EFI32)") 字节）"
	@echo "check: $(EFI64) 是 PE32+ EFI 应用（$$(wc -c < "$(EFI64)") 字节）"

clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

help:
	@printf '%s\n' 'Packages/Baleen/Uefi：UEFI 侧的固件应用' \
	  '  make [all]  构建 $(EFI64)（x64 引导器）与 $(EFI32)（IA32 提示件）' \
	  '  make check  复核两件产物是 PE / PE32+ EFI 应用且关键文本在位' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / LD / CXXFLAGS32 / CXXFLAGS64 / LDFLAGS32 / LDFLAGS64 可覆盖'

-include $(OBJS64:.o=.d)
