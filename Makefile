# Makefile
#    LikesProgramOS 顶层构建：镜像组装、宿主工具、运行与清理
#
#      make                       构建 Out/LikesProgram.iso 与 Out/LikesProgram.hdd
#      make iso | hdd             只构建其中一件
#      make packages              先建 Tools/Build 的构建期工具，再跑 Packages 下已有的子 Makefile
#      make tools                 只构建 Tools/Build 下的构建期工具（产物在 Tools/Bin，随源码交付）
#      make test-tools            只构建 Tools/Test 下的测试与回归工具（手工执行，不进入默认构建）
#      make run [介质] [模拟器] [形态] [固件]
#                                 启动镜像。四个位置参数都可省，默认 hdd qemu built bios：
#                                   介质：hdd（默认）| iso
#                                   模拟器：qemu（默认）| bochs（Bochs 只支持 BIOS）
#                                   形态：built（默认，直接按磁盘/光盘挂给虚拟机）| usb（挂成 USB 存储，
#                                         模拟把镜像写进 U 盘后的样子）
#                                   固件：bios（默认）| uefi（用 OVMF）
#                                 例：make run iso qemu usb uefi / make run hdd bochs built bios
#                                 usb 形态模拟 4Kn 设备：RUN_USB_BLOCK_BYTES=4096（同一份镜像即可，镜像里已含
#                                 1 MiB 与 8 MiB 两份 ESP；SeaBIOS 不枚举 4Kn USB，此用例必须配 uefi 固件）
#      make run-win [介质] [模拟器] [形态] [固件]
#                                 与 run 同一套位置参数，区别只在画面：run 不开窗口，
#                                 run-win 开本地窗口（QEMU 挑可用的 gtk/sdl，Bochs 用 wx）
#                                 例：make run-win hdd qemu built bios / make run-win iso qemu built uefi
#      make deliverables          构建默认两份镜像与全部验收变体（4Kn、降级、三份吊销/防回滚现场）
#      make variants              只构建验收变体镜像
#      make reproducible-check    两次 clean-all + 全量构建并比对交付物 sha256；
#                                 要求 .cache/Keys/Development.sec 已存在（外部密钥输入，不自动生成）
#                                 日志与两份哈希清单留在 /tmp/repro-check-*
#      make clean                 删除顶层 Out/；make clean-all 连同子包与两类工具的产物
#
#      make check-all [WITH_BOOT=1]
#                                 把验证收成一条命令：宿主侧 9 个检查工具串行执行，输出一张
#                                 “通过/失败 + 用例计数”表；WITH_BOOT=1 再跑十档启动矩阵
#                                 （BIOS 六档：HDD / ISO / HDD 当 U 盘 / ISO 写入 U 盘 × QEMU，
#                                 HDD / CD × Bochs；UEFI 四档：HDD / ISO / U 盘 / 4Kn U 盘 × OVMF；
#                                 32 位 UEFI 固件可用时再加 IA32 提示件档、共十一档），
#                                 每档抽三列：进内核 / 刷新路径 / 诊断编号
#                                 ACCEPT=1 的工业验收另有失败注入、NVMe/4Kn 与吊销/防回滚三档
#                                 证据目录用 EVIDENCE_DIR=<Out 之外的目录> 指定，转成脚本
#                                 的 --evidence；镜像批次与各自的 sha256 会先打到 stdout
#                                 输出落 Out/check/ —— **它是构建产物，不是证据目录**：
#                                 make clean 与 clean-all 会连同 Out/ 一起删掉它，要长期留存的
#                                 记录请把 CHECK_ALL_ARGS 里的 --out 指到 Out/ 之外
#                                 例：make check-all / make check-all WITH_BOOT=1
#                                 目标是脚本 Tools/Test/CheckAll.sh；make 的实参写不成
#                                 “--with-boot”（那是 make 的未知选项），所以 make 侧用
#                                 WITH_BOOT=1，脚本侧等价地接受 --with-boot
#                                 其余参数经 CHECK_ALL_ARGS 透传，如 CHECK_ALL_ARGS="--timeout 120"
#
#    载荷来源：各子包的真实产物优先，缺失时用 Tools/Bin/MakePayloads 生成的占位件
#    （EFI 引导镜像与 ESP 已由 Packages/Baleen/Uefi 真实产出，占位件只在它缺失时顶上；
#    系统卷由 Packages/LikesProgramOS 生成并含内核映像；
#    布局描述由组装器生成；Stub、BaleenCore、UEFI 侧与内核侧 Core 已有阶段性实现，
#    每个占位件都写明身份，构建时会打印实际用到的那一份）

# 顶层产物目录
OUT_DIR       ?= Out
# 占位载荷目录
PAYLOAD_DIR   := $(OUT_DIR)/Payload
# 光盘镜像
ISO           := $(OUT_DIR)/LikesProgram.iso
# 磁盘镜像
HDD           := $(OUT_DIR)/LikesProgram.hdd
# ISO 卷标识
VOLUME_ID     ?= LIKESPROGRAM
# 系统卷在 ISO 内的文件名
SYSTEM_VOLUME_ISO_NAME ?= SystemVolume.img
# Ext4 分区项（0x83）的 LBA 单位；0xEF 项固定按 512 单位写（双 ESP 布局），光盘 ISO9660 块仍为 2048
DISK_SECTOR_BYTES ?= 512
# 描述符段产物目录：头部与各段拆开输出，由组装器回填起止位置后写回
DESC_DIR      ?= Packages/Baleen/Common/Out/Descs

# —— 子包与宿主工具 ——
# 所有子包的 Makefile
SUBPACKAGE_MAKEFILES := $(wildcard Packages/*/Makefile Packages/*/*/Makefile)
# 构建期宿主工具目录：默认构建与镜像组装依赖它们，缺失会直接失败
TOOL_DIRS := Tools/Build/MakeImage Tools/Build/TestInstaller Tools/Build/MakePayloads Tools/Build/PackImage Tools/Build/MakeKeys Tools/Build/MakeManifest Tools/Build/SignManifest Tools/Build/MakeRevocation
# 测试与回归工具目录：手工执行，不进入默认构建
TEST_TOOL_DIRS := Tools/Test/CheckIpl Tools/Test/CheckStub Tools/Test/CheckStorage Tools/Test/CheckExt4 Tools/Test/CheckCrypto Tools/Test/CheckSelect Tools/Test/CheckMemory Tools/Test/CheckRevocation Tools/Test/CheckEfiBlockDevice Tools/Test/RunBootCase
# 维护工具目录：面向介质与状态的运维动作，手工执行，不进入默认构建
MAINTAIN_TOOL_DIRS := Tools/Maintain/FixBootState

