# LikesProgramOS 顶层构建
#
#   make                       构建 Out/LikesProgram.iso 与 Out/LikesProgram.hdd
#   make iso | hdd             只构建其中一件
#   make packages              只跑 Packages 下已有的子 Makefile
#   make tools                 只构建 Tools 下的宿主工具（产物在 Tools/Bin，随源码交付）
#   make run [介质] [模拟器] [形态] [固件]
#                              启动镜像。四个位置参数都可省，默认 hdd qemu built bios：
#                                介质：hdd（默认）| iso
#                                模拟器：qemu（默认）| bochs（Bochs 只支持 BIOS）
#                                形态：built（默认，直接按磁盘/光盘挂给虚拟机）| usb（挂成 USB 存储，
#                                      模拟把镜像写进 U 盘后的样子）
#                                固件：bios（默认）| uefi（用 OVMF）
#                              例：make run iso qemu usb uefi / make run hdd bochs built bios
#   make boot-test             无人值守：QEMU 按光盘/磁盘、BIOS/UEFI、内置/U 盘形态逐项启动并查标记
#                              （Bochs 用例只在有 Bochs 专用 BIOS ROM 时执行，否则跳过并说明）
#   make clean                 删除顶层 Out/；make clean-all 连同子包与工具的产物
#
# 载荷来源：各子包的真实产物优先，缺失时用 Tools/Build/Payloads 生成的占位件
# （Stub、Core、EFI 引导镜像、Ext4 系统卷、布局描述都还没实现，占位件只为把引导链
# 跑通；每个占位件都写明身份，构建时会打印实际用到的那一份）。

OUT_DIR       ?= Out
PAYLOAD_DIR   := $(OUT_DIR)/Payload
ISO           := $(OUT_DIR)/LikesProgram.iso
HDD           := $(OUT_DIR)/LikesProgram.hdd
VOLUME_ID     ?= LIKESPROGRAM
SYSTEM_VOLUME_ISO_NAME ?= SystemVolume.img

# —— 子包与宿主工具 ——
SUBPACKAGE_MAKEFILES := $(wildcard Packages/*/Makefile Packages/*/*/Makefile)
TOOL_DIRS := Tools/Build/MakeIso Tools/Build/MakeHdd

# —— 载荷：真实产物优先，缺失时退到占位件 ——
IPL_BIN       := Packages/Baleen/Ipl/Out/Bin
BOOT_IMAGE    ?= $(IPL_BIN)/BaleenIPLCd.bin
MBR           ?= $(IPL_BIN)/BaleenIPL.bin
STUB          ?= $(firstword $(wildcard Packages/Baleen/Stub/Out/Bin/BaleenStub.bin) $(PAYLOAD_DIR)/BaleenStub.bin)
CORE          ?= $(firstword $(wildcard Packages/Baleen/Core/Out/Bin/BaleenCore.bin) $(PAYLOAD_DIR)/BaleenCore.bin)
EFI_IMAGE     ?= $(firstword $(wildcard Packages/Baleen/Uefi/Out/Bin/Efi.img) $(PAYLOAD_DIR)/Efi.img)
ESP_IMAGE     ?= $(EFI_IMAGE)
SYSTEM_VOLUME ?= $(firstword $(wildcard Packages/LikesProgramOS/Out/LikesProgram.img) $(PAYLOAD_DIR)/SystemVolume.img)
LAYOUT        ?= $(firstword $(wildcard Packages/LikesProgramOS/Out/BaleenLayout.bin) $(PAYLOAD_DIR)/BaleenLayout.bin)

