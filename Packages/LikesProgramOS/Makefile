# Makefile
#    LikesProgramOS —— 系统本体的构建：内核映像与系统卷镜像
#
#      make [all]     构建系统卷 $(SYSTEM_VOLUME)
#      make kernel    只构建内核映像并复核
#      make clean     删除本包 Out/ 与 Static/ 下的内核集目录；clean-all 连同 Core 一起清理
#
#    Static/ 是系统卷根的源：构建期按原样写进卷，各模块产物由本 Makefile 从自己的
#    Out/ 拷进 Static/ 的对应落点；卷内布局见 Docs/Specs/Common/系统卷布局.md
#    .keep 是 Git 的存在标记（空目录不进 Git），组装时被丢掉，不进系统卷
#
#    内核集：内核映像与签名清单落进 Static/LikesProgram/System/KernelSets/<BuildId>/，
#    BuildId 从内核映像数据里读（PackImage 已填），清单由 MakeManifest 生成、SignManifest 签名；
#    开发期签名密钥由 MakeKeys 生成在 ../../.cache/Keys（不进仓库），首次构建后复用；
#    当前集的卷内路径写进 Out/KernelPath，供顶层组装器回填布局描述

# 产物根目录
OUT_DIR       ?= Out
# 内核侧 Core 的目录，产物在它自己的 Out/Bin 下
CORE_DIR      := Core
# 系统卷根的源目录：目录骨架与手工维护的内容进 Git，拷入的构建产物不进 Git
STATIC_DIR    := Static
# 组装用的卷根副本：Static 去掉存在标记后的目录，只在构建期存在
VOLUME_ROOT   := $(OUT_DIR)/VolumeRoot
# 内核映像：取内核侧 Core 的产物；OUT_DIR 落在包外（绝对路径）时不再拼 CORE_DIR
ifeq ($(filter /%,$(OUT_DIR)),)
KERNEL_IMAGE  := $(CORE_DIR)/$(OUT_DIR)/Bin/LikesProgram.os
else
KERNEL_IMAGE  := $(OUT_DIR)/Bin/LikesProgram.os
endif
# 内核集：卷内根路径、产物名与清单名；集目录名是 BuildId 的 64 位小写十六进制
KERNEL_SET_ROOT   := /LikesProgram/System/KernelSets
KERNEL_IMAGE_NAME := LikesProgram.os
MANIFEST_NAME     := Manifest.mft
# 集目录：Static 下与卷内路径同构，构建时按当前 BuildId 重建
KERNEL_SETS_DIR := $(STATIC_DIR)$(KERNEL_SET_ROOT)
# 内核路径文件：把当前集的卷内路径写给顶层组装器，用于回填布局描述
KERNEL_PATH_FILE := $(OUT_DIR)/KernelPath
# 系统卷镜像
SYSTEM_VOLUME := $(OUT_DIR)/LikesProgram.img
# 卷大小与卷标识：固定值保证同一份输入的卷标识可复现，组装器按它写布局描述
VOLUME_BYTES  ?= 16777216
VOLUME_BLOCKS ?= 4096
VOLUME_UUID   ?= 4c50534f-534f-4b01-8000-000000000001
# mkfs 工具：需要支持 -d 的 e2fsprogs
MKFS          ?= mke2fs
# 特性参数与占位件生成器保持同一套：关掉惰性初始化，卷内容一次写全
MKFS_OPTIONS  ?= hash_seed=66666666-7777-8888-9999-aaaaaaaaaaaa,root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0
# 清单工具与开发期签名密钥：私钥不进仓库，缺失时由 MakeKeys 生成后复用
MAKE_MANIFEST := ../../Tools/Bin/MakeManifest
SIGN_MANIFEST := ../../Tools/Bin/SignManifest
KEYS_DIR      := ../../.cache/Keys
SIGN_KEY      := $(KEYS_DIR)/Development.sec
# 清单的安全版本号与签名密钥编号
SECURITY_VERSION ?= 1
KEY_ID           ?= 1
# Static 的全部文件：任一变化都重做系统卷
STATIC_FILES  := $(shell find $(STATIC_DIR) -type f 2>/dev/null)
# Static 下的全部目录：目录的修改时间在增删条目时变化，因此"删掉一个文件"也能让卷重做。
# 只列文件会漏掉这一种变化——列表自己缩短，不影响任何仍然存在的前提的修改时间，
# 目标是新的，make 于是判定无需重建。集目录在配方里重建，mkfs 在它之后写卷，所以每次构建结束后卷仍是最新的
STATIC_DIRS   := $(shell find $(STATIC_DIR) -type d 2>/dev/null)