# —— 载荷：真实产物优先，缺失时退到占位件 ——
# 一级引导产物目录
IPL_BIN       := Packages/Baleen/Ipl/Out/Bin
# El Torito 引导镜像
BOOT_IMAGE    ?= $(IPL_BIN)/BaleenIPLCd.bin
# 磁盘 MBR
MBR           ?= $(IPL_BIN)/BaleenIPL.bin
# 实模式服务层
STUB          ?= $(firstword $(wildcard Packages/Baleen/Stub/Out/Bin/BaleenStub.bin) $(PAYLOAD_DIR)/BaleenStub.bin)
# 核心阶段
CORE          ?= $(firstword $(wildcard Packages/Baleen/Core/Out/Bin/BaleenCore.bin) $(PAYLOAD_DIR)/BaleenCore.bin)
# EFI 引导镜像
EFI_IMAGE     ?= $(firstword $(wildcard Packages/Baleen/Uefi/Out/Bin/Efi.img) $(PAYLOAD_DIR)/Efi.img)
# IA32（32 位 UEFI）侧的不支持提示件：只有它，没有它 ESP 里就没有 BOOTIA32.EFI
UEFI_IA32     ?= Packages/Baleen/Uefi/Out/Bin/BOOTIA32.EFI
# ESP 分区内容
ESP_IMAGE     ?= $(EFI_IMAGE)
# Ext4 系统卷镜像
SYSTEM_VOLUME ?= $(firstword $(wildcard Packages/LikesProgramOS/Out/LikesProgram.img) $(PAYLOAD_DIR)/SystemVolume.img)
# 内核路径文件：系统卷包构建时把当前内核集的卷内路径写在这里，组装器按它回填布局描述
KERNEL_PATH_FILE := Packages/LikesProgramOS/Out/KernelPath
# 布局描述模板：可选的外部模板，用 LAYOUT=<路径> 显式传入；仓库内没有生产者，
# 缺省时组装器按 KERNEL_PATH_FILE 给出的内核路径生成布局描述并回填位置字段
LAYOUT        ?=

# —— 构建时间戳与签名身份 ——
# 构建时间戳：PE 头（--no-insert-timestamp）、ISO 元数据（MakeImage --timestamp）与
# Ext4 元数据（E2FSPROGS_FAKE_TIME）都取它；固定值保证同一组输入产出同样的字节
SOURCE_DATE_EPOCH ?= 0
export SOURCE_DATE_EPOCH
# 当前签名密钥编号：Core 的锚表、清单签名与 check 用同一个编号
KEY_ID ?= 1
export KEY_ID

# —— 交付物与验收变体 ——
# 默认交付：光盘与整盘镜像
IMAGES := $(ISO) $(HDD)
# 验收变体镜像：CheckAll 默认按这些路径读取（脚本的 --image-* 选项可覆盖）
# 4Kn：同一批载荷按 4096 单位组装分区表；degrade：布局描述置“记录损坏退缺省集”策略位；
# revocation-*：卷内放进对应的吊销记录，分别验证 E-CORE-66 / E-CORE-75 / E-CORE-74
IMAGE_4KN              := $(OUT_DIR)/LikesProgram.4kn.hdd
IMAGE_DEGRADE          := $(OUT_DIR)/LikesProgram.degrade.hdd
IMAGE_REVOCATION_FLOOR := $(OUT_DIR)/LikesProgram.revocation-floor.hdd
IMAGE_REVOCATION_BAD   := $(OUT_DIR)/LikesProgram.revocation-bad.hdd
IMAGE_REVOCATION_KEY   := $(OUT_DIR)/LikesProgram.revocation-key.hdd
VARIANT_IMAGES := $(IMAGE_4KN) $(IMAGE_DEGRADE) $(IMAGE_REVOCATION_FLOOR) $(IMAGE_REVOCATION_BAD) $(IMAGE_REVOCATION_KEY)

# —— 吊销/防回滚变体的输入与中间产物 ——
# 开发期签名密钥：仓库外的外部输入，缺失时由 MakeKeys 生成一次后复用
SIGN_KEY             := .cache/Keys/Development.sec
MAKE_KEYS_BIN        := Tools/Bin/MakeKeys
MAKE_REVOCATION_BIN  := Tools/Bin/MakeRevocation
# 三份吊销记录：防回滚下限、损坏记录、由第二把密钥签发的“吊销第一把密钥”
REVOCATION_FLOOR_FILE := $(OUT_DIR)/Revocation.floor.rvk
REVOCATION_BAD_FILE   := $(OUT_DIR)/Revocation.bad.rvk
REVOCATION_KEY_FILE   := $(OUT_DIR)/Revocation.key.rvk
# 第二把临时开发密钥与它的锚文本（"2 <公钥>"）：与主密钥同级的仓库外持久目录，
# 已有则复用、缺失才生成；clean/clean-all 不删，因此独立 clean 构建仍用同一把临时密钥
ALT_KEYS_DIR     ?= .cache/AltKeys
ALT_ANCHORS_FILE := $(OUT_DIR)/AltAnchors.txt
# 含第二把锚的变体 BaleenCore：独立产物目录，不覆盖默认引导器
REVOCATION_CORE_DIR := $(abspath $(OUT_DIR)/RevokeCore)
REVOCATION_CORE     := $(REVOCATION_CORE_DIR)/Bin/BaleenCore.bin
# 变体系统卷：同一份内核集内容，只换卷内吊销记录；独立产物目录，互不覆盖
REVOCATION_FLOOR_VOLUME := $(OUT_DIR)/RevocationFloorVolume/LikesProgram.img
REVOCATION_BAD_VOLUME   := $(OUT_DIR)/RevocationBadVolume/LikesProgram.img
REVOCATION_KEY_VOLUME   := $(OUT_DIR)/RevocationKeyVolume/LikesProgram.img
# 默认内核映像：变体系统卷与默认构建共用同一份内核产物（布局描述仍由 KERNEL_PATH_FILE 回填）
KERNEL_IMAGE_DEFAULT := Packages/LikesProgramOS/Core/Out/Bin/LikesProgram.os

