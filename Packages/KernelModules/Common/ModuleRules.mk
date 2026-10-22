# ModuleRules.mk
#    模块包的公共构建规则：编译、链接、链接后规范化与产物校验
#
#    一个模块包目录由三部分组成：
#      Module.mk  构建配置：模块名、逻辑名、版本、源清单与契约声明（模板见 ModuleTemplate.mk）
#      Makefile   一行入口，include 本文件，使 make -C Packages/KernelModules/<模块> 可用
#      Src/       模块源码
#    顶层 Packages/KernelModules/Makefile 遍历 */Module.mk，对每个包跑同一套规则
#
#    产物：$(OUT_DIR)/$(MODULE_NAME)/$(MODULE_NAME).elf，交给 PackModule 打成 .osm；
#    ELF 的形态约束（四段页对齐、入口符号、RELATIVE 重定位、无未定义符号）由 check 复核
#    公共源码按模块重编一份，不建 .a：弱符号默认入口放进静态库不会被拉入，链接期就静默丢失

# 模块包目录：单包构建时是 make -C 进入的目录，可被模块包覆盖
MODULE_DIR ?= $(CURDIR)
# 模块根目录：Common/ 与本文件所在处
KMOD_DIR   := $(abspath $(MODULE_DIR)/..)
# 公共源码目录
COMMON_DIR := $(KMOD_DIR)/Common

# 契约层与公共类型目录：生成的契约头（Out/Contract）与 BootInfo/Storage 等公共头
# 模块源码通过 ModuleAbi.hpp 与生成头引用同一份契约定义，不手写布局副本
COMMON_PKG_DIR := $(abspath $(KMOD_DIR)/../Common)
CONTRACT_DIR   := $(COMMON_PKG_DIR)/Out/Contract
COMMON_INCLUDE := $(COMMON_PKG_DIR)/Include

# 模块包构建配置：必须先于下面的变量默认值与规则载入
include $(MODULE_DIR)/Module.mk

# 构建配置缺项在解析期失败，不产出半套产物
ifeq ($(strip $(MODULE_NAME)),)
$(error Module.mk 未设置 MODULE_NAME：模块包构建配置缺失)
endif
ifeq ($(strip $(MODULE_LOGICAL_NAME)),)
$(error Module.mk 未设置 MODULE_LOGICAL_NAME：参与 ModuleId 派生的逻辑名不可省略)
endif
ifeq ($(strip $(MODULE_MAJOR)),)
$(error Module.mk 未设置 MODULE_MAJOR：容器要求模块主版本非零)
endif
ifeq ($(strip $(MODULE_MAJOR)),0)
$(error Module.mk 的 MODULE_MAJOR 为 0：容器要求模块主版本非零)
endif

# minor / patch 缺省为 0；模块版本是 major.minor.patch 三个 u16（容器 4.2）
MODULE_MINOR ?= 0
MODULE_PATCH ?= 0

# 产物根目录与本次构建的产物目录；OUT_DIR 由顶层或命令行统一指定时各包共用
OUT_DIR    ?= $(KMOD_DIR)/Out
MODULE_OUT := $(OUT_DIR)/$(MODULE_NAME)
OBJ_DIR    := $(MODULE_OUT)/Obj
# 模块 ELF：PackModule 的输入（未 strip，入口偏移从符号表取）
ELF        := $(MODULE_OUT)/$(MODULE_NAME).elf
# 打包工具与模块容器产物：.osm 落 $(OUT_DIR)/Modules/，系统卷包的默认模块来源
PACK_MODULE ?= $(abspath $(KMOD_DIR)/../../Tools/Bin/PackModule)
OSM_DIR     ?= $(OUT_DIR)/Modules
OSM         := $(OSM_DIR)/$(MODULE_NAME).$(MODULE_MAJOR).$(MODULE_MINOR).$(MODULE_PATCH).osm
# 契约工具：模块契约校验与 PackModule 参数生成（与顶层 contract-generated 同一份工具）
MAKE_CONTRACT ?= $(abspath $(KMOD_DIR)/../../Tools/Bin/MakeContract)
# 契约生成头是模块编译的硬输入：缺失时报出生成命令，不在模块构建里抢写 Out/Contract
ifeq ($(filter clean help,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(CONTRACT_DIR)/Abi.hpp),)
$(error 缺少契约生成头 $(CONTRACT_DIR)/Abi.hpp：先在仓库根执行 make contract-generated)
endif
endif
# 模块声明内核 ABI 范围与依赖表：范围按容器 4.3，65535 表示不设上限
MODULE_ABI_MIN ?= 1
MODULE_ABI_MAX ?= 65535
MODULE_DEPENDS ?=

