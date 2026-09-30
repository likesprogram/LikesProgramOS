# Makefile
#    LikesProgramOS 顶层构建：镜像组装、宿主工具、运行与清理
#
#      make                       构建 Out/LikesProgram.iso 与 Out/LikesProgram.hdd
#      make iso | hdd             只构建其中一件
#      make packages              只跑 Packages 下已有的子 Makefile
#      make tools                 只构建 Tools 下的宿主工具（产物在 Tools/Bin，随源码交付）
#      make run [介质] [模拟器] [形态] [固件]
#                                 启动镜像。四个位置参数都可省，默认 hdd qemu built bios：
#                                   介质：hdd（默认）| iso
#                                   模拟器：qemu（默认）| bochs（Bochs 只支持 BIOS）
#                                   形态：built（默认，直接按磁盘/光盘挂给虚拟机）| usb（挂成 USB 存储，
#                                         模拟把镜像写进 U 盘后的样子）
#                                   固件：bios（默认）| uefi（用 OVMF）
#                                 例：make run iso qemu usb uefi / make run hdd bochs built bios
#      make run-win [介质] [模拟器] [形态] [固件]
#                                 与 run 同一套位置参数，区别只在画面：run 不开窗口，
#                                 run-win 开本地窗口（QEMU 挑可用的 gtk/sdl，Bochs 用 wx）
#                                 例：make run-win hdd qemu built bios / make run-win iso qemu built uefi
#      make clean                 删除顶层 Out/；make clean-all 连同子包与工具的产物
#
#    载荷来源：各子包的真实产物优先，缺失时用 Tools/Bin/MakePayloads 生成的占位件
#    （EFI 引导镜像、Ext4 系统卷、布局描述尚用占位件；Stub 与 Core 已有阶段性实现，
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
# 磁盘及混合 ISO 的 MBR 分区 LBA 单位；光盘 ISO9660 块仍为 2048
DISK_SECTOR_BYTES ?= 512

# —— 子包与宿主工具 ——
# 所有子包的 Makefile
SUBPACKAGE_MAKEFILES := $(wildcard Packages/*/Makefile Packages/*/*/Makefile)
# 需要构建的宿主工具目录
TOOL_DIRS := Tools/Build/MakeIso Tools/Build/MakeHdd Tools/Build/MakePayloads Tools/Build/PackImage Tools/Build/RunBootCase Tools/Build/CheckIpl Tools/Build/CheckStub

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
# ESP 分区内容
ESP_IMAGE     ?= $(EFI_IMAGE)
# Ext4 系统卷镜像
SYSTEM_VOLUME ?= $(firstword $(wildcard Packages/LikesProgramOS/Out/LikesProgram.img) $(PAYLOAD_DIR)/SystemVolume.img)
# 布局描述
LAYOUT        ?= $(firstword $(wildcard Packages/LikesProgramOS/Out/BaleenLayout.bin) $(PAYLOAD_DIR)/BaleenLayout.bin)

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
QEMU_MEDIA := -device usb-ehci,id=ehci -drive if=none,id=usbstick,format=raw,file=$(RUN_TARGET_IMAGE) -device usb-storage,bus=ehci.0,drive=usbstick -boot order=c
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
.PHONY: all images iso hdd packages tools compile-db payloads payload-report run run-win run-check run-uefi clean clean-all help qemu bochs built usb bios uefi

# 默认目标
all: images

# 两份镜像
images: $(ISO) $(HDD)

# 只构建光盘镜像
iso: $(ISO)
# 只构建磁盘镜像
hdd: $(HDD)

# run 的实参：作为目标出现时不做任何事（hdd/iso 本身是要构建的镜像目标）
qemu bochs built usb bios uefi: ;

# 跑 Packages 下已有的子 Makefile
packages:
	@for mk in $(SUBPACKAGE_MAKEFILES); do echo "== $(MAKE) -C $$(dirname $$mk)"; $(MAKE) -C "$$(dirname $$mk)" || exit 1; done

# 构建 Tools 下的宿主工具
tools:
	@for dir in $(TOOL_DIRS); do echo "== $(MAKE) -C $$dir"; $(MAKE) -C "$$dir" || exit 1; done

# 生成 compile_commands.json：扫描子包与工具的编译命令，供 clangd 与编辑器取用
compile-db:
	@Tools/Build/MakeCompileDb.sh

# 先建子包与工具，再补齐缺失的占位载荷
payloads: packages tools
	@Tools/Bin/MakePayloads "$(PAYLOAD_DIR)"

# 组装光盘镜像
$(ISO): payloads
	@$(MAKE) --no-print-directory payload-report
	Tools/Bin/MakeIso --out $@ --boot-image $(BOOT_IMAGE) --mbr $(MBR) \
	    --stub $(STUB) --core $(CORE) --efi $(EFI_IMAGE) --esp $(ESP_IMAGE) \
	    --system-volume $(SYSTEM_VOLUME) --iso-name $(SYSTEM_VOLUME_ISO_NAME) \
	    --volume-id $(VOLUME_ID) --sector-bytes $(DISK_SECTOR_BYTES) --file BaleenLayout.bin=$(LAYOUT) \
	    --manifest $(OUT_DIR)/LikesProgram.iso.json