# —— 启动 ——
# 模拟器
QEMU         ?= qemu-system-x86_64
# 虚拟机内存（MiB）
QEMU_MEM     ?= 512
# 默认不开图形窗口；要窗口就 QEMU_DISPLAY= 清空该变量
QEMU_DISPLAY ?= -display none
# 追加给 QEMU 的参数
QEMU_EXTRA   ?=
# OVMF 固件
OVMF_CODE    ?= /usr/share/OVMF/OVMF_CODE_4M.fd
# OVMF 变量存储
OVMF_VARS    ?= /usr/share/OVMF/OVMF_VARS_4M.fd
BOCHS        ?= bochs
# Bochs 的 BIOS ROM：发行版可能缺专用 ROM，或只留下 VGABIOS 的悬空链接
# 用可读文件选择回退项，仍可显式覆盖
BOCHS_SHARE  ?= /usr/share/bochs
BOCHS_ROM    ?= $(shell for rom in "$(BOCHS_SHARE)/BIOS-bochs-latest" /usr/share/seabios/bios-256k.bin; do if test -r "$$rom"; then printf '%s\n' "$$rom"; break; fi; done)
BOCHS_VGAROM ?= $(shell for rom in "$(BOCHS_SHARE)/VGABIOS-lgpl-latest" /usr/share/seabios/vgabios-bochs-display.bin; do if test -r "$$rom"; then printf '%s\n' "$$rom"; break; fi; done)
# 0 让 Bochs 按实际 ROM 大小计算地址，避免根据文件名猜测
BOCHS_ROM_ADDR ?= 0
# 虚拟机内存（MiB）
BOCHS_MEM    ?= 128
# rfb 无需图形会话；timeout=0 不等待 VNC 客户端，端口由 RFB 自动选择（从 5900 起）
# wx 开本地窗口，且要求配置界面也是 wx，否则 Bochs 会退回 rfb；run-win 已把两者一起设好
BOCHS_DISPLAY ?= rfb
# 配置界面：textconfig 无需图形会话，wx 是本地窗口路径要求的那一个
BOCHS_CONFIG_INTERFACE ?= textconfig
# RFB 类显示库的附加选项
BOCHS_DISPLAY_OPTIONS ?= $(if $(filter rfb vncsrv,$(BOCHS_DISPLAY)),timeout=0,)
# make 的 $(if ...) 里逗号是参数分隔符，要输出字面逗号得借道一个变量
comma := ,
# Bochs 配置里的 display_library 行
BOCHS_DISPLAY_LINE = display_library: $(BOCHS_DISPLAY)$(if $(BOCHS_DISPLAY_OPTIONS),$(comma) options="$(BOCHS_DISPLAY_OPTIONS)")

# —— run 的位置参数：make run <hdd|iso> <qemu|bochs> <built|usb> <bios|uefi> ——
# 取第 2 个及之后的全部目标词，多给的参数由 run-check 拦下（否则 make 会先跑 run 再报未知目标）
RUN_GIVEN    := $(wordlist 2,9,$(MAKECMDGOALS))
# 介质：hdd 或 iso
RUN_IMAGE    := $(firstword $(filter hdd iso,$(RUN_GIVEN)) hdd)
# 模拟器：qemu 或 bochs
RUN_EMU      := $(firstword $(filter qemu bochs,$(RUN_GIVEN)) qemu)
# 形态：built 或 usb
RUN_MODE     := $(firstword $(filter built usb,$(RUN_GIVEN)) built)
# 固件：bios 或 uefi
RUN_FIRMWARE := $(firstword $(filter bios uefi,$(RUN_GIVEN)) bios)
# USB 形态的设备块大小：512（默认）或 4096（4Kn）；4Kn 需要按 4096 单位组装的镜像
# 且 SeaBIOS 不枚举 4Kn USB 存储，只有 UEFI 固件能看到该设备
RUN_USB_BLOCK_BYTES ?= 512
# 多给的位置参数，由 run-check 报错
RUN_EXTRA    := $(filter-out hdd iso qemu bochs built usb bios uefi,$(RUN_GIVEN))
# 本次要启动的镜像
ifeq ($(RUN_IMAGE),iso)
RUN_TARGET_IMAGE := $(ISO)
else
RUN_TARGET_IMAGE := $(HDD)
endif

# 形态：built 直接按光盘/磁盘挂载；usb 挂成 USB 存储（模拟写入 U 盘后的样子）
# 挂给 QEMU 的介质参数
ifeq ($(RUN_MODE),usb)
# QEMU 要求物理块不小于逻辑块：4Kn 时两个一起写 4096
QEMU_USB_BLOCK := $(if $(filter 4096,$(RUN_USB_BLOCK_BYTES)),$(comma)logical_block_size=4096$(comma)physical_block_size=4096)
QEMU_MEDIA := -device usb-ehci,id=ehci -drive if=none,id=usbstick,format=raw,file=$(RUN_TARGET_IMAGE) -device usb-storage,bus=ehci.0,drive=usbstick$(QEMU_USB_BLOCK) -boot order=c
else ifeq ($(RUN_IMAGE),iso)
QEMU_MEDIA := -cdrom $(RUN_TARGET_IMAGE) -boot d
else
QEMU_MEDIA := -drive file=$(RUN_TARGET_IMAGE),format=raw,if=ide -boot c
endif

# 固件参数：uefi 时挂 OVMF
ifeq ($(RUN_FIRMWARE),uefi)
QEMU_FIRMWARE := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) -drive if=pflash,format=raw,file=$(OUT_DIR)/OVMF_VARS.fd
else
QEMU_FIRMWARE :=
endif
# 两条固件路径的可见输出都走 COM1：EFI 应用、Bochs 的 mode=file、以及以后的真实内核都能写它
QEMU_CONSOLE := -serial stdio
# QEMU 只允许一个字符设备占用 stdio：串口占了 stdio，调试口（0xE9）默认写文件
QEMU_DEBUGCON ?= -debugcon file:$(OUT_DIR)/debugcon.log

