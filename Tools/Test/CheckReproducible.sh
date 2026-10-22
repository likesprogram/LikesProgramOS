#!/bin/sh
# CheckReproducible.sh
#    可复现构建检查：两次 make clean-all + 全量构建，逐份比对交付物的 sha256
#
#    前提是两把外部密钥输入都已就位：.cache/Keys/Development.sec 与
#    .cache/AltKeys/Development.sec（都在仓库之外、不进镜像；第二把用于吊销变体）。
#    缺失时直接报错、不自动生成——随机构建密钥会让“同一份输入”不成立。
#    两处密钥目录都不受 clean/clean-all 影响，所以两次 clean 构建用的是同一组密钥。
#    每次构建都从 make clean-all 之后开始；日志与两份哈希清单留在 /tmp/repro-check-*。
#    给出 --evidence <目录> 时把结论、两份清单与差异复制到该目录（长期留存用）。
#    退出码：0 两次构建逐字节一致；1 有差异或构建失败；2 用法或前提错误
set -eu

# 脚本自身位置：据此定位项目根，不依赖调用时的当前目录
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

# 证据目录（可选）与并行度
evidence_dir=
jobs=
while [ $# -gt 0 ]; do
    case "$1" in
        --evidence)
            [ $# -ge 2 ] || { echo "repro-check: --evidence 缺少参数" >&2; exit 2; }
            evidence_dir=$2
            shift 2
            ;;
        --jobs)
            [ $# -ge 2 ] || { echo "repro-check: --jobs 缺少参数" >&2; exit 2; }
            jobs=$2
            shift 2
            ;;
        --help|-h)
            sed -n '2,/^set -eu$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "repro-check: 无法识别的参数 $1（--help 看用法）" >&2
            exit 2
            ;;
    esac
done
if [ -n "$jobs" ]; then
    case "$jobs" in *[!0-9]*|0) echo "repro-check: --jobs 必须是正整数" >&2; exit 2 ;; esac
else
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)
fi

# 外部密钥输入：可复现构建不能自带随机输入（主密钥与第二把临时密钥都要固定）
sign_key=$root/.cache/Keys/Development.sec
alt_sign_key=$root/.cache/AltKeys/Development.sec
for required_key in "$sign_key" "$alt_sign_key"; do
    if [ ! -f "$required_key" ]; then
        echo "repro-check: 可复现构建需要外部密钥输入：缺少 $required_key" >&2
        echo "repro-check: 不要把随机生成的开发期密钥当构建输入；先准备/保留一份固定密钥再跑" >&2
        exit 2
    fi
done

# 工作目录：日志、哈希清单与差异都留在这里（不写仓库目录）
work=$(mktemp -d /tmp/repro-check-XXXXXX) || { echo "repro-check: 无法创建临时目录" >&2; exit 1; }
summary=$work/summary.txt
: > "$summary"

# 交付物：相对项目根，两次构建后逐份核对
deliverables="Out/LikesProgram.iso
Out/LikesProgram.hdd
Out/LikesProgram.4kn.hdd
Out/LikesProgram.degrade.hdd
Out/LikesProgram.revocation-floor.hdd
Out/LikesProgram.revocation-bad.hdd
Out/LikesProgram.revocation-key.hdd
Packages/Baleen/Uefi/Out/Bin/Efi.img
Packages/Baleen/Uefi/Out/Bin/BOOTX64.EFI
Packages/Baleen/Uefi/Out/Bin/BOOTIA32.EFI
Packages/Baleen/Core/Out/Bin/BaleenCore.bin
Packages/Baleen/Stub/Out/Bin/BaleenStub.bin
Packages/Baleen/Ipl/Out/Bin/BaleenIPL.bin
Packages/Baleen/Ipl/Out/Bin/BaleenIPLCd.bin
Packages/LikesProgramOS/Out/LikesProgram.img
Packages/LikesProgramOS/Out/KernelSetStage/LikesProgram.os
Packages/LikesProgramOS/Out/KernelSetStage/Modules.list
Packages/LikesProgramOS/Out/KernelSetStage/Manifest.mft
Packages/LikesProgramOS/Out/BuildId
Packages/LikesProgramOS/Out/KernelPath
Packages/KernelModules/Out/Modules/*.osm
Packages/LikesProgramOS/Static/LikesProgram/System/KernelSets/*/Modules/*.osm
Packages/LikesProgramOS/Static/LikesProgram/System/KernelSets/*/Manifest.*.mft
Packages/LikesProgramOS/Static/LikesProgram/System/KernelSets/*/Modules.*.list"

# 结论行同时进终端与汇总文件
say() {
    printf '%s\n' "$*"
    printf '%s\n' "$*" >> "$summary"
}

# 清空全部构建产物（.cache/Keys 与 .cache/AltKeys 是仓库外的外部输入，不删）
clean_all() {
    label=$1
    log=$work/clean-$label.log
    if ! ( cd "$root" && make clean-all ) > "$log" 2>&1; then
        say "repro-check: 第 $label 次 clean-all 失败（日志 $log）"
        tail -n 20 "$log" >> "$summary" || true
        return 1
    fi
}

# 全量构建：交付物 + 验证变体；时间戳/密钥编号取固定值，两把密钥都用仓库外的持久输入
build_once() {
    label=$1
    log=$work/build-$label.log
    start=$(date +%s)
    if ! ( cd "$root" && SOURCE_DATE_EPOCH=0 KEY_ID=1 \
            make -j"$jobs" deliverables ) > "$log" 2>&1; then
        say "repro-check: 第 $label 次构建失败（日志 $log）"
        tail -n 30 "$log" >> "$summary" || true
        return 1
    fi
    elapsed=$(( $(date +%s) - start ))
    say "repro-check: 第 $label 次构建完成（${elapsed}s，日志 $log）"
}

# 记录交付物的 sha256 与字节数：清单每行 "sha256  字节数  相对路径"
record_hashes() {
    label=$1
    out=$work/hashes-$label.txt
    : > "$out"
    missing=0
    for path in $deliverables; do
        if [ ! -f "$root/$path" ]; then
            printf '缺失  -  %s\n' "$path" >> "$out"
            missing=1
            continue
        fi
        sha=$(cd "$root" && sha256sum "$path" | cut -d' ' -f1)
        bytes=$(wc -c < "$root/$path" | tr -d ' ')
        printf '%s  %s  %s\n' "$sha" "$bytes" "$path" >> "$out"
    done
    if [ "$missing" -ne 0 ]; then
        say "repro-check: 第 $label 次构建后交付物不全（清单 $out）"
        return 1
    fi
    say "repro-check: 第 $label 次构建记录 $(wc -l < "$out" | tr -d ' ') 份交付物 sha256（清单 $out）"
}

# PE 头的构建时间戳：--no-insert-timestamp 下必须固定（这里取 bootx64/bootia32 两处）
pe_timestamp() {
    TZ=UTC LC_ALL=C objdump -p "$1" 2>/dev/null | sed -n 's/^Time\/Date[[:space:]]*//p' | head -n 1
}

