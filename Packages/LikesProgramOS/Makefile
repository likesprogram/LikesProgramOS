# Makefile
#    LikesProgramOS —— 系统本体的构建：内核集与系统卷镜像
#
#      make [all]     构建系统卷 $(SYSTEM_VOLUME)
#      make kernel    只构建内核原型镜像并复核（身份尚未分配）
#      make clean     删除本包 Out/ 与 Static/ 下的内核集目录；clean-all 连同 Core 一起清理
#
#    Static/ 是系统卷根的源：构建期按原样写进卷，各模块产物由本 Makefile 从自己的
#    Out/ 拷进 Static/ 的对应落点；卷内布局见 Docs/Specs/Common/系统卷布局.md
#    .keep 是 Git 的存在标记（空目录不进 Git），组装时被丢掉，不进系统卷
#
#    内核集：Core 产出未分配身份的原型 .os；MakeManifest 派生发布线 BuildId（只由 .os 内容
#    与路径决定，模块补丁不变），写进最终 .os，生成 Modules.list 与清单，SignManifest 签名后
#    .os、Modules.list、清单与空 Modules/ 目录落进 Static/LikesProgram/System/KernelSets/<BuildId>/，
#    BuildId 的文本同时写进 Out/BuildId；开发期签名密钥由 MakeKeys 生成在 ../../.cache/Keys
#    （不进仓库），首次构建后复用；当前集的卷内路径写进 Out/KernelPath，供顶层组装器回填布局描述