# —— run-win：与 run 同一套位置参数，只把无窗口画面换成本地窗口 ——
# QEMU 的显示后端清单；只有 run-win 且真跑 QEMU 时才问一次，别的目标不白起进程
QEMU_DISPLAY_HELP := $(if $(filter run-win,$(MAKECMDGOALS)),$(if $(filter qemu,$(RUN_EMU)),$(shell $(QEMU) -display help 2>/dev/null)))
# QEMU 可用的本地窗口后端：gtk 优先，其次 sdl
QEMU_WIN_BACKEND ?= $(firstword $(filter gtk sdl,$(QEMU_DISPLAY_HELP)))
# run-win 下 QEMU 的显示参数；留空说明没有可用的本地窗口后端，由 run-check 拦下
QEMU_DISPLAY_WIN ?= $(if $(QEMU_WIN_BACKEND),-display $(QEMU_WIN_BACKEND))
# run-win 下 Bochs 的配置界面与显示库
BOCHS_CONFIG_INTERFACE_WIN ?= wx
BOCHS_DISPLAY_WIN ?= wx
# 本次是不是 run-win
RUN_WINDOWED := $(filter run-win,$(MAKECMDGOALS))
# 本次入口的名字，出现在打印与报错里
RUN_LABEL := $(if $(RUN_WINDOWED),run-win,run)

.DEFAULT_GOAL := all
.PHONY: all images deliverables variants iso hdd packages tools core-generated test-tools maintain-tools compile-db payloads payload-report run run-win run-check run-uefi check-all reproducible-check clean clean-all help qemu bochs built usb bios uefi

# 默认目标
all: images

# 两份镜像
images: $(ISO) $(HDD)

# 全部交付物：默认两份镜像 + 验收变体
deliverables: images variants

# 验收变体镜像：4Kn、降级策略位与三份吊销/防回滚现场
variants: $(VARIANT_IMAGES)

# 只构建光盘镜像
iso: $(ISO)
# 只构建磁盘镜像
hdd: $(HDD)

# run 的实参：作为目标出现时不做任何事（hdd/iso 本身是要构建的镜像目标）
qemu bochs built usb bios uefi: ;

# 跑 Packages 下已有的子 Makefile
# 先建 Tools/Build 的构建期工具：子包会在构建途中用它们，并行构建同一个工具会互相踩
packages: tools
	@for mk in $(SUBPACKAGE_MAKEFILES); do echo "== $(MAKE) -C $$(dirname $$mk)"; $(MAKE) -C "$$(dirname $$mk)" || exit 1; done

# 构建 Tools/Build 下的构建期工具
tools:
	@for dir in $(TOOL_DIRS); do echo "== $(MAKE) -C $$dir"; $(MAKE) -C "$$dir" || exit 1; done

# Core 的生成物（SecurityFloor.hpp / TrustAnchors.cpp）是部分宿主测试的编译输入：
# 干净树上先让 Core 判断生成，再编测试工具；Tools/Build 工具先建好，Core 侧缺工具时不再各自抢建
core-generated: tools
	@$(MAKE) -C Packages/Baleen/Core

# 构建 Tools/Test 下的测试与回归工具：手工执行，不进入默认构建
test-tools: core-generated
	@for dir in $(TEST_TOOL_DIRS); do echo "== $(MAKE) -C $$dir"; $(MAKE) -C "$$dir" || exit 1; done

# 构建 Tools/Maintain 下的维护工具：手工执行，不进入默认构建
maintain-tools:
	@for dir in $(MAINTAIN_TOOL_DIRS); do echo "== $(MAKE) -C $$dir"; $(MAKE) -C "$$dir" || exit 1; done

# —— 验证：一条命令跑完宿主侧全部检查，可选追加启动矩阵 ——
# 前提：test-tools 先把 9 个检查工具建到 Tools/Bin；WITH_BOOT=1 时再建出默认镜像与全部验收变体
# （4Kn / 降级 / 吊销三档都在依赖里：脚本默认按 Out/ 下的变体路径读镜像）
# 参数经 CHECK_ALL_ARGS 透传，如 CHECK_ALL_ARGS="--timeout 120"
# EVIDENCE_DIR=<Out 之外的目录> 转成脚本的 --evidence，把验证记录留到构建产物之外
# ACCEPT=1 是工业验收：启动矩阵与状态演练之外再跑失败注入、4Kn 与 NVMe 控制器、
# 引导状态机多轮与吊销/防回滚三档
EVIDENCE_DIR ?= $(EVIDENCE)
CHECK_ALL_DEPS := test-tools maintain-tools \
                  $(if $(filter 1,$(WITH_BOOT)$(ACCEPT)),images $(VARIANT_IMAGES))
CHECK_ALL_ARGS ?=

check-all: $(CHECK_ALL_DEPS)
	@if [ -n "$(filter 1,$(WITH_BOOT)$(ACCEPT))" ]; then \
	    printf 'check-all 镜像批次（本次 invocation 实际使用）：\n'; \
	    for image in $(IMAGES) $(VARIANT_IMAGES); do \
	        if [ -f "$$image" ]; then \
	            printf '  %s  %s\n' "$$(sha256sum "$$image" | cut -d' ' -f1)" "$$image"; \
	        else \
	            printf '  缺失  %s\n' "$$image"; \
	        fi; \
	    done; \
	fi
	@Tools/Test/CheckAll.sh $(if $(filter 1,$(WITH_BOOT)$(ACCEPT)),--with-boot,) $(if $(filter 1,$(ACCEPT)),--accept,) $(if $(EVIDENCE_DIR),--evidence $(EVIDENCE_DIR),) $(CHECK_ALL_ARGS)