# 模块源码：Module.mk 显式登记，不靠通配，新增 .cpp 必须登记（与内核 Core 的约定一致）
MODULE_SRCS ?= Src/Module.cpp
MODULE_OBJS := $(addprefix $(OBJ_DIR)/,$(MODULE_SRCS:.cpp=.o))
# 公共源码：最小 C 运行库必带；MODULE_ENTRY_DEFAULTS 非空时带入口弱符号默认实现
COMMON_SRCS := Libc.cpp
ifneq ($(strip $(MODULE_ENTRY_DEFAULTS)),)
COMMON_SRCS += ModuleEntry.cpp
endif
COMMON_OBJS := $(addprefix $(OBJ_DIR)/Common/,$(COMMON_SRCS:.cpp=.o))
OBJS        := $(MODULE_OBJS) $(COMMON_OBJS)

# 必须存在的段：容器要求 TEXT 段与入口；骨架样板额外要求四段齐全（见 Module.mk）
MODULE_REQUIRED_SECTIONS ?= .text

# C++ 编译器、链接器与段表规范化工具
CXX     ?= g++
LD      ?= ld
OBJCOPY ?= objcopy

# 编译选项：严格按 P5 计划 T1.2
#   -fcf-protection=none：宿主 GCC 默认 full，会插 endbr64 与 .note.gnu.property；
#   ring 0 不启用 CET，显式关掉以保持段表干净（不在计划清单里，属对宿主默认值的归一化）
#   -MMD -MP 与仓库其余包一致；-ffile-prefix-map 保证产物不含构建路径（T1.5 复现要求）
#   版本与逻辑名经 -D 传入源码：Module.mk 是唯一事实源，源码不重复写死
CXXFLAGS ?= -m64 -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti -fPIC -fvisibility=hidden \
            -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables -fcf-protection=none \
            -Wall -Wextra -Werror -MMD -MP \
            -ffile-prefix-map=$(MODULE_DIR)=. -ffile-prefix-map=$(KMOD_DIR)=. \
            -I$(COMMON_DIR) -I$(MODULE_DIR)/Include \
            -I$(CONTRACT_DIR) -I$(COMMON_INCLUDE) \
            -DMODULE_LOGICAL_NAME='"$(MODULE_LOGICAL_NAME)"' \
            -DMODULE_MAJOR=$(MODULE_MAJOR) -DMODULE_MINOR=$(MODULE_MINOR) -DMODULE_PATCH=$(MODULE_PATCH)
# 链接选项：ET_DYN、无宿主运行库、无未定义符号、无 build-id；基址 0 的模块映像
#   --no-ld-generated-unwind-info 关掉 ld 为 -shared 生成的 .eh_frame，让模块自己的
#   .eh_frame（异常或栈展开）成为 orphan 而被 --orphan-handling=error 拦下
#   .init_array/.fini_array/.tdata/.tbss 同理：不静默丢弃，链接期直接失败
LDFLAGS ?= -m elf_x86_64 -shared -nostdlib -Bsymbolic --no-undefined --build-id=none \
           -z max-page-size=0x1000 --no-ld-generated-unwind-info --orphan-handling=error \
           --fatal-warnings -T $(COMMON_DIR)/Module.ld

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all check package contract-check clean help

