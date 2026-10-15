# Makefile
#    BaleenCore：BIOS 路径的第二阶段，链接本包与 Common 的目标文件，产出 BaleenCore.bin
#
#    镜像装入在物理 0x100000，由 Stub 按头里的入口偏移跳入；文件起点是入口前缀与
#    128 字节完整性头，0x90 起是 32 位保护模式代码；未初始化区不落盘，
#    由 Stub 在交权前按头里的 MemoryBytes 清零；
#    链接后由 Tools/Bin/PackImage 填头里的 BuildId 与 Digest，并校验头字段

# 产物根目录
OUT_DIR   ?= Out
# 本包产物根：默认与共享根相同；要单独换本包产物的落点（例如对照构建）时改这里，
# 上游对象的查找仍按 OUT_DIR，不受本包产物根影响
LOCAL_OUT_DIR ?= $(OUT_DIR)
# 上游对象的产物目录：各包共用 Out 约定，不随本包的产物根改变
UPSTREAM_OBJ_DIR := $(OUT_DIR)/Obj
# 本包对象目录
OBJ_DIR   := $(LOCAL_OUT_DIR)/Obj
# 本包产物目录
BIN_DIR   := $(LOCAL_OUT_DIR)/Bin
# 源码与头文件目录
SRC_DIR   := Src
INC_DIR   := Include
# 上游包：Common 提供 Print 与平台原语，Baleen/Common 提供 BIOS 路径控制台、镜像完整性头与交权契约
COMMON_DIR        := ../../Common
BALEEN_COMMON_DIR := ../Common
SHARE_DIR         := $(BALEEN_COMMON_DIR)/Include

# C++ 编译器、汇编器、链接器与镜像转换
CXX       ?= g++
NASM      ?= nasm
LD        ?= ld
OBJCOPY   ?= objcopy