# 可复现构建检查：要求两把外部密钥输入都已存在（主密钥与第二把临时密钥，均不自动生成随机密钥），
# 由脚本做两次 clean-all + 全量构建并比对交付物 sha256；日志与两份哈希清单留在 /tmp
reproducible-check:
	@test -f "$(SIGN_KEY)" || { echo "reproducible-check: 可复现构建需要外部密钥输入，请先准备 $(SIGN_KEY)（缺失时构建会自动生成随机密钥，不可复现）"; exit 1; }
	@test -f "$(ALT_KEYS_DIR)/Development.sec" || { echo "reproducible-check: 可复现构建需要外部密钥输入，请先准备 $(ALT_KEYS_DIR)/Development.sec（第二把临时密钥；缺失时构建会自动生成随机密钥，不可复现）"; exit 1; }
	@Tools/Test/CheckReproducible.sh $(if $(EVIDENCE_DIR),--evidence $(EVIDENCE_DIR),)

# 生成 compile_commands.json：扫描子包与工具的编译命令，供 clangd 与编辑器取用
compile-db:
	@Tools/Build/MakeCompileDb.sh

# 先建子包（其依赖已包含 Tools/Build 的构建期工具），再补齐缺失的占位载荷
# IA32 提示件：UEFI 包产出后放进载荷目录，MakePayloads 会把它装进 ESP 的 \EFI\BOOT\BOOTIA32.EFI；
# 缺失时载荷目录里没有它，ESP 的回退路径与以前一致（只有 BOOTX64.EFI）
# 载荷目录得先建：MakePayloads 自己会建，但拷贝发生在它之前，clean-all 之后这里还不存在
payloads: packages
	@mkdir -p "$(PAYLOAD_DIR)"
	@if [ -f "$(UEFI_IA32)" ]; then cp "$(UEFI_IA32)" "$(PAYLOAD_DIR)/BOOTIA32.EFI"; fi
	@Tools/Bin/MakePayloads "$(PAYLOAD_DIR)"

# 组装整盘镜像的命令：默认镜像与验收变体只有输出路径、内核、扇区单位、系统卷与附加开关不同
# 用法：$(call assemble-hdd,<输出镜像>,<core>,<sector-bytes>,<system-volume>,<附加选项>)
# 构建清单写到 <输出镜像>.json，变体因此各有自己的 .json
define assemble-hdd
	@kernel_arg=; \
	[ ! -f "$(KERNEL_PATH_FILE)" ] || kernel_arg="--kernel-path $$(cat $(KERNEL_PATH_FILE))"; \
	Tools/Bin/TestInstaller --out $(1) --mbr $(MBR) --stub $(STUB) --core $(2) \
	    --descs $(DESC_DIR) --sector-bytes $(3) --esp $(ESP_IMAGE) --system-volume $(4) \
	    $(if $(LAYOUT),--layout $(LAYOUT),) $$kernel_arg $(5) --manifest $(1).json
endef

# 组装通用镜像（MakeImage）：一份字节供刻录光盘、写入 U 盘 / 移动硬盘与虚拟机挂载
$(ISO): payloads
	@$(MAKE) --no-print-directory payload-report
	@kernel_arg=; \
	[ ! -f "$(KERNEL_PATH_FILE)" ] || kernel_arg="--kernel-path $$(cat $(KERNEL_PATH_FILE))"; \
	Tools/Bin/MakeImage --out $@ --boot-image $(BOOT_IMAGE) --mbr $(MBR) \
	    --stub $(STUB) --core $(CORE) --descs $(DESC_DIR) --efi $(EFI_IMAGE) --esp $(ESP_IMAGE) \
	    --system-volume $(SYSTEM_VOLUME) --iso-name $(SYSTEM_VOLUME_ISO_NAME) \
	    --volume-id $(VOLUME_ID) --sector-bytes $(DISK_SECTOR_BYTES) --timestamp $(SOURCE_DATE_EPOCH) \
	    $(if $(LAYOUT),--layout $(LAYOUT),) $$kernel_arg \
	    --manifest $@.json

# 组装整盘镜像（TestInstaller，测试安装器）：供写入内置硬盘 / 移动硬盘 / U 盘
$(HDD): payloads
	@$(MAKE) --no-print-directory payload-report
	$(call assemble-hdd,$@,$(CORE),$(DISK_SECTOR_BYTES),$(SYSTEM_VOLUME),)

# —— 验收变体镜像 ——
# 每个变体都是独立的文件目标，配方直接调用 TestInstaller，不递归 make 的 hdd 目标：
# 依赖 payloads（phony）保证载荷/工具更新后一定重建，不会“文件存在就复用”；
# 变体自己的输入（系统卷、变体 Core）也在依赖里
$(IMAGE_4KN): payloads
	@$(MAKE) --no-print-directory payload-report
	$(call assemble-hdd,$@,$(CORE),4096,$(SYSTEM_VOLUME),)

$(IMAGE_DEGRADE): payloads
	@$(MAKE) --no-print-directory payload-report
	$(call assemble-hdd,$@,$(CORE),$(DISK_SECTOR_BYTES),$(SYSTEM_VOLUME),--degrade-without-record)

$(IMAGE_REVOCATION_FLOOR): payloads $(REVOCATION_FLOOR_VOLUME)
	@$(MAKE) --no-print-directory payload-report
	$(call assemble-hdd,$@,$(CORE),$(DISK_SECTOR_BYTES),$(REVOCATION_FLOOR_VOLUME),)

$(IMAGE_REVOCATION_BAD): payloads $(REVOCATION_BAD_VOLUME)
	@$(MAKE) --no-print-directory payload-report
	$(call assemble-hdd,$@,$(CORE),$(DISK_SECTOR_BYTES),$(REVOCATION_BAD_VOLUME),)

$(IMAGE_REVOCATION_KEY): payloads $(REVOCATION_KEY_VOLUME) $(REVOCATION_CORE)
	@$(MAKE) --no-print-directory payload-report
	$(call assemble-hdd,$@,$(REVOCATION_CORE),$(DISK_SECTOR_BYTES),$(REVOCATION_KEY_VOLUME),)

# —— 吊销/防回滚变体的输入与中间产物 ——
# 这些目标直接调用组装工具与子包 Makefile，不递归本文件的 hdd 目标（否则文件目标会绕圈）。
# 它们全部依赖 payloads：工具与开发期密钥先由 packages 建好/生成，这里再并行生成会踩同一份输入
# 开发期签名密钥：仓库外的外部输入；缺失时生成一次，之后复用（不换锚）
$(SIGN_KEY): payloads
	@$(MAKE_KEYS_BIN) --out $(dir $(SIGN_KEY))