.DEFAULT_GOAL := all
.PHONY: all kernel check clean clean-all help

all: $(SYSTEM_VOLUME)

# 只构建内核映像并复核
kernel:
	@$(MAKE) -C $(CORE_DIR) check

# 内核侧 Core 与本包共享层的源文件：任一变化都要重新进子 make 判断
# 顶层 make 直接调用过 Core/Makefile，本包不能假定产物已是最新，依赖源文件自己判断
CORE_FILES := $(shell find $(CORE_DIR)/Src $(CORE_DIR)/Include -type f 2>/dev/null) \
              $(CORE_DIR)/Makefile $(CORE_DIR)/Core.ld \
              $(shell find ../Common/Include ../Common/Src -type f 2>/dev/null)

# 内核映像由 Core 的 Makefile 构建；源文件变化时重新进入子 make
$(KERNEL_IMAGE): $(CORE_FILES)
	@$(MAKE) -C $(CORE_DIR)

# 清单与签名工具放在 Tools/Bin 随源码交付，缺失时现建一次
$(MAKE_MANIFEST):
	@$(MAKE) -C ../../Tools/Build/MakeManifest

$(SIGN_MANIFEST):
	@$(MAKE) -C ../../Tools/Build/SignManifest

# 开发期签名密钥：缺失时由 MakeKeys 生成（0600），已有则复用，重复构建不换锚
$(SIGN_KEY):
	@$(MAKE) -C ../../Tools/Build/MakeKeys
	../../Tools/Bin/MakeKeys --out $(KEYS_DIR)

# 系统卷：先把内核映像与签名清单落进当前 BuildId 的集目录，再按卷根组装
# 集目录按当前 BuildId 重建：同一份内核产物只保留一个集，历史集不随卷发布
$(SYSTEM_VOLUME): $(KERNEL_IMAGE) $(STATIC_FILES) $(STATIC_DIRS) $(MAKE_MANIFEST) $(SIGN_MANIFEST) $(SIGN_KEY) Makefile
	@command -v $(MKFS) >/dev/null 2>&1 || { echo "缺少 $(MKFS)（e2fsprogs，需要支持 -d 的版本）"; exit 1; }
	@set -e; \
	build_id=$$(od -An -tx1 -j 40 -N 32 "$(KERNEL_IMAGE)" | tr -d ' \n'); \
	[ "$${#build_id}" -eq 64 ] || { echo "从 $(KERNEL_IMAGE) 读不出 32 字节 BuildId"; exit 1; }; \
	set_dir="$(KERNEL_SETS_DIR)/$$build_id"; \
	rm -rf "$(KERNEL_SETS_DIR)"; \
	mkdir -p "$$set_dir"; \
	cp "$(KERNEL_IMAGE)" "$$set_dir/$(KERNEL_IMAGE_NAME)"; \
	$(MAKE_MANIFEST) --out "$$set_dir/$(MANIFEST_NAME)" --build-id-from "$(KERNEL_IMAGE)" \
	    --security-version $(SECURITY_VERSION) --artifact 1:$(KERNEL_IMAGE_NAME):"$$set_dir/$(KERNEL_IMAGE_NAME)"; \
	$(SIGN_MANIFEST) --in "$$set_dir/$(MANIFEST_NAME)" --key "$(SIGN_KEY)" --key-id $(KEY_ID); \
	kern=$$(wc -c < "$$set_dir/$(KERNEL_IMAGE_NAME)" | tr -d ' '); \
	[ "$$kern" -lt "$$(( $(VOLUME_BYTES) / 2 ))" ] || { echo "内核映像 $$kern 字节，超过卷容量的一半：$(VOLUME_BYTES) 字节"; exit 1; }; \
	mkdir -p "$(OUT_DIR)"; \
	printf '%s\n' "$(KERNEL_SET_ROOT)/$$build_id/$(KERNEL_IMAGE_NAME)" > "$(KERNEL_PATH_FILE)"; \
	rm -rf $(VOLUME_ROOT) && mkdir -p $(VOLUME_ROOT); \
	cp -a $(STATIC_DIR)/. $(VOLUME_ROOT)/; \
	find $(VOLUME_ROOT) -name .keep -delete; \
	$(MKFS) -t ext4 -b 4096 -q -F -U $(VOLUME_UUID) -E "$(MKFS_OPTIONS)" -d $(VOLUME_ROOT) $(SYSTEM_VOLUME) $(VOLUME_BLOCKS); \
	rm -rf $(VOLUME_ROOT); \
	size=$$(wc -c < "$(SYSTEM_VOLUME)" | tr -d ' '); \
	[ "$$size" = "$(VOLUME_BYTES)" ] || { echo "系统卷大小 $$size 与约定不符 $(VOLUME_BYTES)"; exit 1; }
	@echo "系统卷 $(SYSTEM_VOLUME)：$(VOLUME_BYTES) 字节，内核集路径 $$(cat $(KERNEL_PATH_FILE))"