say "repro-check: 工作目录 $work"
say "repro-check: 外部密钥 $sign_key；第二把临时密钥 $alt_sign_key"

clean_all 1 || exit 1
build_once 1 || exit 1
record_hashes 1 || exit 1
time1_x64=$(pe_timestamp "$root/Packages/Baleen/Uefi/Out/Bin/BOOTX64.EFI")
time1_ia32=$(pe_timestamp "$root/Packages/Baleen/Uefi/Out/Bin/BOOTIA32.EFI")

clean_all 2 || exit 1
build_once 2 || exit 1
record_hashes 2 || exit 1
time2_x64=$(pe_timestamp "$root/Packages/Baleen/Uefi/Out/Bin/BOOTX64.EFI")
time2_ia32=$(pe_timestamp "$root/Packages/Baleen/Uefi/Out/Bin/BOOTIA32.EFI")

# PE 时间戳证据：两次必须一致，且应是固定值（epoch 1970，与时区无关地看年份）
pe_status=通过
[ "$time1_x64" = "$time2_x64" ] || pe_status=失败
[ "$time1_ia32" = "$time2_ia32" ] || pe_status=失败
case "$time1_x64" in *1970*) ;; *) pe_status=失败 ;; esac
case "$time1_ia32" in *1970*) ;; *) pe_status=失败 ;; esac
say "repro-check: BOOTX64.EFI Time/Date：$time1_x64（两次一致：$([ "$time1_x64" = "$time2_x64" ] && echo 是 || echo 否)）"
say "repro-check: BOOTIA32.EFI Time/Date：$time1_ia32（两次一致：$([ "$time1_ia32" = "$time2_ia32" ] && echo 是 || echo 否)）"

# 差异比较
if diff -u "$work/hashes-1.txt" "$work/hashes-2.txt" > "$work/hashes.diff"; then
    rm -f "$work/hashes.diff"
    hash_status=通过
else
    hash_status=失败
fi

status=通过
[ "$pe_status" = 通过 ] || status=失败
[ "$hash_status" = 通过 ] || status=失败

if [ "$status" = 通过 ]; then
    say "repro-check: PASS 两次全量构建的 $(wc -l < "$work/hashes-1.txt" | tr -d ' ') 份交付物 sha256 完全一致"
else
    say "repro-check: FAIL 两次构建的交付物不一致（差异 $work/hashes.diff）"
    sed -n '1,120p' "$work/hashes.diff" >> "$summary" || true
    if [ "$pe_status" != 通过 ]; then say "repro-check: PE 头 Time/Date 不是固定值"; fi
fi
say "repro-check: 日志与哈希清单保留在 $work"

# 证据目录：把结论与两份清单复制过去（不存在则创建）
if [ -n "$evidence_dir" ]; then
    if ! mkdir -p "$evidence_dir"; then
        echo "repro-check: 无法创建证据目录 $evidence_dir" >&2
        exit 2
    fi
    cp "$summary" "$work/hashes-1.txt" "$work/hashes-2.txt" "$evidence_dir/" 2>/dev/null || true
    [ -f "$work/hashes.diff" ] && cp "$work/hashes.diff" "$evidence_dir/" 2>/dev/null || true
    cp "$work"/build-*.log "$work"/clean-*.log "$evidence_dir/" 2>/dev/null || true
    echo "repro-check: 证据已复制到 $evidence_dir"
fi

[ "$status" = 通过 ] || exit 1