# 防回滚下限：min-security-version 1000 远高于清单的安全版本，装载前必报 E-CORE-66
$(REVOCATION_FLOOR_FILE): $(SIGN_KEY)
	@mkdir -p $(OUT_DIR)
	$(MAKE_REVOCATION_BIN) --key $(SIGN_KEY) --key-id $(KEY_ID) --epoch 1 --min-security-version 1000 --out $@

# 损坏的吊销记录：16 个零字节，结构检查失败 → E-CORE-75 WARN 后继续启动
$(REVOCATION_BAD_FILE): payloads
	@mkdir -p $(OUT_DIR)
	@dd if=/dev/zero of=$@ bs=16 count=1 status=none

# 第二把临时开发密钥与它的锚文本（"2 <公钥>"）：与主密钥一样是仓库外的外部输入；
# 锚文本由密钥派生，所以依赖种子文件：种子被换掉时锚表与吊销记录都跟着重建
$(ALT_KEYS_DIR)/Development.sec: payloads
	@mkdir -p $(ALT_KEYS_DIR)
	$(MAKE_KEYS_BIN) --out $(ALT_KEYS_DIR)

$(ALT_ANCHORS_FILE): $(ALT_KEYS_DIR)/Development.sec
	$(MAKE_KEYS_BIN) --out $(ALT_KEYS_DIR) --key-id 2 --anchor-text $@

# 由第二把密钥签名、吊销第一把密钥的吊销记录 → 清单验签时必报 E-CORE-74
$(REVOCATION_KEY_FILE): $(ALT_ANCHORS_FILE)
	@mkdir -p $(OUT_DIR)
	$(MAKE_REVOCATION_BIN) --key $(ALT_KEYS_DIR)/Development.sec --key-id 2 --revoke-key-id 1 --epoch 1 --out $@

# 变体 BaleenCore：锚表含主密钥与第二把临时密钥（ALT_ANCHORS_FILE 作为附加锚输入）
# 产物目录独立；子 make 自己判断源码与锚表变化后是否重编，上游对象复用默认构建的结果
$(REVOCATION_CORE): payloads $(ALT_ANCHORS_FILE)
	@$(MAKE) --no-print-directory -C Packages/Baleen/Core \
	    LOCAL_OUT_DIR=$(REVOCATION_CORE_DIR) ANCHORS_FILE=$(abspath $(ALT_ANCHORS_FILE)) KEY_ID=$(KEY_ID)

# 变体系统卷：把吊销记录放进卷内 /LikesProgram/System/Revocation.rvk 后再 mke2fs；
# 三个卷共用 Static/ 下的内核集目录，按依赖串行构建，避免互相删建
$(REVOCATION_FLOOR_VOLUME): payloads $(REVOCATION_FLOOR_FILE)
	@$(MAKE) --no-print-directory -C Packages/LikesProgramOS \
	    OUT_DIR=$(abspath $(dir $(REVOCATION_FLOOR_VOLUME))) \
	    KERNEL_IMAGE=$(abspath $(KERNEL_IMAGE_DEFAULT)) \
	    REVOCATION_FILE=$(abspath $(REVOCATION_FLOOR_FILE))

$(REVOCATION_BAD_VOLUME): $(REVOCATION_FLOOR_VOLUME) $(REVOCATION_BAD_FILE)
	@$(MAKE) --no-print-directory -C Packages/LikesProgramOS \
	    OUT_DIR=$(abspath $(dir $(REVOCATION_BAD_VOLUME))) \
	    KERNEL_IMAGE=$(abspath $(KERNEL_IMAGE_DEFAULT)) \
	    REVOCATION_FILE=$(abspath $(REVOCATION_BAD_FILE))

$(REVOCATION_KEY_VOLUME): $(REVOCATION_BAD_VOLUME) $(REVOCATION_KEY_FILE)
	@$(MAKE) --no-print-directory -C Packages/LikesProgramOS \
	    OUT_DIR=$(abspath $(dir $(REVOCATION_KEY_VOLUME))) \
	    KERNEL_IMAGE=$(abspath $(KERNEL_IMAGE_DEFAULT)) \
	    REVOCATION_FILE=$(abspath $(REVOCATION_KEY_FILE))

# 打印实际用到的是真实产物还是占位件，以及组装器回填布局描述用的内核路径来源
payload-report:
	@printf '载荷：\n'
	@printf '  引导镜像  %s\n' "$(BOOT_IMAGE)"
	@printf '  MBR       %s\n' "$(MBR)"
	@printf '  Stub      %s%s\n' "$(STUB)" "$(if $(findstring $(PAYLOAD_DIR),$(STUB)),  <- 占位件,)"
	@printf '  BaleenCore %s%s\n' "$(CORE)" "$(if $(findstring $(PAYLOAD_DIR),$(CORE)),  <- 占位件,)"
	@printf '  EFI 镜像  %s%s\n' "$(EFI_IMAGE)" "$(if $(findstring $(PAYLOAD_DIR),$(EFI_IMAGE)),  <- 占位件,)"
	@printf '  系统卷    %s%s\n' "$(SYSTEM_VOLUME)" "$(if $(findstring $(PAYLOAD_DIR),$(SYSTEM_VOLUME)),  <- 占位件,)"
	@printf '  内核路径  %s\n' "$(KERNEL_PATH_FILE)"
	@if [ -f "$(KERNEL_PATH_FILE)" ]; then \
	    printf '            %s\n' "$$(cat $(KERNEL_PATH_FILE))"; \
	else \
	    printf '            未提供（组装器将拒绝或回退并警告）\n'; \
	fi
	@printf '  布局模板  %s\n' "$(if $(LAYOUT),$(LAYOUT)（外部模板）,无（组装器按 $(KERNEL_PATH_FILE) 的路径生成）)"