# —— 启动 ——
QEMU         ?= qemu-system-x86_64
QEMU_MEM     ?= 512
# 默认不开图形窗口；要窗口就 QEMU_DISPLAY= 清空该变量
QEMU_DISPLAY ?= -display none
QEMU_EXTRA   ?=
OVMF_CODE    ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS    ?= /usr/share/OVMF/OVMF_VARS_4M.fd
BOCHS        ?= bochs
# Bochs 的 BIOS ROM：发行版包常常只带 VGABIOS，缺 BIOS 时退到 SeaBIOS
BOCHS_SHARE  ?= /usr/share/bochs
BOCHS_ROM    ?= $(firstword $(wildcard $(BOCHS_SHARE)/BIOS-bochs-latest) $(wildcard /usr/share/seabios/bios-256k.bin))
BOCHS_VGAROM ?= $(firstword $(wildcard $(BOCHS_SHARE)/VGABIOS-lgpl-latest) $(wildcard /usr/share/seabios/vgabios-bochs-display.bin))
BOCHS_ROM_ADDR ?= $(if $(findstring bios-256k,$(BOCHS_ROM)),0xfffc0000,0xfffe0000)
BOCHS_MEM    ?= 128
# 本机 Bochs 只带 rfb（VNC）与 wx（本地窗口）两种显示库，默认 rfb：无头可用，
# 想看画面就 VNC 连 5900，或 BOCHS_DISPLAY=wx 开本地窗口
BOCHS_DISPLAY ?= rfb
BOCHS_DISPLAY_OPTIONS ?= port=5900
# make 的 $(if ...) 里逗号是参数分隔符，要输出字面逗号得借道一个变量
comma := ,
BOCHS_DISPLAY_LINE = display_library: $(BOCHS_DISPLAY)$(if $(BOCHS_DISPLAY_OPTIONS),$(comma) options="$(BOCHS_DISPLAY_OPTIONS)")

# —— run 的位置参数：make run <hdd|iso> <qemu|bochs> <built|usb> <bios|uefi> ——
# 取第 2 个及之后的全部目标词，多给的参数由 run-check 拦下（否则 make 会先跑 run 再报未知目标）
RUN_GIVEN    := $(wordlist 2,9,$(MAKECMDGOALS))
RUN_IMAGE    := $(firstword $(filter hdd iso,$(RUN_GIVEN)) hdd)
RUN_EMU      := $(firstword $(filter qemu bochs,$(RUN_GIVEN)) qemu)
RUN_MODE     := $(firstword $(filter built usb,$(RUN_GIVEN)) built)
RUN_FIRMWARE := $(firstword $(filter bios uefi,$(RUN_GIVEN)) bios)
RUN_EXTRA    := $(filter-out hdd iso qemu bochs built usb bios uefi,$(RUN_GIVEN))
ifeq ($(RUN_IMAGE),iso)
RUN_TARGET_IMAGE := $(ISO)
else
RUN_TARGET_IMAGE := $(HDD)
endif

# 形态：built 直接按光盘/磁盘挂载；usb 挂成 USB 存储（模拟写入 U 盘后的样子）
ifeq ($(RUN_MODE),usb)
QEMU_MEDIA := -device usb-ehci,id=ehci -drive if=none,id=usbstick,format=raw,file=$(RUN_TARGET_IMAGE) -device usb-storage,bus=ehci.0,drive=usbstick -boot order=c
else ifeq ($(RUN_IMAGE),iso)
QEMU_MEDIA := -cdrom $(RUN_TARGET_IMAGE) -boot d
else
QEMU_MEDIA := -drive file=$(RUN_TARGET_IMAGE),format=raw,if=ide -boot c
endif

ifeq ($(RUN_FIRMWARE),uefi)
QEMU_FIRMWARE := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) -drive if=pflash,format=raw,file=$(OUT_DIR)/OVMF_VARS.fd
else
QEMU_FIRMWARE :=
endif
# 两条固件路径的可见输出都走 COM1：EFI 应用、Bochs 的 mode=file、以及以后的真实内核都能写它
QEMU_CONSOLE := -serial stdio
# QEMU 只允许一个字符设备占用 stdio：串口占了 stdio，调试口（0xE9）默认写文件
QEMU_DEBUGCON ?= -debugcon file:$(OUT_DIR)/debugcon.log

.DEFAULT_GOAL := all
.PHONY: all images iso hdd packages tools payloads payload-report run run-uefi boot-test clean clean-all help qemu bochs built usb

all: images

images: $(ISO) $(HDD)

iso: $(ISO)
hdd: $(HDD)

# run 的实参：作为目标出现时不做任何事（hdd/iso 本身是要构建的镜像目标）
qemu bochs built usb: ;

packages:
	@for mk in $(SUBPACKAGE_MAKEFILES); do echo "== $(MAKE) -C $$(dirname $$mk)"; $(MAKE) -C "$$(dirname $$mk)" || exit 1; done