# 组装磁盘镜像
$(HDD): payloads
	@$(MAKE) --no-print-directory payload-report
	Tools/Bin/MakeHdd --out $@ --mbr $(MBR) --stub $(STUB) --core $(CORE) --sector-bytes $(DISK_SECTOR_BYTES) --esp $(ESP_IMAGE) --system-volume $(SYSTEM_VOLUME) --manifest $(OUT_DIR)/LikesProgram.hdd.json

# 打印实际用到的是真实产物还是占位件
payload-report:
	@printf '载荷：\n'
	@printf '  引导镜像  %s\n' "$(BOOT_IMAGE)"
	@printf '  MBR       %s\n' "$(MBR)"
	@printf '  Stub      %s%s\n' "$(STUB)" "$(if $(findstring $(PAYLOAD_DIR),$(STUB)),  <- 占位件,)"
	@printf '  Core      %s%s\n' "$(CORE)" "$(if $(findstring $(PAYLOAD_DIR),$(CORE)),  <- 占位件,)"
	@printf '  EFI 镜像  %s%s\n' "$(EFI_IMAGE)" "$(if $(findstring $(PAYLOAD_DIR),$(EFI_IMAGE)),  <- 占位件,)"
	@printf '  系统卷    %s%s\n' "$(SYSTEM_VOLUME)" "$(if $(findstring $(PAYLOAD_DIR),$(SYSTEM_VOLUME)),  <- 占位件,)"
	@printf '  布局描述  %s%s\n' "$(LAYOUT)" "$(if $(findstring $(PAYLOAD_DIR),$(LAYOUT)),  <- 占位件,)"

# —— 启动：make run / run-win <hdd|iso> <qemu|bochs> <built|usb> <bios|uefi> ——
# 检查本次运行的参数组合与外部依赖
run-check:
	@if [ -n "$(RUN_EXTRA)" ]; then echo "$(RUN_LABEL): 无法识别的参数 $(RUN_EXTRA)；用法 make $(RUN_LABEL) <hdd|iso> <qemu|bochs> <built|usb> <bios|uefi>" >&2; exit 2; fi
	@if [ "$(RUN_EMU)" = bochs ] && [ "$(RUN_MODE)" = usb ]; then echo "$(RUN_LABEL): Bochs 没有 USB 存储仿真；usb 形态请用 qemu" >&2; exit 2; fi
	@if [ "$(RUN_EMU)" = bochs ] && [ "$(RUN_FIRMWARE)" = uefi ]; then echo "$(RUN_LABEL): Bochs 侧只支持 BIOS 路径，uefi 固件请用 qemu" >&2; exit 2; fi
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

# 连同子包与工具的产物一起删除
clean-all: clean
	@for mk in $(SUBPACKAGE_MAKEFILES); do $(MAKE) -C "$$(dirname $$mk)" clean || exit 1; done
	@for dir in $(TOOL_DIRS); do $(MAKE) -C "$$dir" clean || exit 1; done

# 打印用法
help:
	@printf '%s\n' \
	    'LikesProgramOS 顶层构建' \
	    '  make                   构建 $(ISO) 与 $(HDD)' \
	    '  make iso | hdd         只构建其中一件' \
	    '  make packages|tools    只跑子包或只建宿主工具' \
	    '  make compile-db        扫描各包写出 compile_commands.json（clangd 与编辑器用）' \
	    '  make run [介质] [模拟器] [形态] [固件]' \
	    '                         介质 hdd|iso（默认 hdd）；模拟器 qemu|bochs（默认 qemu，Bochs 只支持 BIOS）；' \
	    '                         形态 built|usb（默认 built）；固件 bios|uefi（默认 bios，uefi 用 OVMF）' \
	    '                         不开窗口：QEMU 用 -display none，Bochs 用 rfb（VNC 从 5900 起）' \
	    '                         例：make run iso qemu usb uefi / make run hdd bochs built bios' \
	    '  make run-win [介质] [模拟器] [形态] [固件]' \
	    '                         与 run 同一套位置参数，差别只在开本地窗口：' \
	    '                         QEMU 挑可用的 gtk/sdl，Bochs 用 wx（配置界面同为 wx），要求有图形会话' \
	    '                         例：make run-win hdd qemu built bios' \
	    '                         显示方式可覆盖：QEMU_DISPLAY_WIN= / BOCHS_DISPLAY_WIN=' \
	    '  make run-uefi          = make run iso qemu built uefi' \
	    '  make clean             删除 $(OUT_DIR)/；clean-all 连同子包与工具' \
	    '  磁盘分区单位：DISK_SECTOR_BYTES=512（默认）或 4096；不改变 run 的虚拟设备参数' \
	    '  载荷变量：STUB / CORE / EFI_IMAGE / SYSTEM_VOLUME / LAYOUT（默认取真实产物，缺失时用占位件）'