# 产物根目录
OUT_DIR       ?= Out
# 内核侧 Core 的目录，产物在它自己的 Out/Bin 下
CORE_DIR      := Core
# 系统卷根的源目录：目录骨架与手工维护的内容进 Git，拷入的构建产物不进 Git
STATIC_DIR    := Static
# 组装用的卷根副本：Static 去掉存在标记后的目录，只在构建期存在
VOLUME_ROOT   := $(OUT_DIR)/VolumeRoot
# 原型内核镜像：BuildId 与 Digest 未分配，身份由 MakeManifest 在生成集时写入
# 默认取内核侧 Core 的产物；OUT_DIR 落在包外（绝对路径）时不再拼 CORE_DIR
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
# 集目录的根：正式构建落在 Static/ 下（top-level 读取那里）；变体构建（同集两修订夹具）把它指到
# 自己的 OUT_DIR，避免覆盖正式集目录
KERNEL_SETS_BASE ?= $(STATIC_DIR)
KERNEL_SETS_DIR := $(KERNEL_SETS_BASE)$(KERNEL_SET_ROOT)
# 集暂存目录：MakeManifest 在这里写最终 .os、Modules.list 与清单，签名后再落进集目录
KERNEL_SET_STAGE := $(OUT_DIR)/KernelSetStage
STAGED_KERNEL    := $(KERNEL_SET_STAGE)/$(KERNEL_IMAGE_NAME)
STAGED_MANIFEST  := $(KERNEL_SET_STAGE)/$(MANIFEST_NAME)
# 模块：版本化文件名平铺在集目录的 Modules/ 下；Modules.list 与 Manifest.mft 同级
# Modules.list 是内核侧书写的完整模块加载列表：该修订全部 .osm 条目，顺序即内核装载偏好
MODULES_DIR_NAME   := Modules
MODULE_LIST_NAME   := Modules.list
STAGED_MODULE_LIST := $(KERNEL_SET_STAGE)/$(MODULE_LIST_NAME)
STAGED_MODULES_DIR := $(KERNEL_SET_STAGE)/$(MODULES_DIR_NAME)
# 模块修订号：每次模块补丁发布单调递增；BuildId 与集目录不随它变化
MODULE_REVISION ?= 1
# 模块产物来源：../KernelModules 构建出的版本化 .osm（平铺、无版本目录）；缺失或为空时按空集合构建
MODULES_SRC_DIR ?= ../KernelModules/Out/Modules
# 引导闭包成员表：成员与顺序的唯一事实源；Baleen 与内核都按它从 Modules.list 里筛闭包成员
BOOT_CLOSURE_DEF   := ../Common/Contract/BootClosure.def
BOOT_CLOSURE_NAMES := $(shell sed -n 's/^closure [0-9][0-9]* \([A-Za-z][A-Za-z0-9]*\).*/\1/p' $(BOOT_CLOSURE_DEF) 2>/dev/null)
# 正式集只收引导闭包成员：骨架样板 Hello 等非闭包模块不进正式集目录（运行期夹具走 RUNTIME_MODULES 变体）；
# 这是内核侧的发行选择，不是 Baleen 的限制——列表本身按全部 --artifact .osm 自动生成
MODULE_OSMS     := $(foreach name,$(BOOT_CLOSURE_NAMES),$(wildcard $(MODULES_SRC_DIR)/$(name).*.osm))
# 上一修订快照（测试夹具）：PREVIOUS_REVISION_DIR 指向一份旧修订的暂存目录（含 Manifest.mft 与 Modules.list），
# 非空时按 PREVIOUS_MODULE_REVISION 复制成 Manifest.<rev>.mft / Modules.<rev>.list；留 M2 的选择器回退夹具使用
PREVIOUS_MODULE_REVISION ?=
PREVIOUS_REVISION_DIR ?=
# 运行期模块夹具（G6d 验证用，不进正式交付集）：每项 "<集内相对路径>=<源文件>"，例如
#   Modules/Fault.1.0.0.osm=<Fault .osm>
#   Modules/Optional/Optional.1.0.0.osm=<Optional .osm>
# M6 夹具约定：Modules/ 下直接子项 = 必需运行期模块，Modules/Optional/ 子目录 = 可选运行期模块；
# 两者都以清单 Type=2 条目与 Modules.list 条目登记（列表是内核侧完整模块清单），
# Baleen 只预装其中的引导闭包成员，运行期模块由内核挂卷后装载。
# 正式清单 Type 语义留后续阶段；正式卷 $(SYSTEM_VOLUME) 的配方不引用本变量，运行期夹具因此不会进正式发布集
RUNTIME_MODULES          ?=
# 运行期夹具变体的产物根、系统卷与内核路径文件：与正式构建的 Out/ 分开，互不覆盖
RUNTIME_OUT_DIR          ?= $(OUT_DIR)/Runtime
RUNTIME_VOLUME           ?= $(RUNTIME_OUT_DIR)/LikesProgram.img
RUNTIME_KERNEL_PATH_FILE ?= $(RUNTIME_OUT_DIR)/KernelPath
# 故障注入测试变体（T7.1②，仅验证）：先改一个闭包模块的字节，再对它重新生成清单并签名，
# 于是“清单与签名都合法、容器内容故意坏”——Baleen 不解析容器，能把它送进内核
#   PATCH_MODE=corrupt  ：把容器头的段表偏移改坏（Baleen 最小读取面不读该字段）→ 内核 E-KRN-57
#   PATCH_MODE=initfail ：把 Init 入口改成 mov eax,6; ret（返回值 kCorrupt）     → 内核 E-KRN-59
PATCH_MODE   ?=
PATCH_MODULE ?= Modules/DevModel.1.0.0.osm
# BuildId 文本：MakeManifest 写出的 64 位小写十六进制，集目录名与卷内路径都由它推出
BUILD_ID_FILE := $(OUT_DIR)/BuildId
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
# 清单、签名与校验工具与开发期签名密钥：私钥不进仓库，缺失时由 MakeKeys 生成后复用
MAKE_MANIFEST := ../../Tools/Bin/MakeManifest
SIGN_MANIFEST := ../../Tools/Bin/SignManifest
PACK_IMAGE    := ../../Tools/Bin/PackImage
KEYS_DIR      := ../../.cache/Keys
SIGN_KEY      := $(KEYS_DIR)/Development.sec
# 清单的安全版本号与签名密钥编号
SECURITY_VERSION ?= 1
# 当前密钥的信任锚编号：顶层 export；签名与锚表命中核对用同一个编号
KEY_ID           ?= 1
# KEY_ID 的内容戳：make 不追踪变量值变化。清单签名用 $(KEY_ID)、锚表也用 $(KEY_ID)，
# 两边必须同代；把当前值写成文件，KEY_ID 变了才刷新时间戳，卷随之重建，不会留旧密钥签名的清单
KEY_ID_STAMP     := $(OUT_DIR)/KeyId
# 信任锚文本（BaleenCore 生成物）：签名时核对“签名密钥确实在锚表里”，check 里回验清单签名。
# 引导器内嵌的锚表由 Baleen/Core 包生成；本包只读它，缺失时让那个包现生成一次
TRUST_ANCHORS_FILE ?= ../Baleen/Core/Out/TrustAnchors.txt
# 可选的签名吊销记录：非空时复制到卷内 /LikesProgram/System/Revocation.rvk；
# 该文件变化重做系统卷，变体镜像因此能带上指定的吊销记录
REVOCATION_FILE ?=
# 构建时间戳：写进 Ext4 元数据；固定值保证同一组输入产出同样的卷字节
SOURCE_DATE_EPOCH ?= 0
# Static 的全部文件：任一变化都重做系统卷；内核集目录是本 Makefile 的构建产物，不算静态输入
# （把它算进去会让 `make clean all` 在同一次调用里引用自己刚删掉的旧集文件，且每次重建集目录都会误判过期）
STATIC_FILES  := $(shell find $(STATIC_DIR) -path $(KERNEL_SETS_DIR) -prune -o -type f -print 2>/dev/null)
# Static 下的全部目录：目录的修改时间在增删条目时变化，因此"删掉一个文件"也能让卷重做。
# 只列文件会漏掉这一种变化——列表自己缩短，不影响任何仍然存在的前提的修改时间，
# 目标是新的，make 于是判定无需重建。集目录在配方里重建，mkfs 在它之后写卷，所以每次构建结束后卷仍是最新的
STATIC_DIRS   := $(shell find $(STATIC_DIR) -path $(KERNEL_SETS_DIR) -prune -o -type d -print 2>/dev/null)

