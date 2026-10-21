#!/bin/sh
# CheckAll.sh
#    一条命令跑完宿主侧全部检查，并可选跑启动矩阵，结果收成两张表
#
#    宿主侧：9 个检查工具串行执行（CheckIpl、CheckStub、CheckStorage、CheckExt4、
#    CheckCrypto、CheckSelect、CheckMemory、CheckRevocation、CheckEfiBlockDevice），
#    逐个报告通过或失败与用例计数
#    工业验收（--accept，隐含 --with-boot）：在启动矩阵与状态演练之外再跑失败注入、
#    介质与控制器、吊销与防回滚、只读介质与状态写入、引导状态机多轮；各段按期望停机或进内核核对
#      · 失败注入：整链逐层弄坏一次（描述符区 / Stub 摘要 / BaleanCore 头与摘要 / 布局描述魔数与校验和），
#        每层的期望是“按哪个编号停机、且绝不进内核”；另有两档篡改布局描述的内核路径并重算校验和
#        （布局本身仍合法），指向扁平落点或与清单产物不符时必须拒绝启动（E-CORE-84 / E-CORE-85）；
#        还有一档把布局的 StateBytes 改成不足一个区域（布局仍然合法），状态区参数非法必须停机（E-CORE-90）；
#        以及一档把内核路径改成含空分量的非法语法，布局校验必须在选版前拒绝（E-CORE-93）
#      · 介质与控制器：NVMe 控制器（BIOS / UEFI）与 4Kn 设备 + 按 4096 单位组装的镜像（两档）
#      · 引导状态机多轮：候选连试三次未确认 → 用尽 → 稳态仍可启动；以及确认提交后走已确认集
#    工业验收还包含 4Kn 介质上的状态推进（Pending→Trying、候选失败→Failed 真实落盘）、
#    只读 ISO 上的状态写入降级（E-CORE-70）、吊销与防回滚三档（E-CORE-66 / 75 / 74）
#
#    启动矩阵（--with-boot）：十档或十一档介质各起一次真实引导，每档抽三列——
#    进内核（内核横幅 LikesProgramOS kernel）、刷新路径（Flush= 或“无刷新原语”）、
#    诊断编号（E-XXX-nn），任一档没进内核即判失败
#    其中 BIOS 六档（QEMU 四档 + Bochs 两档）与 UEFI 四档（HDD / ISO / U 盘 / 4Kn U 盘，走 OVMF）；
#    32 位固件与 qemu-system-i386 都在时再加 IA32 一档（OVMF32 加载 BOOTIA32.EFI，只验证 E-UEFI-11）
#    UEFI 档不只认内核横幅：还要求出现“清单已验证 / 已退出启动服务 / 内核已放到装载地址”三条步骤标记
#    启动矩阵之后再跑引导状态演练：在介质副本上造出空区域、两份损坏、候选失败、只剩痕迹等状态，
#    各起一次真实引导，按“该进内核还是该停机 + 该出现哪个编号 + 状态区该不该被推进”逐档核对
#    另有写中途断电（撕裂最新槽位 / 七个槽位只有半条记录）与设备级写缓存三档：刷新覆盖核验
#    （本盘 / 光盘 + 他人硬盘）、USB 无刷新原语的持久化边界、丢掉设备缓存后重来
#
#    输出落在 --out 指定的目录，默认 Out/check。**它是构建产物，不是证据目录**：
#    顶层 make clean 与 clean-all 会连同 Out/ 一起删除它；要长期留存、可复核的验证记录，
#    把 --out 指到 Out/ 之外，或及时拷走 summary.txt 与各例日志
#
#    --evidence <目录> 指定证据目录（必须在项目 Out 之外）：运行结束时把 summary、host/boot/state/accept
#    的逐例日志、状态报告与 metadata.txt（UTC 时间、git 版本、uname、QEMU/OVMF 版本与文件哈希、
#    镜像批次与 ESP 里的 BOOTX64.EFI / BOOTIA32.EFI / Efi.img 的 sha256 和字节数）复制过去；
#    --out 仍然是可删的构建产物，两者不要指到一起
#
#    用法：CheckAll.sh [选项]
#      --with-boot          宿主检查之后再跑十档启动矩阵（另有 IA32 时共十一档）与引导状态演练
#      --accept             工业验收：在上面基础上加失败注入、NVMe / 4Kn 介质与控制器、吊销与防回滚、引导状态机多轮
#      --image-4kn <文件>   4Kn 验收镜像，默认 Out/LikesProgram.4kn.hdd
#      --image-degrade <文件>  置了降级策略位的镜像，默认 Out/LikesProgram.degrade.hdd
#      --image-revocation-floor <文件>  安全版本低于下限的变体，默认 Out/LikesProgram.revocation-floor.hdd
#      --image-revocation-bad <文件>    吊销记录损坏的变体，默认 Out/LikesProgram.revocation-bad.hdd
#      --image-revocation-key <文件>    签名密钥被吊销的变体，默认 Out/LikesProgram.revocation-key.hdd
#      --bin-dir <目录>     检查工具所在目录，默认 Tools/Bin
#      --out <目录>         输出目录，默认 Out/check；不能指向项目根或根 Out（会删真实镜像）
#      --evidence <目录>    证据目录，必须落在项目 Out 之外；结束时复制 summary、逐例日志与 metadata.txt
#      --manifest-hdd <文件>  HDD 镜像的布局清单 JSON，默认 <image-hdd>.json；失败注入的偏移从它读
#      --manifest-iso <文件>  ISO 镜像的布局清单 JSON，默认 <image-iso>.json
#      --image-iso <文件>   启动矩阵用的光盘镜像，默认 Out/LikesProgram.iso
#      --image-hdd <文件>   启动矩阵用的磁盘镜像，默认 Out/LikesProgram.hdd
#      --timeout <秒>       启动矩阵每例的上限，默认 60
#      --help               打印本用法
#    环境变量：QEMU / QEMU_MEM / QEMU32 / OVMF_CODE / OVMF_VARS / OVMF32_CODE / OVMF32_VARS /
#    BOCHS / BOCHS_SHARE / BOCHS_ROM / BOCHS_VGAROM / BOCHS_ROM_ADDR / BOCHS_MEM / BOCHS_DISPLAY /
#    BOCHS_DISPLAY_OPTIONS / RUN_BOOT_CASE
#    退出码：0 全部通过；1 有失败；2 用法错误或判定不了（缺工具、缺镜像、缺模拟器）
set -eu

# 脚本自身位置：据此定位项目根，不依赖调用时的当前目录
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

# —— 默认值 ——
bin_dir=$root/Tools/Bin
out_dir=$root/Out/check
with_boot=0
# 工业验收开关：在启动矩阵与状态演练之外再加三组
accept=0
image_iso=$root/Out/LikesProgram.iso
image_hdd=$root/Out/LikesProgram.hdd
# 4Kn 验收镜像：同一批载荷按 4096 单位组装的另一份产物
image_4kn=$root/Out/LikesProgram.4kn.hdd
# 降级变体镜像：布局描述里置了"记录损坏时退到缺省集"的策略位
image_degrade=$root/Out/LikesProgram.degrade.hdd
# 吊销与防回滚变体镜像：安全版本低于下限（E-CORE-66）、吊销记录损坏（E-CORE-75 WARN）、密钥被吊销（E-CORE-74）
image_revocation_floor=$root/Out/LikesProgram.revocation-floor.hdd
image_revocation_bad=$root/Out/LikesProgram.revocation-bad.hdd
image_revocation_key=$root/Out/LikesProgram.revocation-key.hdd
# 布局清单 JSON：失败注入的偏移从这里读；空表示跟随 --image-hdd / --image-iso 的路径
manifest_hdd=
manifest_iso=
# 证据留存目录：空表示不留证据；给定后必须在项目 Out 之外，结束时复制汇总、日志与元数据
evidence_dir=
# UEFI 固件：OVMF CODE/VARS，每例复制一份 VARS 卷，避免上一例的启动项污染下一例
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
# 32 位 UEFI 固件与 32 位 QEMU：两者都在才跑 IA32 拒绝提示档（BOOTIA32.EFI 只打印 E-UEFI-11）
qemu_i386=${QEMU32:-qemu-system-i386}
ovmf32_code=${OVMF32_CODE:-/usr/share/OVMF/OVMF32_CODE_4M.fd}
ovmf32_vars=${OVMF32_VARS:-/usr/share/OVMF/OVMF32_VARS_4M.fd}
timeout_s=60
qemu=${QEMU:-qemu-system-x86_64}
qemu_mem=${QEMU_MEM:-512}
bochs=${BOCHS:-bochs}
helper=${RUN_BOOT_CASE:-$root/Tools/Bin/RunBootCase}

# 宿主侧的检查工具：顺序即执行顺序，也是表格的行序
host_tools='CheckIpl CheckStub CheckStorage CheckExt4 CheckCrypto CheckSelect CheckMemory CheckRevocation CheckEfiBlockDevice'
# 内核横幅：内核进入 KernelMain 后打出的第一行模块行
# RunBootCase 只看裸子串就能及时停机；但“进内核”这一列的硬判定要用带标签的完整一行：
# 失败诊断的正文里也可能出现 "LikesProgramOS kernel" 字样（如 IA32 提示件），裸子串会误判
kernel_marker='LikesProgramOS kernel'
kernel_banner='[ KERNEL ] LikesProgramOS kernel'

usage() {
    sed -n '2,/^set -eu$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

# 打印用法并退出
die() {
    printf 'check-all: %s\n' "$*" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --with-boot) with_boot=1; shift ;;
        --accept) accept=1; with_boot=1; shift ;;
        --image-4kn) [ $# -ge 2 ] || die "--image-4kn 缺少参数"; image_4kn=$2; shift 2 ;;
        --image-degrade) [ $# -ge 2 ] || die "--image-degrade 缺少参数"; image_degrade=$2; shift 2 ;;
        --image-revocation-floor) [ $# -ge 2 ] || die "--image-revocation-floor 缺少参数"; image_revocation_floor=$2; shift 2 ;;
        --image-revocation-bad) [ $# -ge 2 ] || die "--image-revocation-bad 缺少参数"; image_revocation_bad=$2; shift 2 ;;
        --image-revocation-key) [ $# -ge 2 ] || die "--image-revocation-key 缺少参数"; image_revocation_key=$2; shift 2 ;;
        --manifest-hdd) [ $# -ge 2 ] || die "--manifest-hdd 缺少参数"; manifest_hdd=$2; shift 2 ;;
        --manifest-iso) [ $# -ge 2 ] || die "--manifest-iso 缺少参数"; manifest_iso=$2; shift 2 ;;
        --evidence) [ $# -ge 2 ] || die "--evidence 缺少参数"; evidence_dir=$2; shift 2 ;;
        --bin-dir) [ $# -ge 2 ] || die "--bin-dir 缺少参数"; bin_dir=$2; shift 2 ;;
        --out) [ $# -ge 2 ] || die "--out 缺少参数"; out_dir=$2; shift 2 ;;
        --image-iso) [ $# -ge 2 ] || die "--image-iso 缺少参数"; image_iso=$2; shift 2 ;;
        --image-hdd) [ $# -ge 2 ] || die "--image-hdd 缺少参数"; image_hdd=$2; shift 2 ;;
        --timeout) [ $# -ge 2 ] || die "--timeout 缺少参数"; timeout_s=$2; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) die "无法识别的参数 $1（--help 看用法）" ;;
    esac
done
case "$timeout_s" in
    ''|*[!0-9]*|0) die "--timeout 必须是正整数秒" ;;
esac

# 清单默认跟随镜像路径：显式 --image-hdd / --image-iso 改了路径又没给 --manifest-* 时取 <镜像>.json
[ -n "$manifest_hdd" ] || manifest_hdd=$image_hdd.json
[ -n "$manifest_iso" ] || manifest_iso=$image_iso.json