# 构建本模块 ELF
all: $(ELF)

# 打包成 .osm：导出/导入/依赖来自 Module.mk 的契约声明
#   导出项写成 <稳定名>:<版本>，槽数由 MakeContract 从 Interfaces.def 推出，模块配置不写
#   导入项写成 <稳定名>:<最小版本>:<最大版本>:<使用槽数>
#   依赖项写成 <逻辑名>:<最小主版本>:<最大主版本>
#   契约声明先过 MakeContract --module-check：接口/版本存在、槽数不超提供者、闭包单例提供者
#   与固定表一致、Core.* 不得由模块导出；通过后输出 PackModule 的 --export/--import 参数
CONTRACT_CHECK_ARGS := $(foreach e,$(CONTRACT_EXPORTS),--export $(e) )$(foreach i,$(CONTRACT_IMPORTS),--import $(i) )
package: $(OSM)

# 契约校验：只检查 Module.mk 的导出/导入声明并打印 PackModule 参数，不编译、不打包
# 模块 owner 自检与 M5 契约面收口用：make -C Packages/KernelModules/<模块> contract-check
contract-check:
	@$(MAKE_CONTRACT) --module-check --contract-dir $(COMMON_PKG_DIR)/Contract --logical-name $(MODULE_LOGICAL_NAME) $(CONTRACT_CHECK_ARGS)

# 打包工具与契约工具缺失时现建一次：干净树上直接 make 也能出 .osm
$(PACK_MODULE):
	@$(MAKE) -C $(abspath $(KMOD_DIR)/../../Tools/Build/PackModule)
$(MAKE_CONTRACT):
	@$(MAKE) -C $(abspath $(KMOD_DIR)/../../Tools/Build/MakeContract)

$(OSM): $(ELF) $(PACK_MODULE) $(MAKE_CONTRACT) $(MODULE_DIR)/Module.mk
	@mkdir -p $(OSM_DIR)
	@contract_args=$$($(MAKE_CONTRACT) --module-check --contract-dir $(COMMON_PKG_DIR)/Contract --logical-name $(MODULE_LOGICAL_NAME) $(CONTRACT_CHECK_ARGS)) || exit 1; \
	$(PACK_MODULE) --elf $(ELF) --out $@ --name $(MODULE_LOGICAL_NAME) \
	    --version $(MODULE_MAJOR).$(MODULE_MINOR).$(MODULE_PATCH) \
	    --abi-min $(MODULE_ABI_MIN) --abi-max $(MODULE_ABI_MAX) \
	    $$contract_args \
	    $(foreach d,$(MODULE_DEPENDS),--depends $(d) )

# 链接后把重定位表清成非 alloc：它只给 PackModule 读文件，不进入装载映像
# 无重定位的模块没有 .rela.dyn 段，条件跳过
$(ELF): $(OBJS) $(COMMON_DIR)/Module.ld $(MODULE_DIR)/Module.mk
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@if LC_ALL=C readelf -SW $@ | grep -q '\.rela\.dyn'; then \
	    $(OBJCOPY) --set-section-flags .rela.dyn=readonly $@; \
	fi

