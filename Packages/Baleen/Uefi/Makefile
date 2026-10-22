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
# 强制自建控制器路径的测试变体：1 时即使固件有块设备协议也走 PCI 自建路径，
# 产物落在 Out/Controller 下，不覆盖正式产物；只用于验证，不进发布
CONTROLLER_PATH ?= 0
ifeq ($(CONTROLLER_PATH),1)
LOCAL_OUT_DIR := $(OUT_DIR)/Controller
else
LOCAL_OUT_DIR := $(OUT_DIR)
endif
# 对象目录与产物目录
OBJ_DIR   := $(LOCAL_OUT_DIR)/Obj
BIN_DIR   := $(LOCAL_OUT_DIR)/Bin
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

# 内核模块目录：自建控制器路径复用内核侧的 AHCI / NVMe 只读实现
KERNEL_MODULES_DIR := ../../KernelModules

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
#   --no-insert-timestamp：PE 头不写构建时间，同一份输入产出同样的字节（可复现构建）
LDFLAGS32  ?= -m i386pe --image-base 0x400000 --enable-reloc-section --subsystem 10 -nostdlib -e efi_main --no-insert-timestamp

# x64 引导器的编译选项：本包与上游源码都按这套选项编 x64
# -mno-red-zone 是固件环境的要求：本阶段中断是开着的，红色区域会被固件的中断处理打断
# -DBALEEN_STAGE_PREFIX：共享实现的诊断编号前缀随阶段换（同号异前缀），序号不变
# -I$(CORE_OUT)：ManifestVerify.cpp 引用 Core 包生成的内嵌最低安全版本头
# FATAL 等待毫秒数：0 表示内核按默认 10 秒；测试镜像可给小值加速失败注入
FATAL_WAIT_MS ?= 0
# 自建控制器路径的测试变体：定义 FORCE_CONTROLLER_PATH 让 BindMedium 跳过固件块协议
CONTROLLER_DEFINE := $(if $(filter 1,$(CONTROLLER_PATH)),-DFORCE_CONTROLLER_PATH=1,)
CXXFLAGS64 ?= -m64 -mno-red-zone $(COMMON_FLAGS) -MMD -MP -DBALEEN_STAGE_PREFIX='"E-UEFI"' \
              -I$(INC_DIR) -I$(COMMON_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include -I$(CORE_DIR)/Include -I$(CORE_OUT) \
              -I$(COMMON_DIR)/Out/Contract -I$(KERNEL_MODULES_DIR)/Common \
              -I$(KERNEL_MODULES_DIR)/Ahci/Src -I$(KERNEL_MODULES_DIR)/Nvme/Src \
              -DFATAL_WAIT_MS=$(FATAL_WAIT_MS) $(CONTROLLER_DEFINE)
# 链接选项与 32 位同一套约束，只是目标格式换成 PE32+
LDFLAGS64  ?= -m i386pep --image-base 0x400000 --enable-reloc-section --subsystem 10 -nostdlib -e efi_main --no-insert-timestamp
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
# 本包自己的对象：主流程、控制台装配、固件块设备适配、自建控制器路径、内存图、
# 引导计时与文本目标，外加交接入口的汇编
LOCAL_OBJS64 := $(addprefix $(OBJ_DIR)/,BootX64.o Console.o EfiBlockDevice.o ControllerPath.o EfiTextTarget.o UefiMemory.o ScreenFacts.o UefiClock.o EnterKernel.o)
# 自建控制器路径复用的内核模块只读实现（同一份源码按 x64 再编一份）
CONTROLLER_OBJS64 := $(addprefix $(OBJ_DIR)/,Ahci.o Nvme.o)
# 上游对象：存储抽象、Print 前端与诊断写法，Ext4 只读实现与最小 C 运行库，
# 以及选版、清单验签、内核镜像校验与信任锚——后四项是与 BaleenCore 共用的同一份源码，
# 按 x64 再编一份，诊断编号前缀由 CXXFLAGS64 里的 BALEEN_STAGE_PREFIX 换成 E-UEFI
# SHA-256 取自 Common 的共享目标代码（Sha256.cpp 从 Common/Src 按 x64 再编一份）
# 各源文件的对象名互不相同，因此与本地对象共用一个对象目录不会撞名
# UEFI 侧不校验自身镜像头（镜像头字段校验 E-UEFI-40..51 在本阶段不可达），
# 因此 ImageHeader.o 不链入：它带着三张按阶段排的编号表，链接进去会让死编号重新出现在二进制里
SHARED_OBJS64 := $(addprefix $(OBJ_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Error.o Path.o SubDevice.o FileSystem.o Diagnostics.o Ext4.o Libc.o \
                 LoadKernel.o SelectSet.o ManifestVerify.o BootClosure.o TrustAnchor.o TrustAnchors.o \
                 BootState.o Revocation.o KernelImage.o Sha256.o Sha512.o Ed25519.o BootMemory.o)
OBJS64    := $(LOCAL_OBJS64) $(CONTROLLER_OBJS64) $(SHARED_OBJS64)

# Core 包的生成物：信任锚源/锚文本与内嵌最低安全版本头。两个包各编一份，生成只有一处
CORE_OUT           := $(CORE_DIR)/Out
TRUST_ANCHORS_SRC  := $(CORE_OUT)/TrustAnchors.cpp
TRUST_ANCHORS_TEXT := $(CORE_OUT)/TrustAnchors.txt
SECURITY_FLOOR_HPP := $(CORE_OUT)/SecurityFloor.hpp

# —— ESP 镜像 ——
# 顶层按 EFI_IMAGE 的通配符优先取这一份，取不到才退到占位件
ESP_IMAGE := $(BIN_DIR)/Efi.img
# ESP 的组装工具：FAT16 布局只有 MakePayloads 一份实现，本包不另写一份
MAKE_PAYLOADS := ../../../Tools/Bin/MakePayloads

.DEFAULT_GOAL := all
.PHONY: all check clean help core-generated

all: $(ESP_IMAGE) $(EFI64) $(EFI32)

# Core 生成物是真正的顺序依赖：每次构建先让 Core 的 Makefile 判断密钥、锚源、锚文本与
# 内嵌下限是否要重做，而不是“文件不存在才子构建”。产物缺失、密钥换了、锚输入改了，
# 都在这里刷新；所有对象与链接都 order-only 依赖它，因此不会用到上一轮的旧生成物
core-generated:
	@$(MAKE) -C $(CORE_DIR)

# Core 生成物的兜底声明：旧 .d 文件把它们登记成对象的普通前提，没有规则时 make 会
# 在 core-generated 之前按“没有规则可制作目标”退出。这里只声明“由 core-generated 产出”
# 并核对存在，不另起一遍子 make——否则与 core-generated 并发时会同时写同一个 Out/。
$(TRUST_ANCHORS_SRC) $(TRUST_ANCHORS_TEXT) $(SECURITY_FLOOR_HPP): core-generated
	@test -f "$@" || { echo "缺少由 Core 生成的 $@（core-generated 没有产出它）"; exit 1; }

# 信任锚源是生成物：编译规则显式给出（不靠通配 vpath 在生成前找源文件），
# 源文件由 core-generated 保证已生成；后续构建的重新编译由 -MMD 写出的 .d 依赖驱动
$(OBJ_DIR)/TrustAnchors.o: | $(OBJ_DIR) core-generated
	$(CXX) $(CXXFLAGS64) -c -o $@ $(TRUST_ANCHORS_SRC)

# 契约层生成物：BootClosure.cpp 引用生成的引导闭包期望集头；缺了就先跑契约生成器
MAKE_CONTRACT := ../../../Tools/Bin/MakeContract
$(MAKE_CONTRACT):
	@$(MAKE) -C ../../../Tools/Build/MakeContract
$(COMMON_DIR)/Out/Contract/BootClosure.hpp: $(MAKE_CONTRACT)
	@cd ../../.. && Tools/Bin/MakeContract
$(OBJ_DIR)/BootClosure.o: $(COMMON_DIR)/Out/Contract/BootClosure.hpp

# 从本包、上游、Core 生成物与内核模块目录里找源文件
vpath %.cpp $(SRC_DIR) $(COMMON_DIR)/Src $(COMMON_DIR)/Src/Storage $(BALEEN_COMMON_DIR)/Src $(CORE_DIR)/Src $(CORE_OUT) \
            $(KERNEL_MODULES_DIR)/Ahci/Src $(KERNEL_MODULES_DIR)/Nvme/Src

# x64 侧：本包源码与上游源码共用一条编译规则
# order-only 依赖 core-generated：编译前 Core 的锚源与内嵌下限一定已生成（或已确认最新）
$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR) core-generated
	$(CXX) $(CXXFLAGS64) -c -o $@ $<

# 目标侧 SHA-256 来自 Common 的共享目标代码：vpath 顺序已让 Common/Src 优先于
# Baleen/Common/Src，这里再显式给出源文件，避免两处同名源随 vpath 调整而漂移
$(OBJ_DIR)/Sha256.o: $(COMMON_DIR)/Src/Sha256.cpp | $(OBJ_DIR) core-generated
	$(CXX) $(CXXFLAGS64) -c -o $@ $<

# AHCI / NVMe 只读实现：内核模块源码按 x64 编进引导器；诊断不经过模块日志，纯读路径
$(OBJ_DIR)/Ahci.o: $(KERNEL_MODULES_DIR)/Ahci/Src/Ahci.cpp | $(OBJ_DIR) core-generated
	$(CXX) $(CXXFLAGS64) -c -o $@ $<

$(OBJ_DIR)/Nvme.o: $(KERNEL_MODULES_DIR)/Nvme/Src/Nvme.cpp | $(OBJ_DIR) core-generated
	$(CXX) $(CXXFLAGS64) -c -o $@ $<

# 交接入口：64 位汇编，与 C++ 对象一起链进同一个 PE
$(OBJ_DIR)/EnterKernel.o: $(SRC_DIR)/EnterKernel.asm | $(OBJ_DIR) core-generated
	$(NASM) $(NASMFLAGS) $< -o $@

# 32 位提示件单独一条规则：它不引用任何上游头文件
$(OBJ32): $(SRC_DIR)/BootIa32.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS32) -c -o $@ $<