# —— 启动：make run / run-win <hdd|iso> <qemu|bochs> <built|usb> <bios|uefi> ——
# 检查本次运行的参数组合与外部依赖
run-check:
	@if [ -n "$(RUN_EXTRA)" ]; then echo "$(RUN_LABEL): 无法识别的参数 $(RUN_EXTRA)；用法 make $(RUN_LABEL) <hdd|iso> <qemu|bochs> <built|usb> <bios|uefi>" >&2; exit 2; fi
	@if [ "$(RUN_EMU)" = bochs ] && [ "$(RUN_MODE)" = usb ]; then echo "$(RUN_LABEL): Bochs 没有 USB 存储仿真；usb 形态请用 qemu" >&2; exit 2; fi
	@if [ "$(RUN_EMU)" = bochs ] && [ "$(RUN_FIRMWARE)" = uefi ]; then echo "$(RUN_LABEL): Bochs 侧只支持 BIOS 路径，uefi 固件请用 qemu" >&2; exit 2; fi
	@if [ "$(RUN_MODE)" = usb ] && [ "$(RUN_USB_BLOCK_BYTES)" = 4096 ] && [ "$(RUN_FIRMWARE)" = bios ]; then echo "$(RUN_LABEL): 提示：SeaBIOS 不枚举 4Kn USB 存储，BIOS 路径看不到该设备（固件限制，不是镜像问题）；4Kn 用例请配 uefi 固件" >&2; fi
	@if [ "$(RUN_EMU)" = bochs ]; then \
	    test -r "$(BOCHS_ROM)" || { echo "$(RUN_LABEL): 缺少 BIOS ROM，可用 BOCHS_ROM= 指定" >&2; exit 1; }; \
	    test -r "$(BOCHS_VGAROM)" || { echo "$(RUN_LABEL): 缺少 VGA ROM，可用 BOCHS_VGAROM= 指定" >&2; exit 1; }; \
	fi
	@if [ -n "$(RUN_WINDOWED)" ] && [ "$(RUN_EMU)" = qemu ] && [ -z "$(QEMU_DISPLAY_WIN)" ]; then \
	    echo "$(RUN_LABEL): $(QEMU) 没有 gtk/sdl 本地窗口后端；装 qemu-system-gui，或用 QEMU_DISPLAY_WIN= 指定别的显示方式" >&2; exit 1; \
	fi
	@if [ -n "$(RUN_WINDOWED)" ] && [ -z "$$DISPLAY" ] && [ -z "$$WAYLAND_DISPLAY" ]; then \
	    echo "$(RUN_LABEL): 当前会话没有图形环境（DISPLAY 与 WAYLAND_DISPLAY 都为空），开不了本地窗口；无窗口改用 make run" >&2; exit 1; \
	fi

# QEMU 路径：挂镜像并直接启动
ifeq ($(RUN_EMU),qemu)
run: run-check $(RUN_TARGET_IMAGE)
	@echo "$(RUN_LABEL): qemu / $(RUN_IMAGE) / $(RUN_MODE) / $(RUN_FIRMWARE)$(if $(RUN_WINDOWED), / 显示 $(QEMU_DISPLAY_WIN))"
ifeq ($(RUN_FIRMWARE),uefi)
	@test -f "$(OVMF_CODE)" || { echo "$(RUN_LABEL): 缺少 OVMF（$(OVMF_CODE)），可用 OVMF_CODE= 指定" >&2; exit 1; }
	@mkdir -p $(OUT_DIR) && cp "$(OVMF_VARS)" $(OUT_DIR)/OVMF_VARS.fd
endif
	$(QEMU) -machine q35 -m $(QEMU_MEM) $(QEMU_DISPLAY) $(QEMU_FIRMWARE) $(QEMU_MEDIA) $(QEMU_CONSOLE) $(QEMU_DEBUGCON) $(QEMU_EXTRA)
else
# Bochs 路径：按本次介质生成 bochsrc 再启动
run: run-check $(RUN_TARGET_IMAGE) $(OUT_DIR)/bochsrc.$(RUN_IMAGE)
	@echo "$(RUN_LABEL): bochs / $(RUN_IMAGE) / $(RUN_MODE) / bios"
	@echo "$(RUN_LABEL): 串口日志 $(OUT_DIR)/bochs-$(RUN_IMAGE).serial.log；画面走 $(BOCHS_DISPLAY)$(if $(BOCHS_DISPLAY_OPTIONS),（$(BOCHS_DISPLAY_OPTIONS)）)"
	@echo "$(RUN_LABEL): BIOS ROM $(BOCHS_ROM)；VGA ROM $(BOCHS_VGAROM)"
	@printf 'continue\n' > "$(OUT_DIR)/bochs-continue.rc"
	@if $(BOCHS) --help 2>&1 | grep -q -- '-rc '; then \
	    $(BOCHS) -q -f "$(OUT_DIR)/bochsrc.$(RUN_IMAGE)" -rc "$(OUT_DIR)/bochs-continue.rc"; \
	else \
	    $(BOCHS) -q -f "$(OUT_DIR)/bochsrc.$(RUN_IMAGE)"; \
	fi
endif

# run-win：位置参数与 run 完全相同，只把画面换成本地窗口
# target-specific 变量沿前提链生效：run 的启动命令与 bochsrc.* 的生成都取窗口值
run-win: QEMU_DISPLAY := $(QEMU_DISPLAY_WIN)
run-win: BOCHS_DISPLAY := $(BOCHS_DISPLAY_WIN)
run-win: BOCHS_CONFIG_INTERFACE := $(BOCHS_CONFIG_INTERFACE_WIN)
run-win: run

# 便捷别名：UEFI 光盘启动
run-uefi:
	@$(MAKE) --no-print-directory run iso qemu built uefi

# 每次重新生成以应用命令行/环境覆盖，避免复用旧 ROM 或显示配置
.PHONY: FORCE
FORCE:

# 光盘的 Bochs 配置：BIOS ROM 缺失时用 SeaBIOS 回退
$(OUT_DIR)/bochsrc.iso: Makefile FORCE
	@mkdir -p $(OUT_DIR)
	@printf '%s\n' \
	    'romimage: file="$(BOCHS_ROM)", address=$(BOCHS_ROM_ADDR)' \
	    'vgaromimage: file="$(BOCHS_VGAROM)"' \
	    'megs: $(BOCHS_MEM)' \
	    'config_interface: $(BOCHS_CONFIG_INTERFACE)' \
	    'sound: driver=dummy' \
	    'speaker: enabled=0' \
	    'ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14' \
	    'ata0-master: type=cdrom, path="$(ISO)", status=inserted' \
	    'boot: cdrom' \
	    '$(BOCHS_DISPLAY_LINE)' \
	    'com1: enabled=1, mode=file, dev="$(OUT_DIR)/bochs-iso.serial.log"' \
	    'log: /dev/null' \
	    'panic: action=fatal' \
	    > $@