# 校验内核映像与集目录落点：内核映像的字段与摘要由 PackImage --verify 复核
check: all
	@$(MAKE) -C $(CORE_DIR) check
	@test -f "$(KERNEL_PATH_FILE)" || { echo "check: 缺少 $(KERNEL_PATH_FILE)"; exit 1; }
	@set -e; \
	set_dir="$(STATIC_DIR)$$(cat $(KERNEL_PATH_FILE))"; \
	test -f "$$set_dir" || { echo "check: 缺少卷内落点 $$set_dir"; exit 1; }; \
	cmp -s "$(KERNEL_IMAGE)" "$$set_dir" || { echo "check: 卷内落点与内核产物不一致"; exit 1; }; \
	test -f "$$(dirname "$$set_dir")/$(MANIFEST_NAME)" || { echo "check: 缺少签名清单"; exit 1; }; \
	test -f "$(SYSTEM_VOLUME)" || { echo "check: 缺少 $(SYSTEM_VOLUME)"; exit 1; }; \
	echo "check: 卷内落点 $$(cat $(KERNEL_PATH_FILE)) 与内核产物一致，清单已签名"

# 删除本包产物：Out/ 与 Static/ 下的内核集目录（集目录是构建产物，不进 Git）
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"
	@test -n "$(KERNEL_SETS_DIR)" || { echo "clean: KERNEL_SETS_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(KERNEL_SETS_DIR)"

# 连同内核侧 Core 的产物一起删除
clean-all: clean
	@$(MAKE) -C $(CORE_DIR) clean

# 打印用法
help:
	@printf '%s\n' 'LikesProgramOS：内核映像与系统卷' \
	  '  make [all]   构建 $(SYSTEM_VOLUME)，卷根来自 $(STATIC_DIR)/' \
	  '  make kernel  只构建内核映像并复核' \
	  '  make check   复核内核映像、集目录落点与签名清单' \
	  '  make clean   删除 $(OUT_DIR)/ 与 $(KERNEL_SETS_DIR)/；clean-all 连同 $(CORE_DIR)/ 一起清理' \
	  '  变量：OUT_DIR / VOLUME_UUID / VOLUME_BYTES / SECURITY_VERSION / KEY_ID / MKFS 可覆盖' \
	  '  内核集落点：$(KERNEL_SET_ROOT)/<BuildId>/，路径写在 $(KERNEL_PATH_FILE)'