tools:
	@for dir in $(TOOL_DIRS); do echo "== $(MAKE) -C $$dir"; $(MAKE) -C "$$dir" || exit 1; done

# 先建子包与工具，再补齐缺失的占位载荷
payloads: packages tools
	@sh Tools/Build/Payloads/MakePlaceholders.sh "$(PAYLOAD_DIR)"

$(ISO): payloads
	@$(MAKE) --no-print-directory payload-report
	Tools/Bin/MakeIso --out $@ --boot-image $(BOOT_IMAGE) --mbr $(MBR) \
	    --stub $(STUB) --core $(CORE) --efi $(EFI_IMAGE) --esp $(ESP_IMAGE) \
	    --system-volume $(SYSTEM_VOLUME) --iso-name $(SYSTEM_VOLUME_ISO_NAME) \
	    --volume-id $(VOLUME_ID) --file BaleenLayout.bin=$(LAYOUT) \
	    --manifest $(OUT_DIR)/LikesProgram.iso.json

$(HDD): payloads
	@$(MAKE) --no-print-directory payload-report
	Tools/Bin/MakeHdd --out $@ --mbr $(MBR) --stub $(STUB) --core $(CORE) --esp $(ESP_IMAGE) --system-volume $(SYSTEM_VOLUME) --manifest $(OUT_DIR)/LikesProgram.hdd.json

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

# —— 启动：make run <hdd|iso> <qemu|bochs> <built|usb> ——
run-check:
	@if [ -n "$(RUN_EXTRA)" ]; then echo "run: 无法识别的参数 $(RUN_EXTRA)；用法 make run <hdd|iso> <qemu|bochs> <built|usb> <bios|uefi>" >&2; exit 2; fi
	@if [ "$(RUN_EMU)" = bochs ] && [ "$(RUN_MODE)" = usb ]; then echo "run: Bochs 没有 USB 存储仿真；usb 形态请用 qemu" >&2; exit 2; fi
	@if [ "$(RUN_EMU)" = bochs ] && [ "$(RUN_FIRMWARE)" = uefi ]; then echo "run: Bochs 侧只有 SeaBIOS（BIOS 路径），uefi 固件请用 qemu" >&2; exit 2; fi

ifeq ($(RUN_EMU),qemu)
run: run-check $(RUN_TARGET_IMAGE)
	@echo "run: qemu / $(RUN_IMAGE) / $(RUN_MODE) / $(RUN_FIRMWARE)"
ifeq ($(RUN_FIRMWARE),uefi)
	@test -f "$(OVMF_CODE)" || { echo "run: 缺少 OVMF（$(OVMF_CODE)），可用 OVMF_CODE= 指定" >&2; exit 1; }
	@mkdir -p $(OUT_DIR) && cp "$(OVMF_VARS)" $(OUT_DIR)/OVMF_VARS.fd
endif
	$(QEMU) -machine q35 -m $(QEMU_MEM) $(QEMU_DISPLAY) $(QEMU_FIRMWARE) $(QEMU_MEDIA) $(QEMU_CONSOLE) $(QEMU_DEBUGCON) $(QEMU_EXTRA)
else
run: run-check $(RUN_TARGET_IMAGE) $(OUT_DIR)/bochsrc.$(RUN_IMAGE)
	@echo "run: bochs / $(RUN_IMAGE) / $(RUN_MODE) / bios"
	@echo "run: 串口日志 $(OUT_DIR)/bochs-$(RUN_IMAGE).serial.log；画面走 $(BOCHS_DISPLAY)$(if $(BOCHS_DISPLAY_OPTIONS),（$(BOCHS_DISPLAY_OPTIONS)）)"
	@if [ ! -f "$(BOCHS_SHARE)/BIOS-bochs-latest" ]; then \
	    echo "run: 本机没有 Bochs 专用 BIOS ROM（$(BOCHS_SHARE)/BIOS-bochs-latest），回退到 $(BOCHS_ROM)；" >&2; \
	    echo "     某些 Bochs 构建（如 Ubuntu 的 2.8+dfsg）在这个组合下会在设备初始化时崩溃" >&2; \
	fi
	$(BOCHS) -q -f $(OUT_DIR)/bochsrc.$(RUN_IMAGE)
endif