# 磁盘的 Bochs 配置：BIOS ROM 缺失时用 SeaBIOS 回退
$(OUT_DIR)/bochsrc.hdd: Makefile FORCE
	@mkdir -p $(OUT_DIR)
	@printf '%s\n' \
	    'romimage: file="$(BOCHS_ROM)", address=$(BOCHS_ROM_ADDR)' \
	    'vgaromimage: file="$(BOCHS_VGAROM)"' \
	    'megs: $(BOCHS_MEM)' \
	    'config_interface: $(BOCHS_CONFIG_INTERFACE)' \
	    'sound: driver=dummy' \
	    'speaker: enabled=0' \
	    'ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14' \
	    'ata0-master: type=disk, path="$(HDD)", mode=flat' \
	    'boot: disk' \
	    '$(BOCHS_DISPLAY_LINE)' \
	    'com1: enabled=1, mode=file, dev="$(OUT_DIR)/bochs-hdd.serial.log"' \
	    'log: /dev/null' \
	    'panic: action=fatal' \
	    > $@

# 删除顶层产物
clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OUT_DIR)

# 连同子包与两类工具的产物一起删除
clean-all: clean
	@for mk in $(SUBPACKAGE_MAKEFILES); do $(MAKE) -C "$$(dirname $$mk)" clean || exit 1; done
	@for dir in $(TOOL_DIRS) $(TEST_TOOL_DIRS) $(MAINTAIN_TOOL_DIRS); do $(MAKE) -C "$$dir" clean || exit 1; done

# 打印用法
help:
	@printf '%s\n' \
	    'LikesProgramOS 顶层构建' \
	    '  make                   构建 $(ISO) 与 $(HDD)' \
	    '  make iso | hdd         只构建其中一件' \
	    '  make packages|tools|test-tools' \
	    '                         只跑子包、只建 Tools/Build 的构建期工具或 Tools/Test 的测试工具' \
	    '  make compile-db        扫描各包写出 compile_commands.json（clangd 与编辑器用）' \
	    '  make run [介质] [模拟器] [形态] [固件]' \
	    '                         介质 hdd|iso（默认 hdd）；模拟器 qemu|bochs（默认 qemu，Bochs 只支持 BIOS）；' \
	    '                         形态 built|usb（默认 built）；固件 bios|uefi（默认 bios，uefi 用 OVMF）' \
	    '                         usb 形态可配 RUN_USB_BLOCK_BYTES=4096 模拟 4Kn 设备（同一份镜像即可，已含两份 ESP）' \
	    '                         SeaBIOS 不枚举 4Kn USB，此用例必须配 uefi）' \
	    '                         不开窗口：QEMU 用 -display none，Bochs 用 rfb（VNC 从 5900 起）' \
	    '                         例：make run iso qemu usb uefi / make run hdd bochs built bios' \
	    '  make run-win [介质] [模拟器] [形态] [固件]' \
	    '                         与 run 同一套位置参数，差别只在开本地窗口：' \
	    '                         QEMU 挑可用的 gtk/sdl，Bochs 用 wx（配置界面同为 wx），要求有图形会话' \
	    '                         例：make run-win hdd qemu built bios' \
	    '                         显示方式可覆盖：QEMU_DISPLAY_WIN= / BOCHS_DISPLAY_WIN=' \
	    '  make run-uefi          = make run iso qemu built uefi' \
	    '  make deliverables      构建默认两份镜像与全部验收变体（4Kn / 降级 / 吊销三档）' \
	    '  make variants          只构建验收变体镜像' \
	    '  make check-all [WITH_BOOT=1]' \
	    '                         宿主侧 9 个检查工具串行跑完，出一张“通过/失败 + 用例计数”表；' \
	    '                         WITH_BOOT=1 先建默认镜像与全部验收变体，再跑十档启动矩阵' \
	    '                         （BIOS 六档 + UEFI 四档；32 位 UEFI 固件可用时再加 IA32 一档），' \
	    '                         每档三列：进内核 / 刷新路径 / 诊断编号' \
	    '                         之后再跑引导状态演练：在介质副本上造出空区域、两份损坏、候选失败、' \
	    '                         只剩痕迹等状态，各起一次真实引导，按“进内核/停机 + 编号 + 状态是否推进”核对' \
	    '                         输出落 $(OUT_DIR)/check/ —— 构建产物，不是证据目录，clean 会删；' \
	    '                         EVIDENCE_DIR=<Out 之外的目录> 转成脚本 --evidence，长期留存记录' \
	    '  make check-all ACCEPT=1  工业验收：再加失败注入（描述符区 / Stub / BaleenCore / 布局描述逐层弄坏）、' \
	    '                         NVMe 与 4Kn 介质控制器、引导状态机多轮、吊销/防回滚三档' \
	    '                         参数透传：CHECK_ALL_ARGS="--out /tmp/check --timeout 120"' \
	    '  make reproducible-check 两次 clean-all + 全量构建，比对交付物 sha256（要求 .cache/Keys 已有密钥）' \
	    '  make clean             删除 $(OUT_DIR)/；clean-all 连同子包与两类工具' \
	    '  磁盘分区单位：DISK_SECTOR_BYTES=512（默认）或 4096；只影响 Ext4 分区项，ESP 固定双份' \
	    '  载荷变量：STUB / CORE / EFI_IMAGE / SYSTEM_VOLUME（默认取真实产物，缺失时用占位件）' \
	    '  布局描述：LAYOUT=<路径> 为可选外部模板；仓库内没有生产者，缺省由组装器按 $(KERNEL_PATH_FILE) 生成' \
	    '  可变构建因素：SOURCE_DATE_EPOCH（默认 0）/ KEY_ID（默认 1）/ ALT_KEYS_DIR（第二把临时密钥目录，默认 .cache/AltKeys）'
