#!/bin/sh
# BootTest.sh
#    无人值守启动测试：QEMU 的 ISO/HDD × BIOS/UEFI × 内置/USB 共八例，另跑 Bochs HDD
#
#    BIOS 期望 BaleenStub（占位件输出里的 “BaleenStub not implemented yet” 与真实产物的标题都含它），UEFI 期望 MAKEIMAGE-EFI-BOOT-OK
#    用法：BootTest.sh <iso> <hdd> [OVMF_CODE] [OVMF_VARS]
#    BOOT_TEST_TIMEOUT=40 是每例上限，看到标记立即结束
#    BOOT_TEST_STRICT=1 将工具/固件缺失视为失败（exit 2）；真实失败始终 exit 1
#    失败自动保留日志；BOOT_TEST_KEEP_LOGS=1 也保留成功日志
#    可用 BOCHS / BOCHS_SHARE / BOCHS_ROM / BOCHS_VGAROM / BOCHS_ROM_ADDR /
#    BOCHS_MEM / BOCHS_DISPLAY / BOCHS_DISPLAY_OPTIONS 覆盖；BOCHS_BIOS 是旧 ROM 别名
#    BOCHS_CFG 可指定基础配置，测试会覆盖磁盘、固件、显示和日志以保证隔离
set -eu

iso=${1:?用法: BootTest.sh <iso> <hdd> [OVMF_CODE] [OVMF_VARS]}
hdd=${2:?用法: BootTest.sh <iso> <hdd> [OVMF_CODE] [OVMF_VARS]}
ovmf_code=${3:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${4:-/usr/share/OVMF/OVMF_VARS_4M.fd}
qemu=${QEMU:-qemu-system-x86_64}
bochs=${BOCHS:-bochs}
timeout_s=${BOOT_TEST_TIMEOUT:-40}
strict=${BOOT_TEST_STRICT:-0}
keep_logs=${BOOT_TEST_KEEP_LOGS:-0}
case "$strict:$keep_logs" in
    0:0|0:1|1:0|1:1) ;;
    *) printf '%s\n' 'boot-test: BOOT_TEST_STRICT 与 BOOT_TEST_KEEP_LOGS 只能是 0 或 1' >&2; exit 2 ;;
esac
case "$timeout_s" in
    ''|*[!0-9]*|0) printf '%s\n' 'boot-test: BOOT_TEST_TIMEOUT 必须是正整数秒' >&2; exit 2 ;;
esac

# 把相对路径补成基于当前目录的绝对路径
absolute() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "$PWD" "$1" ;;
    esac
}
iso=$(absolute "$iso")
hdd=$(absolute "$hdd")
ovmf_code=$(absolute "$ovmf_code")
ovmf_vars=$(absolute "$ovmf_vars")
helper=$(absolute "${RUN_BOOT_CASE:-$(dirname "$0")/../Bin/RunBootCase}")
for image in "$iso" "$hdd"; do
    if [ ! -f "$image" ] || [ ! -r "$image" ]; then
        printf 'boot-test: 输入镜像不可读：%s\n' "$image" >&2
        exit 1
    fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/likesprogramos-boot.XXXXXXXX")
work=$(absolute "$work")
passed=0
skipped=0
failed=0
active_pid=
# 退出时收尾：结束在跑的子进程，按结果决定是否保留日志目录
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ -n "$active_pid" ]; then
        kill -TERM "$active_pid" 2>/dev/null || :
        wait "$active_pid" 2>/dev/null || :
    fi
    if [ "$status" -ne 0 ] || [ "$keep_logs" -eq 1 ]; then
        printf 'boot-test: 日志保留在 %s\n' "$work"
    else
        rm -rf "$work"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'boot-test: 本次目录 %s\n' "$work"

# 记一例基础设施缺失（跳过），并写进汇总
missing() {
    skipped=$((skipped + $1))
    printf 'boot-test: 基础设施缺失，跳过 %s 例：%s\n' "$1" "$2"
    printf 'missing\t%s\t%s\n' "$1" "$2" >> "$work/summary.tsv"
}