# 直接链成 PE32 的 EFI 应用
$(EFI32): $(OBJ32) | $(BIN_DIR)
	$(LD) $(LDFLAGS32) -o $@ $<

# 直接链成 PE32+ 的 EFI 应用
$(EFI64): $(OBJS64) | $(BIN_DIR) core-generated
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
	@strings "$(EFI64)" | grep -q "E-UEFI-100" || { echo "check: $(EFI64) 里没有登记编号 E-UEFI-100"; exit 1; }
	@strings "$(EFI64)" | grep -q "E-UEFI-104" || { echo "check: $(EFI64) 里没有登记编号 E-UEFI-104"; exit 1; }
	@strings "$(EFI64)" | grep -q "The self-built controller path is bound" || { echo "check: $(EFI64) 里没有自建控制器路径的绑定文本"; exit 1; }
	@command -v mdir >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1 || { echo "check: 缺少 mtools（mdir/mcopy），无法核对 $(ESP_IMAGE) 的内容"; exit 1; }
	@mdir -i "$(ESP_IMAGE)" ::/EFI/BOOT >/dev/null || { echo "check: $(ESP_IMAGE) 里没有 \\EFI\\BOOT 目录"; exit 1; }
	@mdir -i "$(ESP_IMAGE)" ::/EFI/BOOT | grep -q "BOOTX64" || { echo "check: $(ESP_IMAGE) 的 \\EFI\\BOOT 里没有 BOOTX64.EFI"; exit 1; }
	@mdir -i "$(ESP_IMAGE)" ::/EFI/BOOT | grep -q "BOOTIA32" || { echo "check: $(ESP_IMAGE) 的 \\EFI\\BOOT 里没有 BOOTIA32.EFI"; exit 1; }
	@mcopy -i "$(ESP_IMAGE)" ::/EFI/BOOT/BOOTX64.EFI - | cmp -s - "$(EFI64)" || { echo "check: $(ESP_IMAGE) 里的 BOOTX64.EFI 与 $(EFI64) 不一致"; exit 1; }
	@mcopy -i "$(ESP_IMAGE)" ::/EFI/BOOT/BOOTIA32.EFI - | cmp -s - "$(EFI32)" || { echo "check: $(ESP_IMAGE) 里的 BOOTIA32.EFI 与 $(EFI32) 不一致"; exit 1; }
	@echo "check: $(EFI32) 是 PE32 EFI 应用（$$(wc -c < "$(EFI32)") 字节）；ESP 内容与它逐字节一致"
	@strings "$(EFI64)" | grep -q "ControllerPath" 2>/dev/null || true
	@echo "check: $(EFI64) 是 PE32+ EFI 应用（$$(wc -c < "$(EFI64)") 字节）；ESP 内容与它逐字节一致"

clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

help:
	@printf '%s\n' 'Packages/Baleen/Uefi：UEFI 侧的固件应用' \
	  '  make [all]  构建 $(EFI64)（x64 引导器）与 $(EFI32)（IA32 提示件）' \
	  '  make check  复核两件产物是 PE / PE32+ EFI 应用且关键文本在位，并核对 ESP 内容与它们逐字节一致' \
	  '  make clean  删除 $(OUT_DIR)/' \
	  '  变量：OUT_DIR / CXX / LD / CXXFLAGS32 / CXXFLAGS64 / LDFLAGS32 / LDFLAGS64 可覆盖'

-include $(OBJS64:.o=.d)