.DEFAULT_GOAL := all
.PHONY: all kernel check clean clean-all help FORCE

all: $(SYSTEM_VOLUME)

# 只构建原型内核镜像并复核；身份未分配是合法状态，生成集时才算
kernel:
	@$(MAKE) -C $(CORE_DIR) check

# 内核侧 Core 与本包共享层的源文件：任一变化都要重新进子 make 判断
# 顶层 make 直接调用过 Core/Makefile，本包不能假定产物已是最新，依赖源文件自己判断
CORE_FILES := $(shell find $(CORE_DIR)/Src $(CORE_DIR)/Include -type f 2>/dev/null) \
              $(CORE_DIR)/Makefile $(CORE_DIR)/Core.ld \
              $(shell find ../Common/Include ../Common/Src -type f 2>/dev/null)

# 原型内核镜像由 Core 的 Makefile 构建；源文件变化时重新进入子 make
$(KERNEL_IMAGE): $(CORE_FILES)
	@$(MAKE) -C $(CORE_DIR)

# 清单、签名与校验工具放在 Tools/Bin 随源码交付，缺失时现建一次
$(MAKE_MANIFEST):
	@$(MAKE) -C ../../Tools/Build/MakeManifest

$(SIGN_MANIFEST):
	@$(MAKE) -C ../../Tools/Build/SignManifest

$(PACK_IMAGE):
	@$(MAKE) -C ../../Tools/Build/PackImage

# 开发期签名密钥：缺失时由 MakeKeys 生成（0600），已有则复用，重复构建不换锚
$(SIGN_KEY):
	@$(MAKE) -C ../../Tools/Build/MakeKeys
	../../Tools/Bin/MakeKeys --out $(KEYS_DIR)

# KEY_ID 内容戳：每次 make 检查一次（依赖 FORCE）；内容没变就不写，时间戳不动，卷不重建
$(KEY_ID_STAMP): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' "$(KEY_ID)" | cmp -s - $@ || printf '%s\n' "$(KEY_ID)" > $@

# FORCE：无产物的空目标，让依赖它的规则每次 make 都跑一次（是否真变化由规则自己用 cmp 判断）
FORCE:

# 信任锚文本是 BaleenCore 的生成物：独立构建本包时按需让那个包生成一次。
# 前提里挂本包的 KEY_ID 内容戳：顶层 packages 的串行顺序里本包先于 Baleen/Core 构建，
# 增量 `make KEY_ID=2` 时锚表目标若只按“文件存在”判断就会沿用旧编号的锚表，
# 随后 SignManifest 直接拒绝（锚表里没有 KeyId 2）。挂上内容戳后：
# KEY_ID 变 → 本包 stamp 刷新 → 本目标过期 → 子 make 进 Core 重建锚表（Core 侧同样按内容戳判断）。
# KEY_ID 不变时 stamp 内容相同、不写文件，本目标不会过期，锚表也不重建。
$(TRUST_ANCHORS_FILE): $(KEY_ID_STAMP)
	@$(MAKE) -C ../Baleen/Core trust-anchors