# 打印汇总并按 strict 决定退出码
finish() {
    printf 'boot-test: 通过 %s，失败 %s，基础设施缺失 %s（strict=%s）\n' \
        "$passed" "$failed" "$skipped" "$strict"
    [ "$failed" -eq 0 ] || exit 1
    if [ "$strict" -eq 1 ] && [ "$skipped" -ne 0 ]; then
        exit 2
    fi
    exit 0
}

if [ ! -x "$helper" ]; then
    missing 9 "缺少运行辅助工具 $helper；先 make -C Tools/Build/RunBootCase"
    finish
fi

# 跑一例：run_case <名称> <目录名> <标记> <stdin文件或空> <命令...>
run_case() {
    name=$1
    case_dir="$work/$2"
    marker=$3
    input=$4
    shift 4
    mkdir -p "$case_dir"
    if [ -n "$input" ]; then
        set -- --stdin "$input" -- "$@"
    else
        set -- -- "$@"
    fi
    printf 'boot-test: 启动 %s\n' "$name"
    "$helper" --timeout "$timeout_s" --marker "$marker" \
        --cwd "$case_dir" --console "$case_dir/console.log" \
        --log "$case_dir/serial.log" --log "$case_dir/debug.log" "$@" &
    active_pid=$!
    if wait "$active_pid"; then
        passed=$((passed + 1))
        printf 'boot-test: %s 通过\n' "$name"
        printf 'passed\t%s\t%s\n' "$name" "$case_dir" >> "$work/summary.tsv"
    else
        failed=$((failed + 1))
        printf 'boot-test: %s 失败（日志 %s）\n' "$name" "$case_dir" >&2
        printf 'failed\t%s\t%s\n' "$name" "$case_dir" >> "$work/summary.tsv"
    fi
    active_pid=
}

if command -v "$qemu" >/dev/null 2>&1; then
    qemu=$(absolute "$(command -v "$qemu")")
    for firmware in bios uefi; do
        if [ "$firmware" = uefi ] && { [ ! -r "$ovmf_code" ] || [ ! -r "$ovmf_vars" ]; }; then
            missing 4 "缺少 OVMF CODE/VARS：$ovmf_code / $ovmf_vars"
            continue
        fi
        for media in iso hdd; do
            if [ "$media" = iso ]; then image=$iso; else image=$hdd; fi
            for mode in built usb; do
                case_id="$media-$firmware-$mode"
                case_dir="$work/$case_id"
                mkdir -p "$case_dir"
                set -- "$qemu" -machine q35 -m "${QEMU_MEM:-512}" \
                    -monitor none -display none -no-reboot \
                    -serial "file:$case_dir/serial.log" \
                    -debugcon "file:$case_dir/debug.log"
                if [ "$firmware" = uefi ]; then
                    cp "$ovmf_vars" "$case_dir/vars.fd"
                    set -- "$@" -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                        -drive "if=pflash,format=raw,file=$case_dir/vars.fd"
                    marker=MAKEIMAGE-EFI-BOOT-OK
                else
                    marker=BaleenStub
                fi
                if [ "$mode" = usb ]; then
                    set -- "$@" -device usb-ehci,id=ehci \
                        -drive "if=none,id=usbstick,format=raw,snapshot=on,file=$image" \
                        -device usb-storage,bus=ehci.0,drive=usbstick -boot order=c
                elif [ "$media" = iso ]; then
                    set -- "$@" -drive "if=ide,media=cdrom,format=raw,readonly=on,file=$image" -boot order=d
                else
                    set -- "$@" -drive "if=ide,format=raw,snapshot=on,file=$image" -boot order=c
                fi
                run_case "QEMU $media + $firmware + $mode" "$case_id" "$marker" '' "$@"
            done
        done
    done
else
    missing 8 "找不到 QEMU：$qemu"
fi

