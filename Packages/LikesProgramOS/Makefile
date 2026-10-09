# Makefile
#    LikesProgramOS —— 系统本体的构建：内核映像与系统卷镜像
#
#      make [all]     构建系统卷 $(SYSTEM_VOLUME)
#      make kernel    只构建内核映像并复核
#      make clean     删除本包 Out/；clean-all 连同 Core 一起清理
#
#    Static/ 是系统卷根的源：构建期按原样写进卷，各模块产物由本 Makefile 从自己的
#    Out/ 拷进 Static/ 的对应落点；卷内布局见 Docs/Specs/Common/系统卷布局.md
#    .keep 是 Git 的存在标记（空目录不进 Git），组装时被丢掉，不进系统卷

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
# 内核映像在卷内的路径；与布局描述的默认内核路径一致，覆盖时要两侧一起改
KERNEL_PATH   ?= /LikesProgram/System/LikesProgram.os
# 内核映像在 Static 里的落点：卷内路径直接映射到 Static
KERNEL_DEST   := $(STATIC_DIR)$(KERNEL_PATH)
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
# Static 的全部文件：任一变化都重做系统卷
STATIC_FILES  := $(shell find $(STATIC_DIR) -type f 2>/dev/null)

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

# 把内核映像从 Core 的产物目录拷进 Static 的卷内落点；产物刷新时重新拷贝
$(KERNEL_DEST): $(KERNEL_IMAGE) Makefile
	@mkdir -p $(dir $@)
	cp "$(KERNEL_IMAGE)" "$@"
	@echo "内核映像已拷入 $(KERNEL_DEST)"

# 系统卷：卷根副本去掉存在标记后交给 mkfs 就地写入，不挂载、不需要 root
$(SYSTEM_VOLUME): $(KERNEL_DEST) $(STATIC_FILES) Makefile
	@command -v $(MKFS) >/dev/null 2>&1 || { echo "缺少 $(MKFS)（e2fsprogs，需要支持 -d 的版本）"; exit 1; }
	@kern=$$(wc -c < "$(KERNEL_DEST)" | tr -d ' '); \
	[ "$$kern" -lt "$$(( $(VOLUME_BYTES) / 2 ))" ] || { echo "内核映像 $$kern 字节，超过卷容量的一半：$(VOLUME_BYTES) 字节"; exit 1; }
	@rm -rf $(VOLUME_ROOT) && mkdir -p $(VOLUME_ROOT)
	@cp -a $(STATIC_DIR)/. $(VOLUME_ROOT)/
	@find $(VOLUME_ROOT) -name .keep -delete
	$(MKFS) -t ext4 -b 4096 -q -F -U $(VOLUME_UUID) -E "$(MKFS_OPTIONS)" -d $(VOLUME_ROOT) $(SYSTEM_VOLUME) $(VOLUME_BLOCKS)
	@rm -rf $(VOLUME_ROOT)
	@size=$$(wc -c < "$(SYSTEM_VOLUME)" | tr -d ' '); \
	[ "$$size" = "$(VOLUME_BYTES)" ] || { echo "系统卷大小 $$size 与约定不符 $(VOLUME_BYTES)"; exit 1; }
	@echo "系统卷 $(SYSTEM_VOLUME)：$(VOLUME_BYTES) 字节，内核路径 $(KERNEL_PATH)"

# 校验内核映像与卷内落点：内核映像的字段与摘要由 PackImage --verify 复核
check: all
	@$(MAKE) -C $(CORE_DIR) check
	@test -f "$(KERNEL_DEST)" || { echo "check: 缺少卷内落点 $(KERNEL_DEST)"; exit 1; }
	@cmp -s "$(KERNEL_IMAGE)" "$(KERNEL_DEST)" || { echo "check: $(KERNEL_DEST) 与内核产物不一致"; exit 1; }
	@test -f "$(SYSTEM_VOLUME)" || { echo "check: 缺少 $(SYSTEM_VOLUME)"; exit 1; }
	@echo "check: 卷内落点 $(KERNEL_PATH) 与内核产物一致"

# 删除本包产物
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	@rm -rf -- "$(OUT_DIR)"

# 连同内核侧 Core 的产物一起删除
clean-all: clean
	@$(MAKE) -C $(CORE_DIR) clean

# 打印用法
help:
	@printf '%s\n' 'LikesProgramOS：内核映像与系统卷' \
	  '  make [all]   构建 $(SYSTEM_VOLUME)，卷根来自 $(STATIC_DIR)/' \
	  '  make kernel  只构建内核映像并复核' \
	  '  make check   复核内核映像与卷内落点' \
	  '  make clean   删除 $(OUT_DIR)/；clean-all 连同 $(CORE_DIR)/ 一起清理' \
	  '  变量：OUT_DIR / VOLUME_UUID / VOLUME_BYTES / KERNEL_PATH / MKFS 可覆盖' \
	  '  内核路径与布局描述的默认值必须一致：$(KERNEL_PATH)'