# 系统卷：先在集暂存目录派生 BuildId、写最终 .os 与清单并签名，再按 BuildId 落进集目录组装卷根
# 集目录按当前 BuildId 重建：同一份内核产物只保留一个集，历史集不随卷发布
$(SYSTEM_VOLUME): $(KERNEL_IMAGE) $(STATIC_FILES) $(STATIC_DIRS) $(MODULE_OSMS) $(MAKE_MANIFEST) $(SIGN_MANIFEST) $(SIGN_KEY) $(TRUST_ANCHORS_FILE) $(REVOCATION_FILE) $(KEY_ID_STAMP) Makefile
	@command -v $(MKFS) >/dev/null 2>&1 || { echo "缺少 $(MKFS)（e2fsprogs，需要支持 -d 的版本）"; exit 1; }
	@set -e; \
	mkdir -p "$(OUT_DIR)"; \
	rm -rf "$(KERNEL_SET_STAGE)"; \
	mkdir -p "$(KERNEL_SET_STAGE)"; \
	mkdir -p "$(STAGED_MODULES_DIR)"; \
	for osm in $(MODULE_OSMS); do cp "$$osm" "$(STAGED_MODULES_DIR)/$$(basename "$$osm")" || exit 1; done; \
	$(MAKE_MANIFEST) --out "$(STAGED_MANIFEST)" --kernel-in "$(KERNEL_IMAGE)" --kernel-out "$(STAGED_KERNEL)" \
	    --kernel-path $(KERNEL_IMAGE_NAME) --security-version $(SECURITY_VERSION) \
	    --module-revision $(MODULE_REVISION) --module-list "$(STAGED_MODULE_LIST)" \
	    $(foreach osm,$(MODULE_OSMS),--artifact 2:$(MODULES_DIR_NAME)/$(notdir $(osm)):$(STAGED_MODULES_DIR)/$(notdir $(osm)) ) \
	    --build-id-out "$(BUILD_ID_FILE)"; \
	build_id=$$(cat "$(BUILD_ID_FILE)" | tr -d ' \r\n'); \
	printf '%s' "$$build_id" | grep -Eq '^[0-9a-f]{64}$$' || { echo "MakeManifest 写出的 BuildId 不是 64 位小写十六进制：$$build_id"; exit 1; }; \
	test -f "$(TRUST_ANCHORS_FILE)" || { echo "缺少信任锚文本 $(TRUST_ANCHORS_FILE)：先构建 $(CORE_DIR)"; exit 1; }; \
	$(SIGN_MANIFEST) --in "$(STAGED_MANIFEST)" --key "$(SIGN_KEY)" --key-id $(KEY_ID) --anchors "$(TRUST_ANCHORS_FILE)"; \
	set_dir="$(KERNEL_SETS_DIR)/$$build_id"; \
	rm -rf "$(KERNEL_SETS_DIR)"; \
	mkdir -p "$$set_dir"; \
	cp "$(STAGED_KERNEL)" "$$set_dir/$(KERNEL_IMAGE_NAME)"; \
	cp "$(STAGED_MANIFEST)" "$$set_dir/$(MANIFEST_NAME)"; \
	cp "$(STAGED_MODULE_LIST)" "$$set_dir/$(MODULE_LIST_NAME)"; \
	mkdir -p "$$set_dir/$(MODULES_DIR_NAME)"; \
	for osm in $(MODULE_OSMS); do cp "$(STAGED_MODULES_DIR)/$$(basename "$$osm")" "$$set_dir/$(MODULES_DIR_NAME)/$$(basename "$$osm")" || exit 1; done; \
	cp "$(STAGED_MANIFEST)" "$$set_dir/Manifest.$(MODULE_REVISION).mft"; \
	cp "$(STAGED_MODULE_LIST)" "$$set_dir/Modules.$(MODULE_REVISION).list"; \
	if [ -n "$(PREVIOUS_MODULE_REVISION)" ]; then \
	    test -n "$(PREVIOUS_REVISION_DIR)" || { echo "给了 PREVIOUS_MODULE_REVISION 却缺 PREVIOUS_REVISION_DIR"; exit 1; }; \
	    cp "$(PREVIOUS_REVISION_DIR)/$(MANIFEST_NAME)" "$$set_dir/Manifest.$(PREVIOUS_MODULE_REVISION).mft" || exit 1; \
	    cp "$(PREVIOUS_REVISION_DIR)/$(MODULE_LIST_NAME)" "$$set_dir/Modules.$(PREVIOUS_MODULE_REVISION).list" || exit 1; \
	fi; \
	kern=$$(wc -c < "$$set_dir/$(KERNEL_IMAGE_NAME)" | tr -d ' '); \
	[ "$$kern" -lt "$$(( $(VOLUME_BYTES) / 2 ))" ] || { echo "内核映像 $$kern 字节，超过卷容量的一半：$(VOLUME_BYTES) 字节"; exit 1; }; \
	printf '%s\n' "$(KERNEL_SET_ROOT)/$$build_id/$(KERNEL_IMAGE_NAME)" > "$(KERNEL_PATH_FILE)"; \
	rm -rf $(VOLUME_ROOT) && mkdir -p $(VOLUME_ROOT); \
	cp -a $(STATIC_DIR)/. $(VOLUME_ROOT)/; \
	find $(VOLUME_ROOT) -name .keep -delete; \
	if [ "$(KERNEL_SETS_BASE)" != "$(STATIC_DIR)" ]; then \
	    rm -rf "$(VOLUME_ROOT)$(KERNEL_SET_ROOT)"; \
	    mkdir -p "$(VOLUME_ROOT)$(KERNEL_SET_ROOT)"; \
	    cp -a "$(KERNEL_SETS_DIR)/." "$(VOLUME_ROOT)$(KERNEL_SET_ROOT)/"; \
	fi; \
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
	@echo "系统卷 $(SYSTEM_VOLUME)：$(VOLUME_BYTES) 字节，内核集路径 $$(cat $(KERNEL_PATH_FILE))，BuildId $$(cat $(BUILD_ID_FILE))"