# 模块源码
$(OBJ_DIR)/%.o: $(MODULE_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 公共源码：每个模块各自重编一份，编译选项与模块源码相同
$(OBJ_DIR)/Common/%.o: $(COMMON_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 校验：按容器与计划 T1.2 的硬约束逐项断言，任一不符即失败
#   readelf/nm 一律 LC_ALL=C：断言看英文标记，不受宿主 locale 影响
check: all
	@elf=$(ELF); \
	LC_ALL=C readelf -h "$$elf" | grep -q 'Class:.*ELF64' || { echo "check: $$elf 不是 ELF64"; exit 1; }; \
	LC_ALL=C readelf -h "$$elf" | grep -q 'Type:.*DYN' || { echo "check: $$elf 不是 ET_DYN"; exit 1; }; \
	LC_ALL=C readelf -h "$$elf" | grep -q 'Machine:.*X86-64' || { echo "check: $$elf 不是 x86-64"; exit 1; }; \
	if [ -n "$$(LC_ALL=C nm -u "$$elf")" ]; then echo "check: $$elf 有未定义符号"; LC_ALL=C nm -u "$$elf"; exit 1; fi; \
	bad=$$(LC_ALL=C readelf -rW "$$elf" | grep -o 'R_X86_64_[A-Za-z0-9_]*' | sort -u | grep -v '^R_X86_64_RELATIVE$$' || true); \
	[ -z "$$bad" ] || { echo "check: $$elf 含非 RELATIVE 重定位：$$bad"; exit 1; }; \
	for section in $(MODULE_REQUIRED_SECTIONS); do \
	    LC_ALL=C readelf -SW "$$elf" | grep -q "\] $$section " || { echo "check: $$elf 缺段 $$section"; exit 1; }; \
	done; \
	for section in .init_array .fini_array .tdata .tbss .eh_frame .dynsym .dynamic; do \
	    if LC_ALL=C readelf -SW "$$elf" | grep -q "\] $$section "; then echo "check: $$elf 出现禁止段 $$section"; exit 1; fi; \
	done; \
	for section in .text .rodata .data .bss; do \
	    addr=$$(LC_ALL=C objdump -h "$$elf" | awk -v name="$$section" '$$2 == name { print $$4 }'); \
	    [ -n "$$addr" ] || continue; \
	    case "$$addr" in *000) ;; *) echo "check: $$section 地址 $$addr 未按 4 KiB 对齐"; exit 1;; esac; \
	done; \
	if [ -n "$$(LC_ALL=C objdump -h "$$elf" | grep -A1 '\.rela\.dyn' | grep 'ALLOC')" ]; then echo "check: $$elf 的 .rela.dyn 带 alloc 标志"; exit 1; fi; \
	entry=$$(LC_ALL=C nm "$$elf" | awk '$$3 == "_Module_Init" { print $$1 }'); \
	[ -n "$$entry" ] || { echo "check: $$elf 缺入口符号 _Module_Init"; exit 1; }; \
	[ "$$entry" != "0000000000000000" ] || { echo "check: _Module_Init 偏移为 0，容器要求 EntryInit 非零"; exit 1; }; \
	LC_ALL=C nm "$$elf" | grep -q ' T _Module_Init$$' || { echo "check: _Module_Init 不在 .text"; exit 1; }; \
	LC_ALL=C readelf -sW "$$elf" | grep -q 'GLOBAL DEFAULT.*_Module_Init$$' || { echo "check: _Module_Init 不是默认可见性的全局符号"; exit 1; }; \
	echo "check: $$elf ELF64/ET_DYN、无未定义符号、重定位仅 RELATIVE、必需段 4 KiB 对齐、入口 _Module_Init 在 .text"

# 删除本模块产物目录
clean:
	@test -n "$(MODULE_OUT)" || { echo "clean: MODULE_OUT 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(MODULE_OUT)"

# 打印用法
help:
	@printf '%s\n' '$(MODULE_NAME)：.osm 模块包的构建入口（规则来自 Common/ModuleRules.mk）' \
	  '  make [all]  构建 $(ELF)' \
	  '  make check  复核 ELF 形态、段对齐、重定位与入口符号' \
	  '  make package 用 PackModule 打成 $(OSM)' \
	  '  make clean  删除 $(MODULE_OUT)/' \
	  '  变量：OUT_DIR / CXX / LD / OBJCOPY 与各 FLAGS 可覆盖'

-include $(OBJS:.o=.d)