# 项目根与输出/证据目录都规范成绝对物理路径：
#   · 相对路径按调用时的工作目录展开，消灭 . / .. / 重复斜杠 / 末尾斜杠 / 符号链接，
#     否则 --out Out 与 --out "$root/Out/" 这类写法能绕过下面的相等判断；
#   · 后面 find "$out_dir" -name '*.hdd' -delete 会删掉目录里的镜像副本，所以
#     out_dir 等于项目根或 Out 根时必须在跑起来之前拦下（那会删掉真实产物）
#   · 证据目录不允许在项目 Out 里，也不允许在 out_dir 里（复制会递归到自身）
root_real=$(CDPATH= cd -- "$root" && pwd -P)
mkdir -p -- "$out_dir" || die "--out 无法创建：$out_dir"
out_dir=$(CDPATH= cd -- "$out_dir" && pwd -P)
case "$out_dir" in
    "$root_real"|"$root_real/Out") die "--out 不能指向项目根或 Out 根（会删除真实镜像）" ;;
    /) die "--out 不能指向文件系统根（会删除真实镜像）" ;;
esac
# 输出目录若是项目根的祖先（如 /home、/tmp 的上一级），收尾的 find 同样会扫到真实镜像
case "$root_real/" in
    "$out_dir"/*) die "--out 不能包含项目根（会删除真实镜像）：$out_dir" ;;
esac
if [ -n "$evidence_dir" ]; then
    mkdir -p -- "$evidence_dir" || die "--evidence 无法创建：$evidence_dir"
    evidence_dir=$(CDPATH= cd -- "$evidence_dir" && pwd -P)
    case "$evidence_dir" in
        "$root_real/Out"|"$root_real/Out"/*) die "--evidence 必须在项目 Out 之外：$evidence_dir" ;;
    esac
    case "$evidence_dir/" in
        "$out_dir"/*) die "--evidence 不能位于 --out 目录内：$evidence_dir" ;;
    esac
fi
# 失败注入的偏移来自布局清单 JSON：ACCEPT 一定需要它，缺了就在这里报错，
# 不要等宿主检查与启动矩阵跑完几分钟才死在注入段
if [ "$accept" -eq 1 ] && [ ! -r "$manifest_hdd" ]; then
    die "缺少布局清单 $manifest_hdd；先跑 make（或用 --manifest-hdd 指定）"
fi

host_dir=$out_dir/host
boot_dir=$out_dir/boot
summary=$out_dir/summary.txt
mkdir -p "$host_dir"

# 计数与汇总
host_total=0
host_passed=0
host_cases=0
boot_total=0
boot_passed=0
boot_skipped=0
boot_failed=0
# 引导状态演练的计数：通过数与总数，失败计入 boot_failed（同一个结论出口）
state_total=0
state_passed=0
# 启动矩阵的两种特殊判据：UEFI 档要额外的步骤标记；IA32 档要求出现提示且绝不进内核
boot_require_steps=0
boot_expect_no_kernel=0
# RunBootCase 监视的字面标记：普通档盯内核横幅，IA32 档临时换成 E-UEFI-11
boot_marker=$kernel_marker

# 输出同时进终端与汇总文件
say() {
    printf '%s\n' "$*"
    printf '%s\n' "$*" >> "$summary"
}

# 表格行同样两边都要：先拼好一行再交给它，避免格式串写两遍
row() {
    printf '%s\n' "$1"
    printf '%s\n' "$1" >> "$summary"
}

# 取一个工具日志里的用例计数，按可信度依次退让：
#   1) 工具自报的“N 项…全部通过”；
#   2) 逐例行里的“N/M”编号，取最大 N（工具自己的用例口径）；
#   3) 数逐例通过行（[通过] 行，或行尾以空格加“通过”结束的逐例行）
case_count() {
    reported=$(sed -n 's/.*: \([0-9][0-9]*\) 项.*$/\1/p' "$1" | tail -n 1)
    if [ -n "$reported" ]; then
        printf '%s' "$reported"
        return 0
    fi
    numbered=$(sed -n 's/.*: \([0-9][0-9]*\)\/[0-9][0-9]* .*$/\1/p' "$1" | sort -n | tail -n 1)
    if [ -n "$numbered" ]; then
        printf '%s' "$numbered"
        return 0
    fi
    counted=$(grep -a -c -E '\[通过\]|: .* 通过$' "$1" || true)
    printf '%s' "$counted"
}

# 跑宿主侧的一个工具：run_host <工具名>
run_host() {
    tool=$1
    log=$host_dir/$tool.log
    case "$tool" in
        CheckIpl) set -- --ipl-dir "$root/Packages/Baleen/Ipl/Out/Bin" ;;
        CheckStub) set -- --stub-dir "$root/Packages/Baleen/Stub/Out/Bin" --ipl-dir "$root/Packages/Baleen/Ipl/Out/Bin" ;;
        *) set -- ;;
    esac
    status=通过
    if ! "$bin_dir/$tool" "$@" > "$log" 2>&1; then
        status=失败
    fi
    cases=$(case_count "$log")
    host_total=$((host_total + 1))
    host_cases=$((host_cases + cases))
    if [ "$status" = 通过 ]; then
        host_passed=$((host_passed + 1))
    fi
    row "$(printf '%-18s %-6s %-8s %s' "$tool" "$status" "$cases" "${log#"$root/"}")"
}

# 记一例基础设施缺失：缺模拟器或缺工具时用它，最终退出码为 2
boot_missing() {
    boot_total=$((boot_total + 1))
    boot_skipped=$((boot_skipped + 1))
    row "$(printf '%-22s %-8s %-8s %-8s %s' "$1" 跳过 - - "基础设施缺失：$2")"
}

# 从一例的串口/调试日志里抽三列：进内核、刷新路径、诊断编号
boot_columns() {
    dir=$1
    boot_reached=否
    if grep -a -q -F -- "$kernel_banner" "$dir/serial.log" "$dir/debug.log" 2>/dev/null; then
        boot_reached=是
    fi
    boot_flush=-
    flush_line=$(grep -a -h -o 'Flush=.*' "$dir/serial.log" "$dir/debug.log" 2>/dev/null | head -n 1 | tr -d '\r')
    if [ -n "$flush_line" ]; then
        boot_flush=${flush_line#Flush=}
    elif grep -a -q 'No write cache flush primitive' "$dir/serial.log" "$dir/debug.log" 2>/dev/null; then
        boot_flush='无原语'
    fi
    # 诊断编号按“去重后的行”计数：同一条诊断行会同时出现在串口与调试口两个日志里，
    # 直接对两个文件计数会把一份现场算成两份；先按行去重再数编号
    # 两个文件之间补一个换行：日志最后一行可能没有换行符（停机就在那一行），直接 cat 会把两行粘成一行
    boot_codes=$({ cat "$dir/serial.log" 2>/dev/null; printf '\n'; cat "$dir/debug.log" 2>/dev/null; printf '\n'; } | tr -d '\r' | sort -u |
        grep -a -o -E 'E-[A-Z]+-[0-9][0-9]' | sort | uniq -c | awk '{ printf "%s%s×%s", (NR > 1 ? " " : ""), $2, $1 }')
    [ -n "$boot_codes" ] || boot_codes=无
}

# 跑一例启动：run_boot_case <名称> <目录名> <stdin文件或空> <命令...>
# 用 RunBootCase 盯内核横幅：看到就停模拟器，没看到就等超时；日志一律留在本例目录里
run_boot_case() {
    name=$1
    case_dir=$boot_dir/$2
    input=$3
    shift 3
    mkdir -p "$case_dir"
    # 日志先清空：RunBootCase 按文件长度增量读，残留内容会造成假通过
    : > "$case_dir/serial.log"
    : > "$case_dir/debug.log"
    : > "$case_dir/console.log"
    if [ -n "$input" ]; then
        set -- --stdin "$input" -- "$@"
    else
        set -- -- "$@"
    fi
    boot_status=通过
    if ! "$helper" --timeout "$timeout_s" --marker "$boot_marker" \
            --cwd "$case_dir" --console "$case_dir/console.log" \
            --log "$case_dir/serial.log" --log "$case_dir/debug.log" "$@" > "$case_dir/runner.log" 2>&1; then
        boot_status=失败
    fi
    boot_columns "$case_dir"
    # UEFI 档不只认内核横幅：固件阶段的三个可见边界必须都在日志里，
    # 缺任何一个都说明走的不是“清单已验证 → 退出启动服务 → 放到装载地址”这条完整路径
    if [ "$boot_require_steps" -eq 1 ]; then
        for boot_step in \
            'The kernel set is verified' \
            'The firmware boot services are exited' \
            'The kernel image is placed at its load address'; do
            grep -a -q -F -- "$boot_step" "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null || boot_status=失败
        done
    fi
    # 横幅硬判定：普通档要求进内核；IA32 档反过来，要求“有 E-UEFI-11 停机提示且绝不进内核”
    if [ "$boot_expect_no_kernel" -eq 1 ]; then
        [ "$boot_reached" = 否 ] || boot_status=失败
    else
        [ "$boot_reached" = 是 ] || boot_status=失败
    fi
    boot_total=$((boot_total + 1))
    if [ "$boot_status" = 通过 ]; then
        boot_passed=$((boot_passed + 1))
    else
        boot_failed=$((boot_failed + 1))
    fi
    row "$(printf '%-22s %-8s %-8s %-8s %s' "$name" "$boot_reached" "$boot_flush" "$boot_codes" "$boot_status")"
}

# —— 证据与镜像批次 ——
# 文件 sha256 / 字节数：summary 的批次行与 evidence 的 metadata 用同一口径；文件缺失时给“缺失”
sha256_of() {
    if [ -r "$1" ]; then
        sha256sum "$1" 2>/dev/null | awk '{ print $1 }'
    else
        printf '缺失'
    fi
}

bytes_of() {
    if [ -r "$1" ]; then
        wc -c < "$1" | tr -d '[:space:]'
    else
        printf '%s' -
    fi
}

# 本次运行的镜像批次：summary 开头就记，日志与证据 metadata 靠它对齐到同一批产物
sha_iso=$(sha256_of "$image_iso")
sha_hdd=$(sha256_of "$image_hdd")
sha_4kn=$(sha256_of "$image_4kn")
sha_degrade=$(sha256_of "$image_degrade")
sha_revocation_floor=$(sha256_of "$image_revocation_floor")
sha_revocation_bad=$(sha256_of "$image_revocation_bad")
sha_revocation_key=$(sha256_of "$image_revocation_key")
bytes_iso=$(bytes_of "$image_iso")
bytes_hdd=$(bytes_of "$image_hdd")
bytes_4kn=$(bytes_of "$image_4kn")
bytes_degrade=$(bytes_of "$image_degrade")
bytes_revocation_floor=$(bytes_of "$image_revocation_floor")
bytes_revocation_bad=$(bytes_of "$image_revocation_bad")
bytes_revocation_key=$(bytes_of "$image_revocation_key")

# 写证据元数据：时间、版本、固件与产物的哈希都在这里，供离线复核本次记录对应哪批字节
write_metadata() {
    metadata=$evidence_dir/metadata.txt
    # UEFI 交付件的落点：MakePayloads 从这里把 BOOTX64 / BOOTIA32 装进 ESP 的 Efi.img
    efi_bin_dir=$root/Packages/Baleen/Uefi/Out/Bin
    {
        printf '记录时间(UTC)：%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        printf '项目根：%s\n' "$root"
        printf 'git 版本：%s\n' "$(git -c safe.directory="$root" -C "$root" rev-parse HEAD 2>/dev/null || printf unknown)"
        # 工作树状态：dirty 时本记录不对应任何提交，验收证据必须能一眼看出这一点
        worktree_status=$(git -c safe.directory="$root" -C "$root" status --porcelain 2>/dev/null || true)
        if ! git -c safe.directory="$root" -C "$root" rev-parse HEAD >/dev/null 2>&1; then
            printf 'git 工作树：unknown（git 不可用）\n'
        elif [ -z "$worktree_status" ]; then
            printf 'git 工作树：clean（与提交内容一致）\n'
        else
            printf 'git 工作树：dirty，变更=%s 行 sha256=%s（本记录不对应任何提交）\n' \
                "$(printf '%s\n' "$worktree_status" | wc -l | tr -d ' ')" \
                "$(printf '%s\n' "$worktree_status" | sha256sum | cut -d' ' -f1)"
        fi
        printf '系统架构：%s\n' "$(uname -m)"
        printf 'uname：%s\n' "$(uname -a)"
        if command -v "$qemu" >/dev/null 2>&1; then
            printf 'QEMU：%s\n' "$("$qemu" --version 2>/dev/null | head -n 1)"
        else
            printf 'QEMU：%s（未找到）\n' "$qemu"
        fi
        printf 'OVMF CODE：%s sha256=%s bytes=%s\n' "$ovmf_code" "$(sha256_of "$ovmf_code")" "$(bytes_of "$ovmf_code")"
        printf 'OVMF VARS：%s sha256=%s bytes=%s\n' "$ovmf_vars" "$(sha256_of "$ovmf_vars")" "$(bytes_of "$ovmf_vars")"
        printf 'OVMF32 CODE：%s sha256=%s bytes=%s\n' "$ovmf32_code" "$(sha256_of "$ovmf32_code")" "$(bytes_of "$ovmf32_code")"
        printf 'OVMF32 VARS：%s sha256=%s bytes=%s\n' "$ovmf32_vars" "$(sha256_of "$ovmf32_vars")" "$(bytes_of "$ovmf32_vars")"
        printf '镜像 ISO：%s sha256=%s bytes=%s\n' "$image_iso" "$sha_iso" "$bytes_iso"
        printf '镜像 HDD：%s sha256=%s bytes=%s\n' "$image_hdd" "$sha_hdd" "$bytes_hdd"
        printf '镜像 4Kn：%s sha256=%s bytes=%s\n' "$image_4kn" "$sha_4kn" "$bytes_4kn"
        printf '镜像 degrade：%s sha256=%s bytes=%s\n' "$image_degrade" "$sha_degrade" "$bytes_degrade"
        printf '镜像 revocation-floor：%s sha256=%s bytes=%s\n' "$image_revocation_floor" "$sha_revocation_floor" "$bytes_revocation_floor"
        printf '镜像 revocation-bad：%s sha256=%s bytes=%s\n' "$image_revocation_bad" "$sha_revocation_bad" "$bytes_revocation_bad"
        printf '镜像 revocation-key：%s sha256=%s bytes=%s\n' "$image_revocation_key" "$sha_revocation_key" "$bytes_revocation_key"
        printf 'ESP 固件件 BOOTX64.EFI：%s sha256=%s bytes=%s\n' "$efi_bin_dir/BOOTX64.EFI" "$(sha256_of "$efi_bin_dir/BOOTX64.EFI")" "$(bytes_of "$efi_bin_dir/BOOTX64.EFI")"
        printf 'ESP 固件件 BOOTIA32.EFI：%s sha256=%s bytes=%s\n' "$efi_bin_dir/BOOTIA32.EFI" "$(sha256_of "$efi_bin_dir/BOOTIA32.EFI")" "$(bytes_of "$efi_bin_dir/BOOTIA32.EFI")"
        printf 'ESP 固件件 Efi.img：%s sha256=%s bytes=%s\n' "$efi_bin_dir/Efi.img" "$(sha256_of "$efi_bin_dir/Efi.img")" "$(bytes_of "$efi_bin_dir/Efi.img")"
        # 布局清单记录了各载荷偏移，注入档的注入点由它派生；记下哈希才能复核注入点来源
        printf '布局清单 HDD：%s sha256=%s bytes=%s\n' "$manifest_hdd" "$(sha256_of "$manifest_hdd")" "$(bytes_of "$manifest_hdd")"
        printf '布局清单 ISO：%s sha256=%s bytes=%s\n' "$manifest_iso" "$(sha256_of "$manifest_iso")" "$(bytes_of "$manifest_iso")"
    } > "$metadata"
}

# —— 汇总文件与表头 ——
: > "$summary"
say "check-all：$root"
say "镜像批次：ISO=$sha_iso HDD=$sha_hdd"
say "镜像批次：4KN=$sha_4kn DEGRADE=$sha_degrade"
say "镜像批次：REVOCATION-FLOOR=$sha_revocation_floor REVOCATION-BAD=$sha_revocation_bad REVOCATION-KEY=$sha_revocation_key"
say "输出目录 $out_dir（构建产物：make clean / clean-all 会删除；不是证据目录）"
if [ -n "$evidence_dir" ]; then
    say "证据目录 $evidence_dir（结束时复制本汇总、逐例日志与 metadata.txt）"
fi
say ""
say "宿主检查                                                          用例数"
say "工具               结果   用例    日志"

for tool in $host_tools; do
    [ -x "$bin_dir/$tool" ] || die "缺少检查工具 $bin_dir/$tool；先跑 make test-tools"
    run_host "$tool"
done
say ""
say "宿主小计：通过 $host_passed/$host_total，用例合计 $host_cases"
say ""

# —— 启动矩阵 ——
if [ "$with_boot" -eq 1 ]; then
    for image in "$image_iso" "$image_hdd"; do
        [ -r "$image" ] || die "缺少镜像 $image；先跑 make（或用 --image-iso / --image-hdd 指定）"
    done
    [ -x "$helper" ] || die "缺少运行辅助工具 $helper；先跑 make -C Tools/Test/RunBootCase"
    say "启动矩阵                                                          三列"
    say "介质档                进内核   刷新路径 诊断编号 结果"
    mkdir -p "$boot_dir"

    # QEMU 四档：HDD 直挂、ISO 直挂、HDD 当 U 盘、ISO 写入 U 盘（规格把最后一种列为必查）
    if command -v "$qemu" >/dev/null 2>&1; then
        for spec in hdd-built iso-built hdd-usb iso-usb; do
            media=${spec%%-*}
            mode=${spec#*-}
            if [ "$media" = iso ]; then image=$image_iso; else image=$image_hdd; fi
            case_dir=$boot_dir/$spec
            mkdir -p "$case_dir"
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            if [ "$mode" = usb ]; then
                set -- "$@" -device usb-ehci,id=ehci \
                    -drive "if=none,id=usbstick,format=raw,snapshot=on,file=$image" \
                    -device usb-storage,bus=ehci.0,drive=usbstick -boot order=c
            elif [ "$media" = iso ]; then
                set -- "$@" -drive "if=ide,media=cdrom,format=raw,readonly=on,file=$image" -boot order=d
            else
                set -- "$@" -drive "if=ide,format=raw,snapshot=on,file=$image" -boot order=c
            fi
            if [ "$mode" = usb ]; then
                label="QEMU $media + usb"
            else
                label="QEMU $media + built"
            fi
            run_boot_case "$label" "$spec" '' "$@"
        done
    else
        for spec in hdd-built iso-built hdd-usb iso-usb; do
            boot_missing "$spec" "找不到 QEMU：$qemu"
        done
    fi

    # UEFI 四档：走 OVMF，介质与 BIOS 三档对齐，另加只有 UEFI 固件才看得见的 4Kn U 盘
    # 每档的介质参数：uefi 分支在这里只负责挂介质，固件参数由上面统一加
    for spec in hdd-uefi iso-uefi usb-uefi usb4kn-uefi; do
        uefi_media=${spec%-uefi}
        if ! command -v "$qemu" >/dev/null 2>&1; then
            boot_missing "$spec" "找不到 QEMU：$qemu"
            continue
        fi
        if [ ! -r "$ovmf_code" ] || [ ! -r "$ovmf_vars" ]; then
            boot_missing "$spec" "缺少 OVMF CODE/VARS：$ovmf_code / $ovmf_vars"
            continue
        fi
        if [ "$uefi_media" = iso ]; then image=$image_iso; else image=$image_hdd; fi
        case_dir=$boot_dir/$spec
        mkdir -p "$case_dir"
        # OVMF 的变量卷必须每例一份独立副本，否则上一例的启动项会污染下一例
        cp "$ovmf_vars" "$case_dir/vars.fd"
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
            -drive "if=pflash,format=raw,file=$case_dir/vars.fd" \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        case "$spec" in
            hdd-uefi)
                set -- "$@" -drive "if=ide,format=raw,snapshot=on,file=$image" -boot order=c ;;
            iso-uefi)
                set -- "$@" -drive "if=ide,media=cdrom,format=raw,readonly=on,file=$image" -boot order=d ;;
            usb-uefi)
                set -- "$@" -device usb-ehci,id=ehci \
                    -drive "if=none,id=usbstick,format=raw,snapshot=on,file=$image" \
                    -device usb-storage,bus=ehci.0,drive=usbstick -boot order=c ;;
            usb4kn-uefi)
                # 4Kn：SeaBIOS 不枚举这种设备，只有 UEFI 固件能看到，正是按 8 MiB 那份 ESP 启动
                set -- "$@" -device usb-ehci,id=ehci \
                    -drive "if=none,id=usbstick,format=raw,snapshot=on,file=$image" \
                    -device "usb-storage,bus=ehci.0,drive=usbstick,logical_block_size=4096,physical_block_size=4096" -boot order=c ;;
        esac
        # UEFI 档额外要求三条固件阶段步骤标记，见 run_boot_case
        boot_require_steps=1
        run_boot_case "QEMU $uefi_media + uefi" "$spec" '' "$@"
        boot_require_steps=0
    done

    # IA32 一档：32 位固件只会加载 ESP 里的 BOOTIA32.EFI，那个提示件打印 E-UEFI-11 后停机；
    # 缺 32 位固件或 32 位 QEMU 时按基础设施缺失跳过，不虚增通过数（boot_missing 计入矩阵总数）
    if ! command -v "$qemu_i386" >/dev/null 2>&1; then
        boot_missing "QEMU hdd + uefi32" "找不到 32 位 QEMU：$qemu_i386"
    elif [ ! -r "$ovmf32_code" ] || [ ! -r "$ovmf32_vars" ]; then
        boot_missing "QEMU hdd + uefi32" "缺少 OVMF32 CODE/VARS：$ovmf32_code / $ovmf32_vars"
    else
        case_dir=$boot_dir/hdd-uefi32
        mkdir -p "$case_dir"
        # 变量卷每例一份独立副本，与 64 位 UEFI 档同一规则
        cp "$ovmf32_vars" "$case_dir/vars.fd"
        set -- "$qemu_i386" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -drive "if=pflash,format=raw,readonly=on,file=$ovmf32_code" \
            -drive "if=pflash,format=raw,file=$case_dir/vars.fd" \
            -drive "if=ide,format=raw,snapshot=on,file=$image_hdd" -boot order=c \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        # 这一档的判据与普通档相反：必须看到 E-UEFI-11 提示，且绝不能进内核；
        # RunBootCase 的停止标记也换成 E-UEFI-11（内核横幅的裸子串会出现在提示正文里）
        boot_marker=E-UEFI-11
        boot_expect_no_kernel=1
        run_boot_case "QEMU hdd + uefi32" hdd-uefi32 '' "$@"
        boot_marker=$kernel_marker
        boot_expect_no_kernel=0
    fi

    # Bochs 两档：ATA 硬盘与 ATAPI 光盘（Bochs 没有 USB 存储仿真，也不支持 UEFI）
    bochs_share=${BOCHS_SHARE:-/usr/share/bochs}
    if [ "${BOCHS_ROM+x}" = x ]; then
        bochs_rom=$BOCHS_ROM
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
    for spec in hdd-bochs iso-bochs; do
        media=${spec%%-*}
        if [ "$media" = iso ]; then image=$image_iso; else image=$image_hdd; fi
        if ! command -v "$bochs" >/dev/null 2>&1; then
            boot_missing "$media + bochs" "找不到 Bochs：$bochs"
            continue
        fi
        if [ ! -r "$bochs_rom" ] || [ ! -r "$bochs_vgarom" ]; then
            boot_missing "$media + bochs" "缺少 Bochs BIOS/VGA ROM：$bochs_rom / $bochs_vgarom"
            continue
        fi
        case_dir=$boot_dir/$spec
        mkdir -p "$case_dir"
        printf 'continue\n' > "$case_dir/continue.rc"
        # rfb / vncsrv 要 timeout=0，否则 Bochs 等 VNC 客户端；其余显示库不加选项
        bochs_display=${BOCHS_DISPLAY:-rfb}
        if [ "${BOCHS_DISPLAY_OPTIONS+x}" = x ]; then
            bochs_options=$BOCHS_DISPLAY_OPTIONS
        else
            case "$bochs_display" in
                rfb|vncsrv) bochs_options=timeout=0 ;;
                *) bochs_options= ;;
            esac
        fi
        # cdrom 只接受 path 与 status；mode / journal 是磁盘专有选项
        if [ "$media" = iso ]; then
            ata_line=$(printf 'ata0-master: type=cdrom, path="%s", status=inserted' "$image")
            boot_line='boot: cdrom'
        else
            ata_line=$(printf 'ata0-master: type=disk, path="%s", mode=volatile, journal="%s/disk.redolog"' "$image" "$case_dir")
            boot_line='boot: disk'
        fi
        {
            printf 'romimage: file="%s", address=%s\n' "$bochs_rom" "${BOCHS_ROM_ADDR:-0}"
            printf 'vgaromimage: file="%s"\n' "$bochs_vgarom"
            printf 'megs: %s\n' "${BOCHS_MEM:-128}"
            printf '%s\n' 'config_interface: textconfig' 'sound: driver=dummy' 'speaker: enabled=0'
            printf '%s\n' 'ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14'
            printf '%s\n' "$ata_line"
            printf '%s\n' "$boot_line"
            printf 'display_library: %s' "$bochs_display"
            if [ -n "$bochs_options" ]; then printf ', options="%s"' "$bochs_options"; fi
            printf '\ncom1: enabled=1, mode=file, dev="%s/serial.log"\n' "$case_dir"
            printf 'log: %s/bochs.log\n' "$case_dir"
            printf '%s\n' 'panic: action=fatal'
        } > "$case_dir/bochsrc"
        run_boot_case "$media + bochs" "$spec" "$case_dir/continue.rc" \
            "$bochs" -q -f "$case_dir/bochsrc"
    done
    say ""
    say "启动小计：通过 $boot_passed/$boot_total，失败 $boot_failed，基础设施缺失 $boot_skipped"
    say ""

    # —— 引导状态演练：在介质副本上改状态区，再起一次真实引导，按期望核对 ——
    # 准备工作用 FixBootState：它与引导期选择器、运行期写入方共用同一份 BootState 实现，
    # 因此这里造出来的状态与真实更新流程产生的状态是同一份字节
    # 每一档都在 Out/check/state/ 下用独立副本，不动构建产物；写盘不经 snapshot，好让引导期的写回可见
    fixboot=$root/Tools/Bin/FixBootState
    state_dir=$out_dir/state
    # 状态区的扇区位置：布局描述约定偏移 0x21000，按 512 字节扇区取整
    state_sector=$((0x21000 / 512))
    # 假候选：不存在的 BuildId 与摘要，用于逼出“候选验证失败 → 回退”这条路径
    bogus_build_id=deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef
    bogus_digest=0badc0de0badc0de0badc0de0badc0de0badc0de0badc0de0badc0de0badc0de

    # 缺省集的 BuildId：从镜像的布局描述里取（端到端，不依赖构建产物）
    # 状态演练与验收都要用它造“能通过验证的候选”，因此在这里定义一次
    default_build_id=$(dd if="$image_hdd" bs=1 skip=$((0x20000 + 0x40)) count=255 2>/dev/null | tr -d '\000' |
        sed -n 's#.*/KernelSets/\([0-9a-f]\{64\}\)/.*#\1#p')

    # 把状态区的若干扇区写成不可验证的字节：有写入痕迹但没有有效记录
    state_corrupt() {
        tr '\0' '\377' < /dev/zero | dd bs=512 count="$3" seek="$2" conv=notrunc of="$1" 2>/dev/null
    }

    # 跑一档状态演练：run_state_case <名称> <目录名> <kernel|stop> <期望编号或-> <期望状态或-> <命令...>
    # kernel 档以内核横幅为标记，stop 档以期望编号为标记；两种情况都是“看到标记就停模拟器”
    run_state_case() {
        name=$1
        case_dir=$state_dir/$2
        wanted=$3
        want_code=$4
        want_state=$5
        shift 5
        mkdir -p "$case_dir"
        : > "$case_dir/serial.log"
        : > "$case_dir/debug.log"
        : > "$case_dir/console.log"
        if [ "$wanted" = kernel ]; then marker=$kernel_marker; else marker=$want_code; fi
        state_status=通过
        if ! "$helper" --timeout "$timeout_s" --marker "$marker" \
                --cwd "$case_dir" --console "$case_dir/console.log" \
                --log "$case_dir/serial.log" --log "$case_dir/debug.log" -- "$@" > "$case_dir/runner.log" 2>&1; then
            state_status=失败
        fi
        boot_columns "$case_dir"
        # 期望一：该进内核的必须进，该停机的必须没进
        if [ "$wanted" = kernel ] && [ "$boot_reached" != 是 ]; then state_status=失败; fi
        if [ "$wanted" = stop ] && [ "$boot_reached" = 是 ]; then state_status=失败; fi
        # 期望二：期望编号必须出现在日志里（- 表示不要求出现）
        if [ "$want_code" != - ] && ! printf '%s' "$boot_codes" | grep -q -- "$want_code"; then state_status=失败; fi
        # 期望三：引导期是否按预期改写了状态区（- 表示不检查）
        state_after=-
        if [ "$want_state" != - ]; then
            state_after=$("$fixboot" --image "$case_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*状态=\([A-Za-z]*\).*代次=\([0-9]*\).*/\1:\2/p' | head -n 1)
            [ "$state_after" = "$want_state" ] || state_status=失败
        fi
        state_total=$((state_total + 1))
        if [ "$state_status" = 通过 ]; then
            state_passed=$((state_passed + 1))
        else
            boot_failed=$((boot_failed + 1))
        fi
        printf '%-20s %-8s %-8s %-14s %s\n' "$name" "$boot_reached" "$boot_codes" "${want_state}" "$state_status"
        printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$boot_reached" "$boot_codes" "$want_state" "$state_status" >> "$summary"
    }

    # 4Kn 状态推进的一档：在 4Kn 镜像副本上发布候选，再按 NVMe 4Kn 参数真实引导
    # run_4kn_state_case <名称> <目录名> <候选 BuildId> <期望编号或-> <期望状态或-> <bios|uefi> [publish|clear]
    # 镜像副本与变量卷都每例独立；UEFI 档另加 OVMF pflash 两行，固件缺失时记基础设施缺失
    # 准备动作默认 publish（发布候选）；clear 用于“状态区清空”档，两者都不动构建产物
    run_4kn_state_case() {
        k4_name=$1
        k4_tag=$2
        k4_build_id=$3
        k4_code=$4
        k4_state=$5
        k4_mode=$6
        k4_prepare=${7:-publish}
        if [ "$k4_mode" = uefi ]; then
            if ! command -v "$qemu" >/dev/null 2>&1; then
                boot_missing "$k4_name" "找不到 QEMU：$qemu"
                return 0
            fi
            if [ ! -r "$ovmf_code" ] || [ ! -r "$ovmf_vars" ]; then
                boot_missing "$k4_name" "缺少 OVMF CODE/VARS：$ovmf_code / $ovmf_vars"
                return 0
            fi
        fi
        case_dir=$state_dir/$k4_tag
        mkdir -p "$case_dir"
        cp "$image_4kn" "$case_dir/image.hdd"
        if [ "$k4_prepare" = clear ]; then
            "$fixboot" --image "$case_dir/image.hdd" --clear > "$case_dir/prepare.log" 2>&1
        else
            "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$k4_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
        fi
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot
        if [ "$k4_mode" = uefi ]; then
            cp "$ovmf_vars" "$case_dir/vars.fd"
            set -- "$@" -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                -drive "if=pflash,format=raw,file=$case_dir/vars.fd"
        fi
        set -- "$@" -drive "file=$case_dir/image.hdd,format=raw,if=none,id=n4" \
            -device "nvme,drive=n4,serial=LPOS4K,logical_block_size=4096,physical_block_size=4096" -boot order=c \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        run_state_case "$k4_name" "$k4_tag" kernel "$k4_code" "$k4_state" "$@"
    }

    if [ ! -x "$fixboot" ]; then
        boot_missing "状态演练" "缺少维护工具 $fixboot；先跑 make maintain-tools"
    elif [ ! -r "$image_hdd" ]; then
        boot_missing "状态演练" "缺少磁盘镜像 $image_hdd"
    else
        mkdir -p "$state_dir"
        say "引导状态演练                                                    期望"
        say "演练档               进内核   诊断编号 期望状态       结果"

        for drill in clear corrupt-both pending half-a half-b; do
            case_dir=$state_dir/$drill
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            case "$drill" in
                clear) "$fixboot" --image "$case_dir/image.hdd" --clear > "$case_dir/prepare.log" 2>&1 ;;
                corrupt-both) state_corrupt "$case_dir/image.hdd" "$state_sector" 8 ;;
                pending) "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1 ;;
                half-a) "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
                        state_corrupt "$case_dir/image.hdd" "$state_sector" 1 ;;
                half-b) "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
                        state_corrupt "$case_dir/image.hdd" "$((state_sector + 1))" 1 ;;
            esac
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            case "$drill" in
                clear) run_state_case "状态区清空" "$drill" kernel - Confirmed:1 "$@" ;;
                corrupt-both) run_state_case "全部槽位都损坏" "$drill" stop E-CORE-69 - "$@" ;;
                pending) run_state_case "候选验证失败回退" "$drill" kernel E-CORE-72 Failed:3 "$@" ;;
                half-a) run_state_case "只剩痕迹不猜集" "$drill" stop E-CORE-69 - "$@" ;;
                half-b) run_state_case "坏槽位被忽略" "$drill" kernel E-CORE-72 - "$@" ;;
            esac
        done

        # 写中途断电档：让最新槽位停在"头落盘、尾部与校验和没落"的半截状态
        # 造法：连发两次 Pending（槽位 A 代次 1、槽位 B 代次 2），再把 B 的代次字段抬到 3，
        # 其余字段与校验和保持旧内容——这正是写被切断时落在介质上的样子
        # 读方必须把这个槽位判为不可用并回落到 A，绝不能按它声称的代次选中它
        case_dir=$state_dir/torn-newest
        mkdir -p "$case_dir"
        cp "$image_hdd" "$case_dir/image.hdd"
        "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare-1.log" 2>&1
        "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare-2.log" 2>&1
        printf '\003\000\000\000\000\000\000\000' | dd bs=1 seek=$((0x21000 + 512 + 8)) conv=notrunc of="$case_dir/image.hdd" 2>/dev/null
        printf '撕裂后槽位：'; "$fixboot" --image "$case_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*槽位：\(.*\)/\1/p'
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        run_state_case "撕裂最新槽位" torn-newest kernel E-CORE-72 Failed:3 "$@"

        # 七个槽位都只落了头 32 字节（尾巴与校验和是旧的）：必须仍从唯一完好的那个启动
        case_dir=$state_dir/torn-heads
        mkdir -p "$case_dir"
        cp "$image_hdd" "$case_dir/image.hdd"
        for i in 1 2 3 4 5 6 7 8; do
            "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare-$i.log" 2>&1
        done
        # 八个槽位此时都有效；取最后一个槽位的头部当"新记录的头"，覆盖到前七个上并把代次抬高
        dd if="$case_dir/image.hdd" of="$case_dir/head.bin" bs=1 skip=$((0x21000 + 7 * 512)) count=32 2>/dev/null
        slot=0
        while [ "$slot" -lt 7 ]; do
            seek=$((0x21000 + slot * 512))
            dd if="$case_dir/head.bin" of="$case_dir/image.hdd" bs=1 seek=$seek conv=notrunc 2>/dev/null
            oct=$(printf '%03o' $((20 + slot)))
            printf "\\$oct\000\000\000\000\000\000\000" | dd bs=1 seek=$((seek + 8)) conv=notrunc of="$case_dir/image.hdd" 2>/dev/null
            slot=$((slot + 1))
        done
        printf '撕裂后槽位：'; "$fixboot" --image "$case_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*槽位：\(.*\)/\1/p'
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        run_state_case "七槽只有半条记录" torn-heads kernel E-CORE-72 - "$@"

        # 刷新覆盖档：可刷新的端口是不是启动介质所在的设备——三种情况必须分得开
        # 判据是那一行 INFO：找到端口但核验不成立时才会出现；覆盖成立、或根本没有端口都不出现
        coverage_case() {
            cover_name=$1
            cover_tag=$2
            cover_expect=$3
            cover_commit=$4
            shift 4
            cover_dir=$state_dir/coverage-$cover_tag
            mkdir -p "$cover_dir"
            cover_status=通过
            if ! "$helper" --timeout "$timeout_s" --marker "$kernel_marker" --cwd "$cover_dir" \
                    --console "$cover_dir/console.log" \
                    --log "$cover_dir/serial.log" --log "$cover_dir/debug.log" -- "$@" > "$cover_dir/runner.log" 2>&1; then
                cover_status=失败
            fi
            boot_columns "$cover_dir"
            [ "$boot_reached" = 是 ] || cover_status=失败
            if grep -aq "isn't this medium's device" "$cover_dir/serial.log"; then cover_got=未覆盖; else cover_got=覆盖; fi
            [ "$cover_got" = "$cover_expect" ] || cover_status=失败
            # 覆盖成立且真的提交过时，不得报持久化边界；未覆盖时报了才对
            cover_warn=无
            printf '%s' "$boot_codes" | grep -q 'E-CORE-71' && cover_warn=E-CORE-71
            if [ "$cover_expect" = 覆盖 ]; then
                [ "$cover_warn" = 无 ] || cover_status=失败
                if [ "$cover_commit" = 提交 ]; then
                    cover_done=$("$fixboot" --image "$cover_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*已试次数=\([0-9]*\).*/\1/p' | head -1)
                    [ "$cover_done" = 1 ] || cover_status=失败
                fi
            fi
            state_total=$((state_total + 1))
            if [ "$cover_status" = 通过 ]; then state_passed=$((state_passed + 1)); else boot_failed=$((boot_failed + 1)); fi
            printf '%-20s %-8s %-8s %-14s %s\n' "覆盖 $cover_name" "$boot_reached" "$cover_got" "$cover_expect" "$cover_status"
            printf '%s\t%s\t%s\t%s\t%s\n' "覆盖 $cover_name" "$boot_reached" "$cover_got" "$cover_expect" "$cover_status" >> "$summary"
        }

        # 覆盖成立 + 真提交一次：介质就是那块 AHCI 盘，身份核验通过，不报持久化边界
        case_dir=$state_dir/coverage-hdd
        mkdir -p "$case_dir"
        cp "$image_hdd" "$case_dir/image.hdd"
        "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$default_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        coverage_case "hdd 本盘" hdd 覆盖 提交 "$@"

        # 找到端口但启动介质是光盘：刷的是机器里那块 ATA 盘，核验必须报未覆盖
        # 光盘只读、不会有提交，所以这一档的判据就是那行 INFO
        if [ -r "$image_iso" ]; then
            case_dir=$state_dir/coverage-iso
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/other.hdd"
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "if=none,id=cd0,media=cdrom,format=raw,readonly=on,file=$image_iso" \
                -device ide-cd,bus=ide.1,drive=cd0 \
                -drive "file=$case_dir/other.hdd,format=raw,if=ide" -boot order=d \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            coverage_case "光盘+他人硬盘" iso 未覆盖 不提交 "$@"
        else
            boot_missing "覆盖核验 光盘+他人硬盘" "缺少 $image_iso"
        fi

        # 设备级写缓存档：USB 在 BIOS 侧没有刷新原语（`无原语`），提交只到设备缓存
        # 需要缺省集 BuildId 来造一个能通过验证的候选：读不到就跳过并声明基础设施缺失
        if [ -z "$default_build_id" ]; then
            boot_missing "设备级写缓存" "读不到缺省集 BuildId"
        else
        # 这一档验证两件事：持久化边界必须报到现场；缓存丢失只丢一次转换，不丢完整性
        # “丢缓存”的造法：启动前把状态区 8 个扇区存一份，启动后原样写回——设备没把缓存落到介质上的效果就是它
        # 这一档验证两件事：持久化边界必须报到现场；缓存丢失只丢一次转换，不丢完整性
        # "丢缓存"的造法：启动前把状态区 8 个扇区存一份，启动后原样写回——设备没把缓存落到介质上的效果就是它
        case_dir=$state_dir/usb-cache-loss
        mkdir -p "$case_dir"
        cp "$image_hdd" "$case_dir/image.hdd"
        "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$default_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
        dd if="$case_dir/image.hdd" of="$case_dir/area-before.bin" bs=512 skip=$state_sector count=8 2>/dev/null
        printf '启动前槽位：'; "$fixboot" --image "$case_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*槽位：\(.*\)/\1/p'
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -device usb-ehci,id=ehci \
            -drive "if=none,id=usbstick,format=raw,file=$case_dir/image.hdd" \
            -device usb-storage,bus=ehci.0,drive=usbstick -boot order=c \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        run_state_case "USB 无刷新原语" usb-cache-loss kernel E-CORE-71 Trying:2 "$@"
        # 丢缓存：把状态区恢复成启动前的内容，再起一次——必须照常启动，且不得出现 E-CORE-69
        dd if="$case_dir/area-before.bin" of="$case_dir/image.hdd" bs=512 seek=$state_sector conv=notrunc 2>/dev/null
        printf '丢缓存后槽位：'; "$fixboot" --image "$case_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*槽位：\(.*\)/\1/p'
        # 第二次启动放到自己的目录：RunBootCase 盯的是它自己的 serial.log，日志落点必须与它一致
        case2_dir=$state_dir/usb-cache-loss-2
        mkdir -p "$case2_dir"
        cp "$case_dir/image.hdd" "$case2_dir/image.hdd"
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -device usb-ehci,id=ehci \
            -drive "if=none,id=usbstick,format=raw,file=$case2_dir/image.hdd" \
            -device usb-storage,bus=ehci.0,drive=usbstick -boot order=c \
            -serial "file:$case2_dir/serial.log" -debugcon "file:$case2_dir/debug.log"
        run_state_case "丢缓存后重来" usb-cache-loss-2 kernel E-CORE-71 Trying:2 "$@"
        fi

        # 环形收益档：状态区切成 8 个 512 字节槽位，坏掉 7 个仍要能启动
        # 造法：先发布候选落在槽位 A，再把槽位 B..H 逐个写成不可验证（模拟连续七次各毁一个槽位的断电）
        case_dir=$state_dir/ring-seven
        mkdir -p "$case_dir"
        cp "$image_hdd" "$case_dir/image.hdd"
        "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
        state_corrupt "$case_dir/image.hdd" "$((state_sector + 1))" 7
        printf '槽位：'; "$fixboot" --image "$case_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*槽位：\(.*\)/\1/p'
        set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
            -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        run_state_case "坏 7 槽仍可启动" ring-seven kernel E-CORE-72 - "$@"

        # 降级档：介质声明了策略位，记录损坏时退到缺省集而不是停机
        # 两档一起看才有意义——同一个损坏状态，没有策略位时是 E-CORE-69 停机（上面那档），有策略位时降级启动
        for drill in degrade-bios degrade-uefi; do
            # 干净树上变体镜像可能还没构建：缺镜像按基础设施缺失跳过两档，
            # 不能让 cp 失败被 set -eu 直接杀掉整轮检查
            if [ ! -r "$image_degrade" ]; then
                if [ "$drill" = degrade-bios ]; then
                    boot_missing "降级策略位 + bios" "缺少 $image_degrade；先跑 make（或用 --image-degrade 指定）"
                else
                    boot_missing "降级策略位 + uefi" "缺少 $image_degrade；先跑 make（或用 --image-degrade 指定）"
                fi
                continue
            fi
            case_dir=$state_dir/$drill
            mkdir -p "$case_dir"
            cp "$image_degrade" "$case_dir/image.hdd"
            state_corrupt "$case_dir/image.hdd" "$state_sector" 8
            if [ "$drill" = degrade-bios ]; then
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                    -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
                run_state_case "降级策略位 + bios" "$drill" kernel E-CORE-56 Confirmed:1 "$@"
            else
                if command -v "$qemu" >/dev/null 2>&1 && [ -r "$ovmf_code" ] && [ -r "$ovmf_vars" ]; then
                    cp "$ovmf_vars" "$case_dir/vars.fd"
                    set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                        -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                        -drive "if=pflash,format=raw,file=$case_dir/vars.fd" \
                        -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                        -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
                    run_state_case "降级策略位 + uefi" "$drill" kernel E-UEFI-56 Confirmed:1 "$@"
                else
                    boot_missing "降级策略位 + uefi" "缺少 QEMU 或 OVMF"
                fi
            fi
        done

        # UEFI 侧同一档：验证固件环境的写入路径（块设备写 + FlushBlocks）也把状态推进了
        if command -v "$qemu" >/dev/null 2>&1 && [ -r "$ovmf_code" ] && [ -r "$ovmf_vars" ]; then
            case_dir=$state_dir/pending-uefi
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
            cp "$ovmf_vars" "$case_dir/vars.fd"
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                -drive "if=pflash,format=raw,file=$case_dir/vars.fd" \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_state_case "候选失败回退 + UEFI" "pending-uefi" kernel E-UEFI-72 Failed:3 "$@"
        else
            boot_missing "状态演练 + UEFI" "缺少 QEMU 或 OVMF"
        fi

        # 4Kn 状态推进：块设备契约要求偏移与长度是逻辑块的整数倍，4Kn NVMe 上 512 字节槽位写会被
        # 块设备以 InvalidArgument 拒绝，空状态能启动看不出这一点——必须把状态转换在 4Kn 介质上走一遍
        # 验收模式才跑：候选 BuildId 从 4Kn 镜像自己的布局描述里取，digest 用上面那个假摘要
        if [ "$accept" -eq 1 ]; then
            if [ ! -r "$image_4kn" ]; then
                boot_missing "4Kn 状态推进（五档）" "缺少 $image_4kn；先跑 make 的 4Kn 依赖或用 --image-4kn 指定"
            else
                build_id_4kn=$(dd if="$image_4kn" bs=1 skip=$((0x20000 + 0x40)) count=255 2>/dev/null | tr -d '\000' |
                    sed -n 's#.*/KernelSets/\([0-9a-f]\{64\}\)/.*#\1#p')
                if [ -z "$build_id_4kn" ]; then
                    boot_missing "4Kn 状态推进（五档）" "读不到 $image_4kn 的缺省集 BuildId"
                else
                    # 有效候选：Pending 要推进到 Trying；BIOS 侧 NVMe 无刷新原语，提交真实发生后必报 E-CORE-71
                    run_4kn_state_case "4Kn 状态推进 + bios" 4kn-pending-bios "$build_id_4kn" E-CORE-71 Trying:2 bios
                    # UEFI 侧有 FlushBlocks 原语：状态同样要推进，但不得报持久化边界
                    run_4kn_state_case "4Kn 状态推进 + uefi" 4kn-pending-uefi "$build_id_4kn" - Trying:2 uefi
                    # 坏候选：候选验证失败后 Failed 必须同样落到 4Kn 介质上
                    run_4kn_state_case "4Kn 候选失败回退 + bios" 4kn-failed-bios "$bogus_build_id" E-CORE-72 Failed:3 bios
                    run_4kn_state_case "4Kn 候选失败回退 + uefi" 4kn-failed-uefi "$bogus_build_id" E-UEFI-72 Failed:3 uefi
                    # 状态区清空：空区域首次启动要在 4Kn 上新建 Confirmed 记录并真实落盘（不是只读内存状态）
                    run_4kn_state_case "4Kn 状态区清空" 4kn-clear "$bogus_build_id" - Confirmed:1 bios clear
                fi
            fi
        fi

        # 维修档：坏副本被另一份重建，不引导，只看两份是否都回到有效
        case_dir=$state_dir/repair
        mkdir -p "$case_dir"
        cp "$image_hdd" "$case_dir/image.hdd"
        "$fixboot" --image "$case_dir/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
        state_corrupt "$case_dir/image.hdd" "$((state_sector + 4))" 4
        if "$fixboot" --image "$case_dir/image.hdd" --repair > "$case_dir/repair.log" 2>&1 &&
           [ "$("$fixboot" --image "$case_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*槽位：\(.*\)/\1/p' | tr ' ' '\n' | grep -c '^[A-H]=有效')" -eq 8 ]; then
            state_total=$((state_total + 1)); state_passed=$((state_passed + 1))
            printf '%-20s %-8s %-8s %-14s %s\n' "维修重建坏槽位" - - 全部槽位有效 通过
            printf '%s\t%s\t%s\t%s\t%s\n' "维修重建坏槽位" - - 全部槽位有效 通过 >> "$summary"
        else
            state_total=$((state_total + 1))
            boot_failed=$((boot_failed + 1))
            printf '%-20s %-8s %-8s %-14s %s\n' "维修重建坏槽位" - - 全部槽位有效 失败
            printf '%s\t%s\t%s\t%s\t%s\n' "维修重建坏槽位" - - 全部槽位有效 失败 >> "$summary"
        fi

        # 留证据、省空间：每档的最终状态报告落成文本，镜像副本删掉（要复跑就再跑一次本脚本）
        for case_dir in "$state_dir"/*; do
            [ -d "$case_dir" ] || continue
            [ -f "$case_dir/image.hdd" ] || continue
            "$fixboot" --image "$case_dir/image.hdd" --report > "$case_dir/state-report.txt" 2>&1
            rm -f "$case_dir/image.hdd"
        done
        # 验收与覆盖核验留下的副本同理：日志与逐例报告已经落盘，副本不必留（一次全量跑会攒几百兆）
        # 全部 .hdd 都是本脚本造的副本（真产物在 Out/ 下，不在这个目录里），收尾一并回收
        find "$out_dir" -name '*.hdd' -type f -delete 2>/dev/null || true

        say ""
        say "状态演练小计：通过 $state_passed/$state_total（每档最终状态见 Out/check/state/<档>/state-report.txt）"
        say ""
    fi

    # —— 工业验收：失败注入、介质与控制器、引导状态机多轮 ——
    if [ "$accept" -eq 1 ]; then
        accept_dir=$out_dir/accept
        mkdir -p "$accept_dir"
        accept_total=0
        accept_passed=0

        # 往镜像的绝对偏移写一串字节：字节以八进制转义给出（POSIX printf 只认 \ooo）
        inject_bytes() {
            printf "$3" | dd bs=1 seek="$2" conv=notrunc of="$1" 2>/dev/null
        }

        # 把十进制值写成 printf 认的三位八进制转义 \ooo：必须恰好三位，
        # 多一位会吃掉下一个转义的反斜杠，少一位会把后随数字并进同一个字符
        octal_escape() {
            printf '\\%03o' "$1"
        }

        # 重算布局描述的校验和，让改过的布局本身仍然合法
        # 布局块在 0x20000，校验和在 +0x140（4 字节小端，覆盖 [0x20000, 0x200140) 逐字节无符号和取低 32 位）
        refresh_layout_checksum() {
            refresh_image=$1
            refresh_sum=$(od -An -tu1 -v -j $((0x20000)) -N $((0x140)) "$refresh_image" | tr -s ' ' '\n' |
                awk '{ sum += $1 } END { printf "%d", sum }')
            refresh_byte0=$(octal_escape $((refresh_sum % 256)))
            refresh_byte1=$(octal_escape $(((refresh_sum / 256) % 256)))
            refresh_byte2=$(octal_escape $(((refresh_sum / 65536) % 256)))
            refresh_byte3=$(octal_escape $((refresh_sum / 16777216)))
            inject_bytes "$refresh_image" "$((0x20000 + 0x140))" "$refresh_byte0$refresh_byte1$refresh_byte2$refresh_byte3"
        }

        # 改写镜像布局描述的内核路径，并重算布局校验和让布局本身仍然合法
        # 路径字节数在 +0x3C（2 字节小端）、路径在 +0x40（256 字节）
        patch_layout_kernel_path() {
            patch_image=$1
            patch_path=$2
            # 路径区先整段清零再写新路径：残留的旧尾巴会被读方当成路径的一部分
            dd if=/dev/zero of="$patch_image" bs=1 count=256 seek=$((0x20000 + 0x40)) conv=notrunc 2>/dev/null
            printf '%s' "$patch_path" | dd of="$patch_image" bs=1 seek=$((0x20000 + 0x40)) conv=notrunc 2>/dev/null
            # 路径字节数按 2 字节小端写回：上限 255，高字节恒为 0
            patch_bytes=$(printf '%s' "$patch_path" | wc -c | tr -d '[:space:]')
            inject_bytes "$patch_image" "$((0x20000 + 0x3C))" \
                "$(octal_escape $((patch_bytes % 256)))$(octal_escape $((patch_bytes / 256)))"
            refresh_layout_checksum "$patch_image"
        }

        # 改写布局描述的状态区域长度（StateBytes，+0x38，4 字节小端），并重算校验和
        patch_layout_state_bytes() {
            patch_image=$1
            patch_bytes=$2
            inject_bytes "$patch_image" "$((0x20000 + 0x38))" \
                "$(octal_escape $((patch_bytes % 256)))$(octal_escape $(((patch_bytes / 256) % 256)))$(octal_escape $(((patch_bytes / 65536) % 256)))$(octal_escape $((patch_bytes / 16777216)))"
            refresh_layout_checksum "$patch_image"
        }

        # 工业验收的一档：expect 为 code:E-XXX-nn（日志里必须出现该编号且不进内核）
        # 或 ipl:N（IPL 只输出一位十六进制码到 0xE9，且绝不进内核）
        run_accept_case() {
            name=$1
            case_dir=$accept_dir/$2
            expect=$3
            shift 3
            mkdir -p "$case_dir"
            : > "$case_dir/serial.log"
            : > "$case_dir/debug.log"
            : > "$case_dir/console.log"
            accept_status=通过
            case "$expect" in
                code:*)
                    # 期望停机的档以期望编号为标记：日志里出现它就停模拟器
                    if ! "$helper" --timeout "$timeout_s" --marker "${expect#code:}" \
                            --cwd "$case_dir" --console "$case_dir/console.log" \
                            --log "$case_dir/serial.log" --log "$case_dir/debug.log" -- "$@" > "$case_dir/runner.log" 2>&1; then
                        accept_status=失败
                    fi
                    ;;
                *)
                    # IPL 档没有可等的文本标记（它只往 0xE9 写一位十六进制码），用固定上限跑完再看日志
                    timeout 20 "$@" > "$case_dir/runner.log" 2>&1 || true
                    ;;
            esac
            boot_columns "$case_dir"
            case "$expect" in
                code:*)
                    want=${expect#code:}
                    [ "$boot_reached" = 否 ] || accept_status=失败
                    printf '%s' "$boot_codes" | grep -q -- "$want" || accept_status=失败
                    ;;
                ipl:*)
                    want=${expect#ipl:}
                    [ "$boot_reached" = 否 ] || accept_status=失败
                    # IPL 阶段只有一位十六进制码走 0xE9：调试口内容去掉空白后必须就是它
                    got=$(tr -d '\r\n\000' < "$case_dir/debug.log" | tr -d '[:space:]')
                    [ "$got" = "$want" ] || accept_status=失败
                    boot_codes=$got
                    ;;
            esac
            accept_total=$((accept_total + 1))
            if [ "$accept_status" = 通过 ]; then
                accept_passed=$((accept_passed + 1))
            else
                boot_failed=$((boot_failed + 1))
            fi
            printf '%-20s %-8s %-14s %-14s %s\n' "$name" "$boot_reached" "$boot_codes" "$expect" "$accept_status"
            printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$boot_reached" "$boot_codes" "$expect" "$accept_status" >> "$summary"
        }

        # 介质与控制器的一档：与启动矩阵同规格，只是挂在不同控制器上
        run_media_case() {
            name=$1
            case_dir=$accept_dir/$2
            shift 2
            mkdir -p "$case_dir"
            : > "$case_dir/serial.log"
            : > "$case_dir/debug.log"
            : > "$case_dir/console.log"
            accept_status=通过
            if ! "$helper" --timeout "$timeout_s" --marker "$kernel_marker" \
                    --cwd "$case_dir" --console "$case_dir/console.log" \
                    --log "$case_dir/serial.log" --log "$case_dir/debug.log" -- "$@" > "$case_dir/runner.log" 2>&1; then
                accept_status=失败
            fi
            boot_columns "$case_dir"
            [ "$boot_reached" = 是 ] || accept_status=失败
            accept_total=$((accept_total + 1))
            if [ "$accept_status" = 通过 ]; then
                accept_passed=$((accept_passed + 1))
            else
                boot_failed=$((boot_failed + 1))
            fi
            printf '%-20s %-8s %-14s %-14s %s\n' "$name" "$boot_reached" "$boot_codes" "$boot_flush" "$accept_status"
            printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$boot_reached" "$boot_codes" "$boot_flush" "$accept_status" >> "$summary"
        }

        # 工业验收的一档（WARN 档）：必须进内核，且日志里必须出现指定编号
        # 用于“状态写不进去 / 吊销记录不可用之后仍按降级路径继续启动”这类现场
        run_accept_warn_case() {
            name=$1
            case_dir=$accept_dir/$2
            want_code=$3
            shift 3
            mkdir -p "$case_dir"
            : > "$case_dir/serial.log"
            : > "$case_dir/debug.log"
            : > "$case_dir/console.log"
            accept_status=通过
            if ! "$helper" --timeout "$timeout_s" --marker "$kernel_marker" \
                    --cwd "$case_dir" --console "$case_dir/console.log" \
                    --log "$case_dir/serial.log" --log "$case_dir/debug.log" -- "$@" > "$case_dir/runner.log" 2>&1; then
                accept_status=失败
            fi
            boot_columns "$case_dir"
            [ "$boot_reached" = 是 ] || accept_status=失败
            printf '%s' "$boot_codes" | grep -q -- "$want_code" || accept_status=失败
            accept_total=$((accept_total + 1))
            if [ "$accept_status" = 通过 ]; then
                accept_passed=$((accept_passed + 1))
            else
                boot_failed=$((boot_failed + 1))
            fi
            printf '%-20s %-8s %-14s %-14s %s\n' "$name" "$boot_reached" "$boot_codes" "进内核+$want_code" "$accept_status"
            printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$boot_reached" "$boot_codes" "进内核+$want_code" "$accept_status" >> "$summary"
        }

        # 失败注入的偏移来自构建产物的布局清单 JSON：产物布局一变，硬编码偏移就会打到别的区
        # 只取需要的四个区起点（TestInstaller / MakeImage 写出的稳定格式），并逐点按镜像大小校验
        [ -r "$manifest_hdd" ] || die "缺少布局清单 $manifest_hdd；先跑 make（或用 --manifest-hdd 指定）"
        layout_offset=$(sed -n 's/.*"layout": {"offset": \([0-9][0-9]*\).*/\1/p' "$manifest_hdd" | head -n 1)
        descriptor_offset=$(sed -n 's/.*"descriptor_area": {"offset": \([0-9][0-9]*\).*/\1/p' "$manifest_hdd" | head -n 1)
        stub_offset=$(sed -n 's/.*"stub": {"offset": \([0-9][0-9]*\), "bytes": \([0-9][0-9]*\).*/\1/p' "$manifest_hdd" | head -n 1)
        stub_region_bytes=$(sed -n 's/.*"stub": {"offset": \([0-9][0-9]*\), "bytes": \([0-9][0-9]*\).*/\2/p' "$manifest_hdd" | head -n 1)
        core_offset=$(sed -n 's/.*"core": {"offset": \([0-9][0-9]*\), "bytes": \([0-9][0-9]*\).*/\1/p' "$manifest_hdd" | head -n 1)
        core_region_bytes=$(sed -n 's/.*"core": {"offset": \([0-9][0-9]*\), "bytes": \([0-9][0-9]*\).*/\2/p' "$manifest_hdd" | head -n 1)
        if [ -z "$layout_offset" ] || [ -z "$descriptor_offset" ] || [ -z "$stub_offset" ] || [ -z "$stub_region_bytes" ] ||
           [ -z "$core_offset" ] || [ -z "$core_region_bytes" ]; then
            die "$manifest_hdd 里读不到 layout / descriptor_area / stub / core 偏移；格式不对，先跑 make 重新生成"
        fi
        image_hdd_bytes=$(wc -c < "$image_hdd" | tr -d '[:space:]')
        # 摘要注入点取载荷区中段：优先 +8192，超出本区范围时退到半程；最后统一按文件大小兜底
        core_digest_offset=$((core_offset + 8192))
        [ $((core_digest_offset + 1)) -le $((core_offset + core_region_bytes)) ] ||
            core_digest_offset=$((core_offset + core_region_bytes / 2))
        stub_digest_offset=$((stub_offset + 8192))
        [ $((stub_digest_offset + 1)) -le $((stub_offset + stub_region_bytes)) ] ||
            stub_digest_offset=$((stub_offset + stub_region_bytes / 2))
        for inject_point in "layout-magic:$layout_offset:4" "layout-checksum:$((layout_offset + 16)):1" \
                            "descriptor-head:$descriptor_offset:1" "core-header:$((core_offset + 16)):1" \
                            "core-digest:$core_digest_offset:1" "stub-digest:$stub_digest_offset:1"; do
            inject_point_name=${inject_point%%:*}
            inject_point_rest=${inject_point#*:}
            inject_point_offset=${inject_point_rest%%:*}
            inject_point_need=${inject_point_rest#*:}
            if [ $((inject_point_offset + inject_point_need)) -gt "$image_hdd_bytes" ]; then
                die "注入点 $inject_point_name 越界：偏移 $inject_point_offset + $inject_point_need > $image_hdd 的 $image_hdd_bytes 字节；$manifest_hdd 与镜像不是同一批产物"
            fi
        done

        say "工业验收：失败注入"
        say "注入档               进内核   诊断编号       期望           结果"
        # 注入点全部由上面 JSON 的区起点派生（十进制，运行期不做 0x 常量算术）：
        # 布局魔数=layout 起点、布局校验和=layout 起点+16（改校验覆盖区里的卷标识首字节）、
        # 描述符区首字节、Stub 头=core 起点+16、两份摘要=各载荷区中段一个字节
        for inject_name in layout-magic layout-checksum core-header core-digest stub-digest descriptor-head; do
            case "$inject_name" in
                layout-magic) inject_offset=$layout_offset; inject_byte='\000'; inject_want=code:E-CORE-18 ;;
                layout-checksum) inject_offset=$((layout_offset + 16)); inject_byte='\377'; inject_want=code:E-CORE-24 ;;
                core-header) inject_offset=$((core_offset + 16)); inject_byte='\000'; inject_want=code:E-STUB-40 ;;
                core-digest) inject_offset=$core_digest_offset; inject_byte='\252'; inject_want=code:E-STUB-51 ;;
                stub-digest) inject_offset=$stub_digest_offset; inject_byte='\252'; inject_want=code:E-STUB-51 ;;
                descriptor-head) inject_offset=$descriptor_offset; inject_byte='\000'; inject_want=ipl:3 ;;
            esac
            case_dir=$accept_dir/inject-$inject_name
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            inject_bytes "$case_dir/image.hdd" "$inject_offset" "$inject_byte"
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_accept_case "注入 $inject_name" "inject-$inject_name" "$inject_want" "$@"
            rm -f "$case_dir/image.hdd"
        done

        # 布局路径篡改两档：只改布局描述的内核路径并重算校验和，布局本身必须仍然通过校验；
        # 装载路径只认验签通过清单登记的 .os 条目，形态不符或与清单不符都必须拒绝启动，
        # 不再有“布局路径解析失败就未经验证直启”的回退
        if command -v "$qemu" >/dev/null 2>&1; then
            # 档一：路径指回选版启用前的扁平落点，推不出 KernelSets/<BuildId>/ 形态 → E-CORE-84
            case_dir=$accept_dir/inject-flat-path
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            patch_layout_kernel_path "$case_dir/image.hdd" '/LikesProgram/System/LikesProgram.os'
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_accept_case "注入 layout-flat-path" inject-flat-path code:E-CORE-84 "$@"
            rm -f "$case_dir/image.hdd"

            # 档二：保留集目录、只把产物名换成清单没登记的 Other.os：
            # 清单验签通过之后，布局路径与登记产物比对不符 → E-CORE-85
            layout_path=$(dd if="$image_hdd" bs=1 skip=$((0x20000 + 0x40)) count=255 2>/dev/null | tr -d '\000')
            case_dir=$accept_dir/inject-layout-artifact
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            patch_layout_kernel_path "$case_dir/image.hdd" "${layout_path%/*}/Other.os"
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_accept_case "注入 layout-artifact" inject-layout-artifact code:E-CORE-85 "$@"
            rm -f "$case_dir/image.hdd"

            # 档三：布局路径语法非法（双斜杠产生空分量），布局校验必须在选版之前拒绝 → E-CORE-93
            case_dir=$accept_dir/inject-layout-path-syntax
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            patch_layout_kernel_path "$case_dir/image.hdd" '/LikesProgram//System/LikesProgram.os'
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_accept_case "注入 layout-path-syntax" inject-layout-path-syntax code:E-CORE-93 "$@"
            rm -f "$case_dir/image.hdd"
        else
            boot_missing "注入 layout-flat-path" "找不到 QEMU：$qemu"
            boot_missing "注入 layout-artifact" "找不到 QEMU：$qemu"
            boot_missing "注入 layout-path-syntax" "找不到 QEMU：$qemu"
        fi

        # 状态区域参数非法档：把布局描述的 StateBytes 改成 2048（不足一个区域）并重算校验和，
        # 布局本身合法；选版不能把“参数非法”当成“无记录”按零事实启动，必须以 E-CORE-90 停机
        if command -v "$qemu" >/dev/null 2>&1; then
            case_dir=$accept_dir/inject-state-area
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            patch_layout_state_bytes "$case_dir/image.hdd" 2048
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_accept_case "注入 state-area-size" inject-state-area code:E-CORE-90 "$@"
            rm -f "$case_dir/image.hdd"
        else
            boot_missing "注入 state-area-size" "找不到 QEMU：$qemu"
        fi

        # UEFI 侧同一档：布局路径指回扁平落点，OVMF 引导必须报同号异前缀的 E-UEFI-84 且不进内核
        if command -v "$qemu" >/dev/null 2>&1 && [ -r "$ovmf_code" ] && [ -r "$ovmf_vars" ]; then
            case_dir=$accept_dir/inject-layout-flat-path-uefi
            mkdir -p "$case_dir"
            cp "$image_hdd" "$case_dir/image.hdd"
            patch_layout_kernel_path "$case_dir/image.hdd" '/LikesProgram/System/LikesProgram.os'
            cp "$ovmf_vars" "$case_dir/vars.fd"
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                -drive "if=pflash,format=raw,file=$case_dir/vars.fd" \
                -drive "file=$case_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_accept_case "注入 layout-flat-path + uefi" inject-layout-flat-path-uefi code:E-UEFI-84 "$@"
            rm -f "$case_dir/image.hdd"
        else
            boot_missing "注入 layout-flat-path + uefi" "缺少 QEMU 或 OVMF"
        fi

        # 只读介质 + Pending 候选：状态写入在只读介质上必然失败，必须报 E-CORE-70 WARN 后
        # 按降级路径继续启动；判据是“进内核 + 出现该编号”，不是只看状态列
        say ""
        say "工业验收：只读介质与状态写入"
        say "验收档               进内核   诊断编号       期望           结果"
        if ! command -v "$qemu" >/dev/null 2>&1; then
            boot_missing "只读 ISO + Pending" "找不到 QEMU：$qemu"
        elif [ ! -x "$fixboot" ]; then
            boot_missing "只读 ISO + Pending" "缺少维护工具 $fixboot；先跑 make maintain-tools"
        elif [ -z "$default_build_id" ]; then
            boot_missing "只读 ISO + Pending" "读不到缺省集 BuildId"
        else
            case_dir=$accept_dir/readonly-iso-pending
            mkdir -p "$case_dir"
            cp "$image_iso" "$case_dir/image.iso"
            "$fixboot" --image "$case_dir/image.iso" --publish --build-id "$default_build_id" --digest "$bogus_digest" > "$case_dir/prepare.log" 2>&1
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "if=ide,media=cdrom,format=raw,readonly=on,file=$case_dir/image.iso" -boot order=d \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            run_accept_warn_case "只读 ISO + Pending" readonly-iso-pending E-CORE-70 "$@"
            rm -f "$case_dir/image.iso"
        fi

        say ""
        say "工业验收：介质与控制器"
        say "介质档               进内核   诊断编号       刷新路径       结果"
        if command -v "$qemu" >/dev/null 2>&1; then
            set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$image_hdd,format=raw,if=none,id=nv0,snapshot=on" -device nvme,drive=nv0,serial=LPOSNVME -boot order=c \
                -serial "file:$accept_dir/nvme-bios/serial.log" -debugcon "file:$accept_dir/nvme-bios/debug.log"
            run_media_case "NVMe + bios" nvme-bios "$@"
            if [ -r "$ovmf_code" ] && [ -r "$ovmf_vars" ]; then
                mkdir -p "$accept_dir/nvme-uefi"
                cp "$ovmf_vars" "$accept_dir/nvme-uefi/vars.fd"
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                    -drive "if=pflash,format=raw,file=$accept_dir/nvme-uefi/vars.fd" \
                    -drive "file=$image_hdd,format=raw,if=none,id=nv0,snapshot=on" -device nvme,drive=nv0,serial=LPOSNVME \
                    -serial "file:$accept_dir/nvme-uefi/serial.log" -debugcon "file:$accept_dir/nvme-uefi/debug.log"
                run_media_case "NVMe + uefi" nvme-uefi "$@"
            else
                boot_missing "NVMe + uefi" "缺少 OVMF"
            fi
            if [ -r "$image_4kn" ]; then
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "file=$image_4kn,format=raw,if=none,id=n4,snapshot=on" \
                    -device "nvme,drive=n4,serial=LPOS4K,logical_block_size=4096,physical_block_size=4096" -boot order=c \
                    -serial "file:$accept_dir/4kn-bios/serial.log" -debugcon "file:$accept_dir/4kn-bios/debug.log"
                run_media_case "4Kn 镜像 + NVMe 4Kn + bios" 4kn-bios "$@"
                if [ -r "$ovmf_code" ] && [ -r "$ovmf_vars" ]; then
                    mkdir -p "$accept_dir/4kn-uefi"
                    cp "$ovmf_vars" "$accept_dir/4kn-uefi/vars.fd"
                    set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                        -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                        -drive "if=pflash,format=raw,file=$accept_dir/4kn-uefi/vars.fd" \
                        -drive "file=$image_4kn,format=raw,if=none,id=n4,snapshot=on" \
                        -device "nvme,drive=n4,serial=LPOS4K,logical_block_size=4096,physical_block_size=4096" \
                        -serial "file:$accept_dir/4kn-uefi/serial.log" -debugcon "file:$accept_dir/4kn-uefi/debug.log"
                    run_media_case "4Kn 镜像 + NVMe 4Kn + uefi" 4kn-uefi "$@"
                else
                    boot_missing "4Kn 镜像 + uefi" "缺少 OVMF"
                fi
            else
                boot_missing "4Kn 镜像（两档）" "缺少 $image_4kn；先跑 make ACCEPT 的依赖或 make hdd HDD=$image_4kn DISK_SECTOR_BYTES=4096"
            fi
        else
            boot_missing "介质与控制器" "找不到 QEMU"
        fi

        # 吊销与防回滚三档：变体镜像由 make 生成（各自 json 记录偏移）；缺文件按基础设施缺失跳过
        # 三档的判据分别是：E-CORE-66 停机、E-CORE-75 WARN 后仍进内核、E-CORE-74 停机
        say ""
        say "工业验收：吊销与防回滚"
        say "验收档               进内核   诊断编号       期望           结果"
        if ! command -v "$qemu" >/dev/null 2>&1; then
            boot_missing "防回滚下限" "找不到 QEMU：$qemu"
            boot_missing "吊销记录损坏" "找不到 QEMU：$qemu"
            boot_missing "签名密钥被吊销" "找不到 QEMU：$qemu"
        else
            # 安全版本低于下限：清单验证里的防回滚比较失败，必须 E-CORE-66 停机
            if [ ! -r "$image_revocation_floor" ]; then
                boot_missing "防回滚下限" "缺少 $image_revocation_floor；先跑 make（或用 --image-revocation-floor 指定）"
            else
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "file=$image_revocation_floor,format=raw,if=ide,snapshot=on" -boot order=c \
                    -serial "file:$accept_dir/revocation-floor/serial.log" -debugcon "file:$accept_dir/revocation-floor/debug.log"
                run_accept_case "防回滚下限" revocation-floor code:E-CORE-66 "$@"
            fi
            # 吊销记录损坏：只有 NotFound 是正常缺失，结构与签名不可用要报 E-CORE-75 WARN 后继续启动
            if [ ! -r "$image_revocation_bad" ]; then
                boot_missing "吊销记录损坏" "缺少 $image_revocation_bad；先跑 make（或用 --image-revocation-bad 指定）"
            else
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "file=$image_revocation_bad,format=raw,if=ide,snapshot=on" -boot order=c \
                    -serial "file:$accept_dir/revocation-bad/serial.log" -debugcon "file:$accept_dir/revocation-bad/debug.log"
                run_accept_warn_case "吊销记录损坏" revocation-bad E-CORE-75 "$@"
            fi
            # 签名密钥被吊销：吊销判定在查信任锚表之前完成，必须 E-CORE-74 停机
            if [ ! -r "$image_revocation_key" ]; then
                boot_missing "签名密钥被吊销" "缺少 $image_revocation_key；先跑 make（或用 --image-revocation-key 指定）"
            else
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "file=$image_revocation_key,format=raw,if=ide,snapshot=on" -boot order=c \
                    -serial "file:$accept_dir/revocation-key/serial.log" -debugcon "file:$accept_dir/revocation-key/debug.log"
                run_accept_case "签名密钥被吊销" revocation-key code:E-CORE-74 "$@"
            fi
        fi

        # 引导状态机多轮：候选连试三次未确认 → 用尽 → 稳态仍可启动
        # 候选用缺省集自己的 BuildId：状态记录里的清单摘要不参与启动期校验，
        # 于是能造出一个“能通过验证的候选”，把 Trying 递减到 Failed 这条真实路径走完
        say ""
        say "工业验收：引导状态机多轮"
        say "序列                 步骤                                   期望           结果"
        sequence_result() {
            accept_total=$((accept_total + 1))
            if [ "$1" = 通过 ]; then
                accept_passed=$((accept_passed + 1))
            else
                boot_failed=$((boot_failed + 1))
            fi
            printf '%-20s %-38s %-14s %s\n' "$2" "$3" "$4" "$1"
            printf '%s\t%s\t%s\t%s\t%s\n' "$2" "$3" "$4" "$1" >> "$summary"
        }

        if [ "$state_total" -gt 0 ] && [ -n "$default_build_id" ]; then
            seq_dir=$accept_dir/sequence-countdown
            mkdir -p "$seq_dir"
            cp "$image_hdd" "$seq_dir/image.hdd"
            "$fixboot" --image "$seq_dir/image.hdd" --publish --build-id "$default_build_id" --digest "$bogus_digest" > "$seq_dir/prepare.log" 2>&1
            countdown_status=通过
            # 发布时的剩余次数就是这一版计划试几次：三次试启动各留一条记录，已试次数应对上它
            tries_planned=$("$fixboot" --image "$seq_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*剩余尝试=\([0-9]*\).*/\1/p' | head -1)
            countdown_note=""
            attempt=0
            while [ "$attempt" -lt 4 ]; do
                attempt=$((attempt + 1))
                if ! "$helper" --timeout "$timeout_s" --marker "$kernel_marker" --cwd "$seq_dir" \
                        --console "$seq_dir/console-$attempt.log" \
                        --log "$seq_dir/serial-$attempt.log" --log "$seq_dir/debug-$attempt.log" -- \
                        "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                        -drive "file=$seq_dir/image.hdd,format=raw,if=ide" -boot order=c \
                        -serial "file:$seq_dir/serial-$attempt.log" -debugcon "file:$seq_dir/debug-$attempt.log" \
                        > "$seq_dir/runner-$attempt.log" 2>&1; then
                    countdown_status=失败
                fi
            done
            # 前三次是试启动（Attempt 非 0），第四次起候选用尽并回退；四次都必须进内核
            # 第四次必须报 E-CORE-73（用尽后回退），不能只靠状态列间接判断
            grep -a -q 'E-CORE-73' "$seq_dir/serial-4.log" "$seq_dir/debug-4.log" 2>/dev/null || countdown_status=失败
            first_attempt=$(tr -d '\r' < "$seq_dir/serial-1.log" | grep -ao 'Attempt=0x[0-9A-F]*' | head -1)
            state_now=$("$fixboot" --image "$seq_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*状态=\([A-Za-z]*\).*/\1/p' | head -1)
            tries_done=$("$fixboot" --image "$seq_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*已试次数=\([0-9]*\).*/\1/p' | head -1)
            countdown_note="试启动 $tries_done 次后回退"
            [ "$state_now" = Failed ] || countdown_status=失败
            # 已试次数是审计用的账：三次试启动必须留下三条记录，换候选才清零
            [ "$tries_done" = "$tries_planned" ] || countdown_status=失败
            case "$first_attempt" in Attempt=0x0000000000000000|"") countdown_status=失败 ;; esac
            rm -f "$seq_dir/image.hdd"
            sequence_result "$countdown_status" "候选三次未确认" "$countdown_note" "全程进内核"

            seq2_dir=$accept_dir/sequence-confirm
            mkdir -p "$seq2_dir"
            cp "$image_hdd" "$seq2_dir/image.hdd"
            "$fixboot" --image "$seq2_dir/image.hdd" --publish --build-id "$default_build_id" --digest "$bogus_digest" > "$seq2_dir/prepare.log" 2>&1
            confirm_status=通过
            "$helper" --timeout "$timeout_s" --marker "$kernel_marker" --cwd "$seq2_dir" \
                --console "$seq2_dir/console-1.log" \
                --log "$seq2_dir/serial-1.log" --log "$seq2_dir/debug-1.log" -- \
                "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "file=$seq2_dir/image.hdd,format=raw,if=ide" -boot order=c \
                -serial "file:$seq2_dir/serial-1.log" -debugcon "file:$seq2_dir/debug-1.log" \
                > "$seq2_dir/runner-1.log" 2>&1 || confirm_status=失败
            attempt_id=$("$fixboot" --image "$seq2_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*尝试身份=\([0-9]*\).*/\1/p' | head -1)
            if [ -n "$attempt_id" ] && [ "$attempt_id" != 0 ] &&
               "$fixboot" --image "$seq2_dir/image.hdd" --confirm "$attempt_id" > "$seq2_dir/confirm.log" 2>&1; then
                "$helper" --timeout "$timeout_s" --marker "$kernel_marker" --cwd "$seq2_dir" \
                    --console "$seq2_dir/console-2.log" \
                    --log "$seq2_dir/serial-2.log" --log "$seq2_dir/debug-2.log" -- \
                    "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "file=$seq2_dir/image.hdd,format=raw,if=ide" -boot order=c \
                    -serial "file:$seq2_dir/serial-2.log" -debugcon "file:$seq2_dir/debug-2.log" \
                    > "$seq2_dir/runner-2.log" 2>&1 || confirm_status=失败
                state_now=$("$fixboot" --image "$seq2_dir/image.hdd" --report 2>/dev/null | sed -n 's/.*状态=\([A-Za-z]*\).*/\1/p' | head -1)
                [ "$state_now" = Confirmed ] || confirm_status=失败
            else
                confirm_status=失败
            fi
            rm -f "$seq2_dir/image.hdd"
            sequence_result "$confirm_status" "试启动后确认提交" "以尝试身份提交 Confirmed" "启动进内核"
        else
            boot_missing "引导状态机多轮" "缺少 FixBootState 或读不到缺省集 BuildId"
        fi

        say ""
        say "验大小计：通过 $accept_passed/$accept_total（明细在 Out/check/accept/）"
        say ""
    fi
fi

say "汇总写在 ${summary#"$root/"}，逐例明细在同一目录"

# —— 证据目录：把可复核的记录搬出构建产物目录 ——
# 镜像副本在各段收尾时已经删掉，这里只搬文本：summary、host/boot/state/accept 的逐例日志与状态报告。
# 同名文件覆盖；evidence 目录本身由用户负责清理，脚本不删里面的既有内容
if [ -n "$evidence_dir" ]; then
    say "证据目录 $evidence_dir（summary、逐例日志与 metadata.txt 已复制）"
    mkdir -p "$evidence_dir"
    cp "$summary" "$evidence_dir/summary.txt"
    for evidence_sub in host boot state accept; do
        if [ -d "$out_dir/$evidence_sub" ]; then
            cp -R "$out_dir/$evidence_sub" "$evidence_dir/"
        fi
    done
    write_metadata
fi

# 结论：有失败 exit 1；判定不了（基础设施缺失）exit 2；否则 0
if [ "$host_passed" -ne "$host_total" ] || [ "$boot_failed" -ne 0 ]; then
    printf 'check-all: 有失败\n' >&2
    exit 1
fi
if [ "$boot_skipped" -ne 0 ]; then
    printf 'check-all: 有 %s 档因基础设施缺失未能判定\n' "$boot_skipped" >&2
    exit 2
fi
printf 'check-all: 全部通过\n'