# 运行期模块夹具卷（G6d 验证）：与正式卷同一套内核与闭包模块，额外把 RUNTIME_MODULES 里的 .osm
# 以清单 Type=2 条目与 Modules.list 条目登记进集目录（列表由全部 .osm 自动生成），再组装一份独立的系统卷。
# 集目录、暂存区与卷根都落在 RUNTIME_OUT_DIR 下，不碰 Static/ 的正式集目录；正式卷配方保持一致，
# 只有显式构建本目标的验证变体才带运行期夹具
$(RUNTIME_VOLUME): $(KERNEL_IMAGE) $(MODULE_OSMS) $(MAKE_MANIFEST) $(SIGN_MANIFEST) $(SIGN_KEY) $(TRUST_ANCHORS_FILE) $(STATIC_FILES) $(STATIC_DIRS) $(KEY_ID_STAMP) Makefile
	@command -v $(MKFS) >/dev/null 2>&1 || { echo "缺少 $(MKFS)（e2fsprogs，需要支持 -d 的版本）"; exit 1; }
	@test -n "$(RUNTIME_MODULES)" -o -n "$(PATCH_MODE)" || { echo "RUNTIME_MODULES 与 PATCH_MODE 都为空：变体卷没有意义"; exit 1; }
	@set -e; \
	stage="$(RUNTIME_OUT_DIR)/KernelSetStage"; root="$(RUNTIME_OUT_DIR)/VolumeRoot"; setbase="$(RUNTIME_OUT_DIR)/SetTree"; \
	rm -rf "$$stage" "$$root" "$$setbase"; \
	mkdir -p "$$stage/$(MODULES_DIR_NAME)" "$(RUNTIME_OUT_DIR)" "$(dir $(RUNTIME_VOLUME))"; \
	for osm in $(MODULE_OSMS); do cp "$$osm" "$$stage/$(MODULES_DIR_NAME)/$$(basename "$$osm")" || exit 1; done; \
	for item in $(RUNTIME_MODULES); do \
	    rel=$${item%%=*}; src=$${item#*=}; \
	    test -n "$$rel" && test -n "$$src" || { echo "RUNTIME_MODULES 项不是 <集内相对路径>=<源文件>：$$item"; exit 1; }; \
	    test -f "$$src" || { echo "缺少运行期模块源文件 $$src"; exit 1; }; \
	    mkdir -p "$$stage/$$(dirname "$$rel")"; \
	    cp "$$src" "$$stage/$$rel" || exit 1; \
	done; \
	if [ -n "$(PATCH_MODE)" ]; then \
	    printf '%s\n' \
	        'import sys,struct' \
	        'mode,path=sys.argv[1],sys.argv[2]' \
	        'data=bytearray(open(path,"rb").read())' \
	        'if mode=="corrupt":' \
	        '    struct.pack_into("<H",data,0x24,0x10)' \
	        'else:' \
	        '    entry=struct.unpack_from("<I",data,0x18)[0]' \
	        '    table=struct.unpack_from("<H",data,0x24)[0]' \
	        '    count=struct.unpack_from("<H",data,0x26)[0]' \
	        '    for index in range(count):' \
	        '        entry_offset=table+index*32' \
	        '        if struct.unpack_from("<I",data,entry_offset+4)[0]!=1: continue' \
	        '        file_offset=struct.unpack_from("<I",data,entry_offset+12)[0]' \
	        '        memory_offset=struct.unpack_from("<I",data,entry_offset+20)[0]' \
	        '        target=file_offset+(entry-memory_offset)' \
	        '        data[target:target+6]=bytes([0xB8,6,0,0,0,0xC3])' \
	        '        break' \
	        'open(path,"wb").write(data)' \
	        | python3 - "$(PATCH_MODE)" "$$stage/$(PATCH_MODULE)" || { echo "PATCH_MODE=$(PATCH_MODE) 打补丁失败：$$stage/$(PATCH_MODULE)"; exit 1; }; \
	fi; \
	artifacts=""; \
	for osm in $(MODULE_OSMS); do artifacts="$$artifacts --artifact 2:$(MODULES_DIR_NAME)/$$(basename "$$osm"):$$stage/$(MODULES_DIR_NAME)/$$(basename "$$osm")"; done; \
	for item in $(RUNTIME_MODULES); do rel=$${item%%=*}; src=$${item#*=}; artifacts="$$artifacts --artifact 2:$$rel:$$src"; done; \
	$(MAKE_MANIFEST) --out "$$stage/$(MANIFEST_NAME)" --kernel-in "$(KERNEL_IMAGE)" --kernel-out "$$stage/$(KERNEL_IMAGE_NAME)" \
	    --kernel-path $(KERNEL_IMAGE_NAME) --security-version $(SECURITY_VERSION) \
	    --module-revision $(MODULE_REVISION) --module-list "$$stage/$(MODULE_LIST_NAME)" \
	    $$artifacts --build-id-out "$(RUNTIME_OUT_DIR)/BuildId"; \
	build_id=$$(cat "$(RUNTIME_OUT_DIR)/BuildId" | tr -d ' \r\n'); \
	printf '%s' "$$build_id" | grep -Eq '^[0-9a-f]{64}$$' || { echo "运行期夹具的 BuildId 不是 64 位小写十六进制：$$build_id"; exit 1; }; \
	$(SIGN_MANIFEST) --in "$$stage/$(MANIFEST_NAME)" --key "$(SIGN_KEY)" --key-id $(KEY_ID) --anchors "$(TRUST_ANCHORS_FILE)"; \
	set_dir="$$setbase$(KERNEL_SET_ROOT)/$$build_id"; \
	mkdir -p "$$set_dir/$(MODULES_DIR_NAME)"; \
	cp "$$stage/$(KERNEL_IMAGE_NAME)" "$$stage/$(MANIFEST_NAME)" "$$stage/$(MODULE_LIST_NAME)" "$$set_dir/"; \
	cp "$$stage/$(MANIFEST_NAME)" "$$set_dir/Manifest.$(MODULE_REVISION).mft"; \
	cp "$$stage/$(MODULE_LIST_NAME)" "$$set_dir/Modules.$(MODULE_REVISION).list"; \
	cp -a "$$stage/$(MODULES_DIR_NAME)/." "$$set_dir/$(MODULES_DIR_NAME)/"; \
	rm -rf "$$root"; mkdir -p "$$root"; \
	cp -a $(STATIC_DIR)/. "$$root/"; \
	find "$$root" -name .keep -delete; \
	rm -rf "$$root$(KERNEL_SET_ROOT)"; \
	mkdir -p "$$root$(KERNEL_SET_ROOT)"; \
	cp -a "$$setbase$(KERNEL_SET_ROOT)/$$build_id" "$$root$(KERNEL_SET_ROOT)/"; \
	printf '%s\n' "$(KERNEL_SET_ROOT)/$$build_id/$(KERNEL_IMAGE_NAME)" > "$(RUNTIME_KERNEL_PATH_FILE)"; \
	find "$$root" -exec touch -h -d @$(SOURCE_DATE_EPOCH) {} +; \
	E2FSPROGS_FAKE_TIME=$(SOURCE_DATE_EPOCH) SOURCE_DATE_EPOCH=$(SOURCE_DATE_EPOCH) TZ=UTC LC_ALL=C \
	    $(MKFS) -t ext4 -b 4096 -q -F -U $(VOLUME_UUID) -E "$(MKFS_OPTIONS)" -d "$$root" "$(RUNTIME_VOLUME)" $(VOLUME_BLOCKS); \
	size=$$(wc -c < "$(RUNTIME_VOLUME)" | tr -d ' '); \
	[ "$$size" = "$(VOLUME_BYTES)" ] || { echo "运行期夹具卷大小 $$size 与约定不符 $(VOLUME_BYTES)"; exit 1; }; \
	rm -rf "$$stage/$(MODULES_DIR_NAME)" "$$root" "$$setbase"; \
	echo "运行期夹具卷 $(RUNTIME_VOLUME)：BuildId $$build_id，运行期模块：$(RUNTIME_MODULES)"

# 校验内核原型、集目录落点、清单重算与签名链：
# - PackImage --verify 核对卷内 .os 的结构与身份，--expect-build-id 要求 .os 内的 BuildId 等于 BuildId 文本
# - MakeManifest --verify 按清单登记的各产物重算 BuildId 与条目（类型、路径、字节数、摘要），
#   并核对 Modules.list 的模块主版本等于 .os 的发布线、修订号非零、条目集合与清单 .osm 条目一一相等
# - SignManifest --verify 按信任锚文本验签
# - 集目录名 = BuildId 文本；清单 BuildId 由重算覆盖，.os 内 BuildId 由 --expect-build-id 覆盖
check: all $(PACK_IMAGE)
	@$(MAKE) -C $(CORE_DIR) check
	@test -f "$(KERNEL_PATH_FILE)" || { echo "check: 缺少 $(KERNEL_PATH_FILE)"; exit 1; }
	@test -f "$(BUILD_ID_FILE)" || { echo "check: 缺少 $(BUILD_ID_FILE)"; exit 1; }
	@test -f "$(TRUST_ANCHORS_FILE)" || { echo "check: 缺少 $(TRUST_ANCHORS_FILE)"; exit 1; }
	@set -e; \
	build_id=$$(cat "$(BUILD_ID_FILE)" | tr -d ' \r\n'); \
	printf '%s' "$$build_id" | grep -Eq '^[0-9a-f]{64}$$' || { echo "check: $(BUILD_ID_FILE) 不是 64 位小写十六进制 BuildId"; exit 1; }; \
	set_file="$(KERNEL_SETS_BASE)$$(cat $(KERNEL_PATH_FILE))"; \
	set_dir="$$(dirname "$$set_file")"; \
	manifest="$$set_dir/$(MANIFEST_NAME)"; \
	module_list="$$set_dir/$(MODULE_LIST_NAME)"; \
	test "$$(basename "$$set_dir")" = "$$build_id" || { echo "check: 集目录名与 $(BUILD_ID_FILE) 的 BuildId 不一致"; exit 1; }; \
	test -f "$$set_file" || { echo "check: 缺少卷内落点 $$set_file"; exit 1; }; \
	test -f "$$manifest" || { echo "check: 缺少签名清单"; exit 1; }; \
	test -f "$$module_list" || { echo "check: 缺少模块加载列表 $(MODULE_LIST_NAME)"; exit 1; }; \
	test -d "$$set_dir/$(MODULES_DIR_NAME)" || { echo "check: 缺少模块目录 $(MODULES_DIR_NAME)/"; exit 1; }; \
	cmp -s "$(STAGED_KERNEL)" "$$set_file" || { echo "check: 卷内落点与暂存的最终 .os 不一致"; exit 1; }; \
	cmp -s "$(STAGED_MANIFEST)" "$$manifest" || { echo "check: 卷内清单与暂存清单不一致"; exit 1; }; \
	cmp -s "$(STAGED_MODULE_LIST)" "$$module_list" || { echo "check: 卷内 Modules.list 与暂存列表不一致"; exit 1; }; \
	test -f "$(SYSTEM_VOLUME)" || { echo "check: 缺少 $(SYSTEM_VOLUME)"; exit 1; }; \
	grep -q "^$(KEY_ID) " "$(TRUST_ANCHORS_FILE)" || { echo "check: 信任锚文本里没有 KeyId $(KEY_ID)"; exit 1; }; \
	$(SIGN_MANIFEST) --verify --in "$$manifest" --key "$(SIGN_KEY)" --key-id $(KEY_ID) --anchors "$(TRUST_ANCHORS_FILE)"; \
	$(PACK_IMAGE) --verify --expect-build-id "$$build_id" "$$set_file"; \
	module_major=$$(od -An -tu2 -j 104 -N 2 "$$set_file" | tr -d ' '); \
	osm_file_count=0; \
	for osm in "$$set_dir/$(MODULES_DIR_NAME)"/*.osm; do \
	    test -f "$$osm" || continue; \
	    name=$$(basename "$$osm"); \
	    printf '%s' "$$name" | grep -Eq '^[A-Z][A-Za-z0-9]*\.[0-9]+\.[0-9]+\.[0-9]+\.osm$$' || { echo "check: 模块文件名不合 <主名>.<major>.<minor>.<patch>.osm：$$name"; exit 1; }; \
	    major=$$(od -An -tu2 -j 102 -N 2 "$$osm" | tr -d ' '); \
	    [ "$$major" = "$$module_major" ] || { echo "check: 模块 $$name 的主版本 $$major 与 .os 发布线 $$module_major 不一致"; exit 1; }; \
	    osm_file_count=$$((osm_file_count + 1)); \
	done; \
	count=$$(od -An -tu2 -j 56 -N 2 "$$manifest" | tr -d ' '); \
	[ "$$count" -ge 2 ] || { echo "check: 清单产物条目少于 .os 与模块加载列表"; exit 1; }; \
	offset=64; index=0; artifacts=""; os_path=""; boot_list_path=""; osm_list_count=0; \
	while [ "$$index" -lt "$$count" ]; do \
	    type=$$(od -An -tu1 -j "$$offset" -N 1 "$$manifest" | tr -d ' '); \
	    path_bytes=$$(od -An -tu2 -j "$$((offset + 1))" -N 2 "$$manifest" | tr -d ' '); \
	    path=$$(dd if="$$manifest" bs=1 skip="$$((offset + 44))" count="$$path_bytes" 2>/dev/null); \
	    if [ "$$type" = "1" ]; then os_path="$$path"; fi; \
	    if [ "$$type" = "3" ]; then boot_list_path="$$path"; fi; \
	    if [ "$$type" = "2" ]; then osm_list_count=$$((osm_list_count + 1)); fi; \
	    artifacts="$$artifacts --artifact $$type:$$path:$$set_dir/$$path"; \
	    offset=$$((offset + 44 + path_bytes)); \
	    index=$$((index + 1)); \
	done; \
	test "$$os_path" = "$(KERNEL_IMAGE_NAME)" || { echo "check: 清单 .os 条目的路径不是 $(KERNEL_IMAGE_NAME)"; exit 1; }; \
	test "$$boot_list_path" = "$(MODULE_LIST_NAME)" || { echo "check: 清单模块加载列表条目的路径不是 $(MODULE_LIST_NAME)"; exit 1; }; \
	test "$$osm_list_count" = "$$osm_file_count" || { echo "check: $(MODULES_DIR_NAME)/ 下有 $$osm_file_count 个 .osm，清单登记 $$osm_list_count 条"; exit 1; }; \
	$(MAKE_MANIFEST) --verify --in "$$manifest" $$artifacts; \
	echo "check: 集目录 $$build_id 与 BuildId 文本、.os 内 BuildId、清单 BuildId 及条目一致，Modules.list 主版本与 .os 发布线一致，清单验签通过（KeyId $(KEY_ID) 在锚表里）"

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
	@printf '%s\n' 'LikesProgramOS：内核集与系统卷' \
	  '  make [all]   构建 $(SYSTEM_VOLUME)，卷根来自 $(STATIC_DIR)/' \
	  '  make kernel  只构建原型内核镜像并复核（身份尚未分配）' \
	  '  make check   复核内核原型、集目录落点、清单重算与签名链' \
	  '  make clean   删除 $(OUT_DIR)/ 与 $(KERNEL_SETS_DIR)/；clean-all 连同 $(CORE_DIR)/ 一起清理' \
	  '  变量：OUT_DIR / VOLUME_UUID / VOLUME_BYTES / SECURITY_VERSION / KEY_ID / MKFS 可覆盖' \
	  '        MODULE_REVISION（模块修订号，默认 1；补丁发布时单调递增，BuildId 不变）' \
	  '        MODULES_SRC_DIR（模块产物目录，默认 ../KernelModules/Out/Modules；缺失时按空集合构建）' \
	  '        PREVIOUS_MODULE_REVISION / PREVIOUS_REVISION_DIR（旧修订快照夹具，测试用）' \
	  '        REVOCATION_FILE（可选吊销记录，非空时进卷内 /LikesProgram/System/Revocation.rvk）' \
	  '        KERNEL_IMAGE（原型内核镜像，变体构建可覆盖）/ TRUST_ANCHORS_FILE（签名锚文本）' \
	  '  内核集落点：$(KERNEL_SET_ROOT)/<BuildId>/，BuildId 写在 $(BUILD_ID_FILE)，路径写在 $(KERNEL_PATH_FILE)'