# 编译选项：32 位保护模式、无宿主运行库、无异常与 RTTI、不开 PIC/PIE 与栈保护，警告视为错误
# 优化级别取 -Os：Baleen 的 32 位阶段在低 64KiB 静态窗口里运行，代码体积优先于指令数；
# 宿主工具与 64 位内核各自按 -O2 编译，不受这里影响
CXXFLAGS  ?= -m32 -std=c++20 -Os -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
             -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP \
             -I$(INC_DIR) -I$(LOCAL_OUT_DIR) -I$(COMMON_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include
# 汇编选项：32 位保护模式；-I 让 Src 里的 %INCLUDE 找到本包与共享的 Include；警告视为错误
NASMFLAGS ?= -f elf32 -Werror -I$(INC_DIR)/ -I$(SHARE_DIR)/
# 链接选项：无运行库并按 Core.ld 的固定布局；build-id 是额外的落盘节，不要，链接器警告视为错误
LDFLAGS   ?= -m elf_i386 -nostdlib -T Core.ld --build-id=none --fatal-warnings

# 本包目标文件：Core.cpp 是主流程，Console.cpp 是控制台装配，Ext4/BiosBlockDevice/Libc 服务系统卷读取，
# LoadKernel.cpp 定位并校验内核镜像，SelectSet.cpp 做选版（状态记录读写来自共用包的 BootState.cpp），
# ManifestVerify.cpp 验证内核集清单与产物，
# TrustAnchor.cpp 做信任锚查找，TrustAnchors.o 是构建期生成的锚表
LOCAL_OBJS := $(addprefix $(OBJ_DIR)/,Core.o Console.o Ext4.o BiosBlockDevice.o DiskFlush.o LoadKernel.o SelectSet.o ManifestVerify.o Libc.o TrustAnchor.o TrustAnchors.o Core_asm.o)
# 上游目标文件：Print 与平台原语，以及存储抽象（块设备、偏移视图、路径与文件系统接口）、内核镜像数据校验，
# 以及引导状态记录与吊销记录（后两者是两阶段共用的一份实现，吊销记录的验签在本包侧链入）
COMMON_OBJS := $(addprefix $(COMMON_DIR)/$(UPSTREAM_OBJ_DIR)/,Print.o SerialTarget.o DebugPortTarget.o Cpu.o Cpu_asm.o Descriptor.o Descriptor_asm.o Error.o Path.o SubDevice.o FileSystem.o KernelImage.o BootState.o Revocation.o)
BALEEN_COMMON_OBJS := $(addprefix $(BALEEN_COMMON_DIR)/$(UPSTREAM_OBJ_DIR)/,PrintTarget.o Diagnostics.o BootClock.o VgaTextTarget.o Sha256.o Sha512.o Ed25519.o ImageHeader.o BootMemory.o ReadFailure.o)

# 开发期签名密钥与信任锚源：私钥不进仓库，缺失时由 MakeKeys 生成后复用；
# 锚源是生成物，随密钥变化重新生成，供 --anchor-cpp 写进本包产物目录
KEYS_DIR       := ../../../.cache/Keys
TRUST_ANCHOR_SRC := $(LOCAL_OUT_DIR)/TrustAnchors.cpp
# 保留锚条目（生成输入，进版本库）：轮换窗口里仍要承认的旧公钥，默认空；
# 旧公钥由这份输入复现，不依赖本地构建产物，见 Docs/Specs/内核集清单与签名.md 第五节
ANCHORS_FILE   := Anchors/Retained.txt
# 密钥与锚源生成工具：产物在 Tools/Bin，随源码交付
MAKE_KEYS      := ../../../Tools/Bin/MakeKeys
# 内嵌的最低允许 SecurityVersion：发布时抬高它即吊销更低世代的清单，不必改源码；
# 生成头按内容变化重写，值没变时不触碰文件，ManifestVerify.o 的 .d 依赖会跟着重编
SECURITY_FLOOR   ?= 0
SECURITY_FLOOR_HPP := $(LOCAL_OUT_DIR)/SecurityFloor.hpp

# 链接产物与交付镜像
CORE_ELF  := $(BIN_DIR)/BaleenCore.elf
CORE_BIN  := $(BIN_DIR)/BaleenCore.bin
# 宿主侧打包工具：填充头里的 BuildId 与 Digest，并核对头字段与摘要
PACK_IMAGE := ../../../Tools/Bin/PackImage

.DEFAULT_GOAL := all
.PHONY: all upstream check clean help

# 顶层取用的是 BaleenCore.bin
all: $(CORE_BIN)

# 链接：上游对象由各自包的 Makefile 生成，子 make 自己判断是否需要重编
$(CORE_ELF): $(LOCAL_OBJS) $(COMMON_OBJS) $(BALEEN_COMMON_OBJS) Core.ld | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $@ $(LOCAL_OBJS) $(COMMON_OBJS) $(BALEEN_COMMON_OBJS)

# 建产物目录
$(BIN_DIR):
	@mkdir -p $@

$(COMMON_OBJS) $(BALEEN_COMMON_OBJS): upstream

upstream:
	@$(MAKE) -C $(COMMON_DIR)
	@$(MAKE) -C $(BALEEN_COMMON_DIR)

# 转平坦镜像并按头格式打包：未初始化区不落盘，交权前由 Stub 清零
$(CORE_BIN): $(CORE_ELF) $(PACK_IMAGE)
	$(OBJCOPY) -O binary $< $@
	$(PACK_IMAGE) $@

# 打包工具放在 Tools/Bin 随源码交付，缺失时现建一次
$(PACK_IMAGE):
	@$(MAKE) -C ../../../Tools/Build/PackImage

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 密钥与锚源工具放在 Tools/Bin 随源码交付，缺失时现建一次（与系统卷包同一套规则）
$(MAKE_KEYS):
	@$(MAKE) -C ../../../Tools/Build/MakeKeys

# 开发期签名密钥：缺失时由 MakeKeys 生成（0600，放在仓库外），已有则复用，重复构建不换锚
$(KEYS_DIR)/Development.sec: $(MAKE_KEYS)
	$(MAKE_KEYS) --out $(KEYS_DIR)

# 信任锚源：当前密钥与保留锚条目合并导出；换锚 = 换密钥文件并重建引导器，
# 打开轮换窗口 = 往保留锚条目里追加旧公钥
$(TRUST_ANCHOR_SRC): $(MAKE_KEYS) $(KEYS_DIR)/Development.sec $(ANCHORS_FILE)
	@mkdir -p $(dir $@)
	$(MAKE_KEYS) --out $(KEYS_DIR) --anchor-cpp $@ --key-id 1 --anchors $(ANCHORS_FILE)

# 生成物按本包编译选项编进镜像
$(OBJ_DIR)/TrustAnchors.o: $(TRUST_ANCHOR_SRC) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 内嵌下限头：每次 make 都跑（依赖 FORCE），内容一致时保留原文件，不触发无谓重编
$(SECURITY_FLOOR_HPP): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' \
	    '/* SecurityFloor.hpp' \
	    '    内嵌的最低允许 SecurityVersion（生成物）：由 BaleenCore 的 Makefile 按 SECURITY_FLOOR 写出' \
	    '' \
	    '    清单的 SecurityVersion 低于它一律拒绝；抬高它即吊销更低世代，' \
	    '    撤销的生效点是一次可信的引导器更新，见 Docs/Specs/内核集清单与签名.md 第五节' \
	    '*/' \
	    '' \
	    '#pragma once' \
	    '#include <stdint.h>' \
	    '' \
	    'namespace Baleen {' \
	    '    namespace Crypto {' \
	    '        // 最低允许的安全版本：低于它的清单不装载' \
	    '        constexpr uint64_t kMinimumSecurityVersion = $(SECURITY_FLOOR);' \
	    '    }' \
	    '}' > $@.tmp; \
	cmp -s $@.tmp $@ || { mv $@.tmp $@; echo "内嵌最低安全版本：$(SECURITY_FLOOR)"; }; \
	rm -f $@.tmp

# ManifestVerify.cpp 引用生成头，显式给出依赖，首次构建时不等 .d 文件
$(OBJ_DIR)/ManifestVerify.o: $(SECURITY_FLOOR_HPP)

FORCE:
.PHONY: FORCE

# 汇编产物与同名 C++ 源以 _asm 后缀区分，逐条列出
$(OBJ_DIR)/Core_asm.o: $(SRC_DIR)/Core.asm $(INC_DIR)/Const.inc $(INC_DIR)/CoreHeader.inc $(SHARE_DIR)/Contract.inc
	@mkdir -p $(OBJ_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

# 校验镜像非空且不超过 0x400000 上限，头字段与摘要由 PackImage --verify 复核
check: all
	@limit=$$((0x400000)); \
	bytes=$$(wc -c < "$(CORE_BIN)" | tr -d ' '); \
	[ "$$bytes" -gt 0 ] || { echo "check: $(CORE_BIN) 为空"; exit 1; }; \
	[ "$$bytes" -le "$$limit" ] || { echo "check: $(CORE_BIN) 大小 $$bytes，超出 0x400000"; exit 1; }; \
	echo "check: $(CORE_BIN) 大小 $$bytes"; \
	$(PACK_IMAGE) --verify "$(CORE_BIN)"

# 删除本包产物目录：共享根与上游对象不动
clean:
	@test -n "$(LOCAL_OUT_DIR)" || { echo "clean: LOCAL_OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(LOCAL_OUT_DIR)"

# 打印用法
help:
	@printf '%s\n' 'BaleenCore：BIOS 路径的第二阶段' \
	  '  make [all]  构建 $(CORE_BIN)' \
	  '  make check  校验镜像大小、完整性头字段与摘要' \
	  '  make clean  删除 $(LOCAL_OUT_DIR)/' \
	  '  变量：OUT_DIR（共享产物根，定位上游对象）/ LOCAL_OUT_DIR（本包产物根）' \
	  '        SECURITY_FLOOR（内嵌的最低允许 SecurityVersion，默认 0；抬高即吊销更低世代）' \
	  '        CXX / NASM / LD / OBJCOPY 与各 FLAGS 可覆盖'

-include $(LOCAL_OBJS:.o=.d)
