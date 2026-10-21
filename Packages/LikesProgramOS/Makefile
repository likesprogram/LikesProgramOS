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
# 内核映像：默认取内核侧 Core 的产物；OUT_DIR 落在包外（绝对路径）时不再拼 CORE_DIR
# 变体构建（不同锚表/吊销记录）可以显式覆盖：同一个内核产物配不同的系统卷
ifndef KERNEL_IMAGE
ifeq ($(filter /%,$(OUT_DIR)),)
KERNEL_IMAGE  := $(CORE_DIR)/$(OUT_DIR)/Bin/LikesProgram.os
else
KERNEL_IMAGE  := $(OUT_DIR)/Bin/LikesProgram.os
endif
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
# 当前密钥的信任锚编号：顶层 export；签名与锚表命中核对用同一个编号
KEY_ID           ?= 1
# 信任锚文本（BaleenCore 生成物）：签名时核对“签名密钥确实在锚表里”，check 里回验清单签名。
# 引导器内嵌的锚表由 Baleen/Core 包生成；本包只读它，缺失时让那个包现生成一次
TRUST_ANCHORS_FILE ?= ../Baleen/Core/Out/TrustAnchors.txt
# 可选的签名吊销记录：非空时复制到卷内 /LikesProgram/System/Revocation.rvk；
# 该文件变化重做系统卷，变体镜像因此能带上指定的吊销记录
REVOCATION_FILE ?=
# 构建时间戳：写进 Ext4 元数据；固定值保证同一组输入产出同样的卷字节
SOURCE_DATE_EPOCH ?= 0
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

# 信任锚文本是 BaleenCore 的生成物：独立构建本包时按需让那个包生成一次
$(TRUST_ANCHORS_FILE):
	@$(MAKE) -C ../Baleen/Core trust-anchors

# 系统卷：先把内核映像与签名清单落进当前 BuildId 的集目录，再按卷根组装
# 集目录按当前 BuildId 重建：同一份内核产物只保留一个集，历史集不随卷发布
$(SYSTEM_VOLUME): $(KERNEL_IMAGE) $(STATIC_FILES) $(STATIC_DIRS) $(MAKE_MANIFEST) $(SIGN_MANIFEST) $(SIGN_KEY) $(TRUST_ANCHORS_FILE) $(REVOCATION_FILE) Makefile
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
	test -f "$(TRUST_ANCHORS_FILE)" || { echo "缺少信任锚文本 $(TRUST_ANCHORS_FILE)：先构建 $(CORE_DIR)"; exit 1; }; \
	$(SIGN_MANIFEST) --in "$$set_dir/$(MANIFEST_NAME)" --key "$(SIGN_KEY)" --key-id $(KEY_ID) --anchors "$(TRUST_ANCHORS_FILE)"; \
	kern=$$(wc -c < "$$set_dir/$(KERNEL_IMAGE_NAME)" | tr -d ' '); \
	[ "$$kern" -lt "$$(( $(VOLUME_BYTES) / 2 ))" ] || { echo "内核映像 $$kern 字节，超过卷容量的一半：$(VOLUME_BYTES) 字节"; exit 1; }; \
	mkdir -p "$(OUT_DIR)"; \
	printf '%s\n' "$(KERNEL_SET_ROOT)/$$build_id/$(KERNEL_IMAGE_NAME)" > "$(KERNEL_PATH_FILE)"; \
	rm -rf $(VOLUME_ROOT) && mkdir -p $(VOLUME_ROOT); \
	cp -a $(STATIC_DIR)/. $(VOLUME_ROOT)/; \
	find $(VOLUME_ROOT) -name .keep -delete; \
	if [ -n "$(REVOCATION_FILE)" ]; then \
	    test -f "$(REVOCATION_FILE)" || { echo "缺少吊销记录 $(REVOCATION_FILE)"; exit 1; }; \
	    mkdir -p "$(VOLUME_ROOT)/LikesProgram/System"; \
	    cp "$(REVOCATION_FILE)" "$(VOLUME_ROOT)/LikesProgram/System/Revocation.rvk"; \
	fi; \
	find $(VOLUME_ROOT) -exec touch -h -d @$(SOURCE_DATE_EPOCH) {} +; \
	E2FSPROGS_FAKE_TIME=$(SOURCE_DATE_EPOCH) SOURCE_DATE_EPOCH=$(SOURCE_DATE_EPOCH) TZ=UTC LC_ALL=C \
	    $(MKFS) -t ext4 -b 4096 -q -F -U $(VOLUME_UUID) -E "$(MKFS_OPTIONS)" -d $(VOLUME_ROOT) $(SYSTEM_VOLUME) $(VOLUME_BLOCKS); \
	rm -rf $(VOLUME_ROOT); \
	size=$$(wc -c < "$(SYSTEM_VOLUME)" | tr -d ' '); \
	[ "$$size" = "$(VOLUME_BYTES)" ] || { echo "系统卷大小 $$size 与约定不符 $(VOLUME_BYTES)"; exit 1; }
	@echo "系统卷 $(SYSTEM_VOLUME)：$(VOLUME_BYTES) 字节，内核集路径 $$(cat $(KERNEL_PATH_FILE))"