# 便捷别名：UEFI 光盘启动
run-uefi:
	@$(MAKE) --no-print-directory run iso qemu built uefi

# Bochs 配置：BIOS ROM 缺 BIOS-bochs-latest 时用 SeaBIOS 顶上
$(OUT_DIR)/bochsrc.iso: Makefile
	@mkdir -p $(OUT_DIR)
	@printf '%s\n' \
	    'romimage: file=$(BOCHS_ROM), address=$(BOCHS_ROM_ADDR)' \
	    'vgaromimage: file=$(BOCHS_VGAROM)' \
	    'megs: $(BOCHS_MEM)' \
	    'sound: enabled=0' \
	    'ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14' \
	    'ata0-master: type=cdrom, path="$(ISO)", status=inserted' \
	    'boot: cdrom' \
	    '$(BOCHS_DISPLAY_LINE)' \
	    'com1: enabled=1, mode=file, dev="$(OUT_DIR)/bochs-iso.serial.log"' \
	    'log: /dev/null' \
	    'panic: action=fatal' \
	    > $@

$(OUT_DIR)/bochsrc.hdd: Makefile
	@mkdir -p $(OUT_DIR)
	@printf '%s\n' \
	    'romimage: file=$(BOCHS_ROM), address=$(BOCHS_ROM_ADDR)' \
	    'vgaromimage: file=$(BOCHS_VGAROM)' \
	    'megs: $(BOCHS_MEM)' \
	    'sound: enabled=0' \
	    'ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14' \
	    'ata0-master: type=disk, path="$(HDD)", mode=flat' \
	    'boot: disk' \
	    '$(BOCHS_DISPLAY_LINE)' \
	    'com1: enabled=1, mode=file, dev="$(OUT_DIR)/bochs-hdd.serial.log"' \
	    'log: /dev/null' \
	    'panic: action=fatal' \
	    > $@

# 无人值守启动测试：光盘/磁盘 × BIOS/UEFI × 内置/U 盘形态
boot-test: $(ISO) $(HDD) $(OUT_DIR)/bochsrc.hdd
	QEMU="$(QEMU)" QEMU_MEM="$(QEMU_MEM)" BOCHS="$(BOCHS)" BOCHS_CFG="$(abspath $(OUT_DIR)/bochsrc.hdd)" BOCHS_BIOS="$(BOCHS_SHARE)/BIOS-bochs-latest" sh Tools/Build/BootTest.sh "$(ISO)" "$(HDD)" "$(OVMF_CODE)" "$(OVMF_VARS)"

clean:
	@test -n "$(OUT_DIR)" || { echo "clean: OUT_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OUT_DIR)

clean-all: clean
	@for mk in $(SUBPACKAGE_MAKEFILES); do $(MAKE) -C "$$(dirname $$mk)" clean || exit 1; done
	@for dir in $(TOOL_DIRS); do $(MAKE) -C "$$dir" clean || exit 1; done

help:
	@printf '%s\n' \
	    'LikesProgramOS 顶层构建' \
	    '  make                   构建 $(ISO) 与 $(HDD)' \
	    '  make iso | hdd         只构建其中一件' \
	    '  make packages|tools    只跑子包或只建宿主工具' \
	    '  make run [介质] [模拟器] [形态] [固件]' \
	    '                         介质 hdd|iso（默认 hdd）；模拟器 qemu|bochs（默认 qemu，Bochs 只支持 BIOS）；' \
	    '                         形态 built|usb（默认 built）；固件 bios|uefi（默认 bios，uefi 用 OVMF）' \
	    '                         例：make run iso qemu usb uefi / make run hdd bochs built bios' \
	    '                         QEMU_DISPLAY= 开图形窗口；Bochs 默认 rfb（VNC 5900），BOCHS_DISPLAY=wx 开本地窗口' \
	    '  make run-uefi          = make run iso qemu built uefi' \
	    '  make boot-test         无人值守启动测试（BIOS/UEFI × 光盘/磁盘/U 盘形态）' \
	    '  make clean             删除 $(OUT_DIR)/；clean-all 连同子包与工具' \
	    '  载荷变量：STUB / CORE / EFI_IMAGE / SYSTEM_VOLUME / LAYOUT（默认取真实产物，缺失时用占位件）'