# 与顶层 Makefile 的 run 使用相同的可读 ROM 候选，悬空链接不阻止回退
bochs_share=${BOCHS_SHARE:-/usr/share/bochs}
if [ "${BOCHS_ROM+x}" = x ]; then
    bochs_rom=$BOCHS_ROM
elif [ "${BOCHS_BIOS+x}" = x ]; then
    bochs_rom=$BOCHS_BIOS
else
    bochs_rom=
    for rom in "$bochs_share/BIOS-bochs-latest" /usr/share/seabios/bios-256k.bin; do
        if [ -r "$rom" ]; then bochs_rom=$rom; break; fi
    done
fi
if [ "${BOCHS_VGAROM+x}" = x ]; then
    bochs_vgarom=$BOCHS_VGAROM
else
    bochs_vgarom=
    for rom in "$bochs_share/VGABIOS-lgpl-latest" /usr/share/seabios/vgabios-bochs-display.bin; do
        if [ -r "$rom" ]; then bochs_vgarom=$rom; break; fi
    done
fi
bochs_cfg=${BOCHS_CFG:-}
if ! command -v "$bochs" >/dev/null 2>&1; then
    missing 1 "找不到 Bochs：$bochs"
elif [ ! -r "$bochs_rom" ] || [ ! -r "$bochs_vgarom" ]; then
    missing 1 "缺少 Bochs BIOS/VGA ROM：$bochs_rom / $bochs_vgarom"
elif [ -n "$bochs_cfg" ] && [ ! -r "$bochs_cfg" ]; then
    printf 'boot-test: BOCHS_CFG 不可读：%s\n' "$bochs_cfg" >&2
    failed=$((failed + 1))
else
    bochs=$(absolute "$(command -v "$bochs")")
    bochs_rom=$(absolute "$bochs_rom")
    bochs_vgarom=$(absolute "$bochs_vgarom")
    bochs_display=${BOCHS_DISPLAY:-rfb}
    if [ "${BOCHS_DISPLAY_OPTIONS+x}" = x ]; then
        bochs_options=$BOCHS_DISPLAY_OPTIONS
    else
        case "$bochs_display" in
            rfb|vncsrv) bochs_options=timeout=0 ;;
            *) bochs_options= ;;
        esac
    fi
    case_dir="$work/hdd-bios-bochs"
    mkdir -p "$case_dir"
    printf 'continue\n' > "$case_dir/continue.rc"
    {
        if [ -n "$bochs_cfg" ]; then
            printf '#include "%s"\n' "$(absolute "$bochs_cfg")"
        fi
        printf 'romimage: file="%s", address=%s\n' "$bochs_rom" "${BOCHS_ROM_ADDR:-0}"
        printf 'vgaromimage: file="%s"\n' "$bochs_vgarom"
        printf 'megs: %s\n' "${BOCHS_MEM:-128}"
        printf '%s\n' 'config_interface: textconfig' 'sound: driver=dummy' 'speaker: enabled=0'
        printf '%s\n' 'ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14'
        printf 'ata0-master: type=disk, path="%s", mode=volatile, journal="%s/disk.redolog"\n' "$hdd" "$case_dir"
        printf '%s\n' 'boot: disk'
        printf 'display_library: %s' "$bochs_display"
        if [ -n "$bochs_options" ]; then printf ', options="%s"' "$bochs_options"; fi
        printf '\ncom1: enabled=1, mode=file, dev="%s/serial.log"\n' "$case_dir"
        printf 'log: %s/bochs.log\n' "$case_dir"
        printf '%s\n' 'panic: action=fatal'
    } > "$case_dir/bochsrc"
    printf 'boot-test: Bochs BIOS=%s，VGA=%s，display=%s\n' "$bochs_rom" "$bochs_vgarom" "$bochs_display"
    # 非 debugger 构建忽略 stdin；debugger 构建会读 continue，避免停在复位向量
    run_case 'Bochs hdd + bios + built' hdd-bios-bochs BaleenStub \
        "$case_dir/continue.rc" "$bochs" -q -f "$case_dir/bochsrc"
fi

finish