# 校验内核映像、集目录落点与签名链：内核映像的字段与摘要由 PackImage --verify 复核；
# 清单要能被信任锚文本验签、BuildId 与内核一致、.os 条目的长度/摘要/路径是内核产物的，
# 且签名密钥编号确实在锚表里
check: all
	@$(MAKE) -C $(CORE_DIR) check
	@test -f "$(KERNEL_PATH_FILE)" || { echo "check: 缺少 $(KERNEL_PATH_FILE)"; exit 1; }
	@test -f "$(TRUST_ANCHORS_FILE)" || { echo "check: 缺少 $(TRUST_ANCHORS_FILE)"; exit 1; }
	@set -e; \
	set_dir="$(STATIC_DIR)$$(cat $(KERNEL_PATH_FILE))"; \
	manifest="$$(dirname "$$set_dir")/$(MANIFEST_NAME)"; \
	test -f "$$set_dir" || { echo "check: 缺少卷内落点 $$set_dir"; exit 1; }; \
	cmp -s "$(KERNEL_IMAGE)" "$$set_dir" || { echo "check: 卷内落点与内核产物不一致"; exit 1; }; \
	test -f "$$manifest" || { echo "check: 缺少签名清单"; exit 1; }; \
	test -f "$(SYSTEM_VOLUME)" || { echo "check: 缺少 $(SYSTEM_VOLUME)"; exit 1; }; \
	grep -q "^$(KEY_ID) " "$(TRUST_ANCHORS_FILE)" || { echo "check: 信任锚文本里没有 KeyId $(KEY_ID)"; exit 1; }; \
	$(SIGN_MANIFEST) --verify --in "$$manifest" --key "$(SIGN_KEY)" --key-id $(KEY_ID) --anchors "$(TRUST_ANCHORS_FILE)"; \
	kbid=$$(od -An -tx1 -j 40 -N 32 "$(KERNEL_IMAGE)" | tr -d ' \n'); \
	mbid=$$(od -An -tx1 -j 16 -N 32 "$$manifest" | tr -d ' \n'); \
	test "$$kbid" = "$$mbid" || { echo "check: 清单 BuildId 与内核产物不一致"; exit 1; }; \
	entry_bytes=$$(od -An -tu8 -j 68 -N 8 "$$manifest" | tr -d ' '); \
	kern_bytes=$$(wc -c < "$(KERNEL_IMAGE)" | tr -d ' '); \
	test "$$entry_bytes" = "$$kern_bytes" || { echo "check: 清单 .os 条目的字节数与内核产物不一致"; exit 1; }; \
	entry_digest=$$(od -An -tx1 -j 76 -N 32 "$$manifest" | tr -d ' \n'); \
	kern_digest=$$(sha256sum "$(KERNEL_IMAGE)" | cut -d' ' -f1); \
	test "$$entry_digest" = "$$kern_digest" || { echo "check: 清单 .os 条目的摘要与内核产物不一致"; exit 1; }; \
	entry_path_bytes=$$(od -An -tu2 -j 65 -N 2 "$$manifest" | tr -d ' '); \
	entry_path=$$(dd if="$$manifest" bs=1 skip=108 count=$$entry_path_bytes 2>/dev/null); \
	test "$$entry_path" = "$(KERNEL_IMAGE_NAME)" || { echo "check: 清单 .os 条目的路径不是 $(KERNEL_IMAGE_NAME)"; exit 1; }; \
	echo "check: 卷内落点 $$(cat $(KERNEL_PATH_FILE)) 与内核产物一致，清单验签通过（KeyId $(KEY_ID) 在锚表里）"

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
	  '        REVOCATION_FILE（可选吊销记录，非空时进卷内 /LikesProgram/System/Revocation.rvk）' \
	  '        KERNEL_IMAGE（内核映像，变体构建可覆盖）/ TRUST_ANCHORS_FILE（签名锚文本）' \
	  '  内核集落点：$(KERNEL_SET_ROOT)/<BuildId>/，路径写在 $(KERNEL_PATH_FILE)'
