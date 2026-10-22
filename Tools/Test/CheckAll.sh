#!/bin/sh
# CheckAll.sh
#    一条命令跑完宿主侧全部检查，并可选跑启动矩阵，结果收成两张表
#
#    宿主侧：30 个检查工具串行执行（+RunBootCase 共 31 个工具目录；CheckIpl、CheckStub、CheckStorage、CheckExt4、
#    CheckCrypto、CheckSelect、CheckMemory、CheckRevocation、CheckEfiBlockDevice、CheckOsm、
#    CheckPreload、CheckRevision、CheckHandoff、CheckScreen、CheckObjects、CheckContainer、
#    CheckRegistry、CheckDependency、CheckRelocation、CheckLoader、CheckDevModel、CheckVfs、
#    CheckExt4Module、CheckNvme、CheckUsbCore、CheckXhci、CheckIso9660、CheckFat32、
#    CheckUsbHub、CheckUsbStorage），逐个报告通过或失败与用例计数
#    扩展验证（--accept，隐含 --with-boot）：在启动矩阵与状态演练之外再跑失败注入、
#    介质与控制器、吊销与防回滚、只读介质与状态写入、引导状态机多轮；各段按期望停机或进内核核对
#      · 失败注入：整链逐层弄坏一次（描述符区 / Stub 摘要 / BaleanCore 头与摘要 / 布局描述魔数与校验和），
#        每层的期望是“按哪个编号停机、且绝不进内核”；另有两档篡改布局描述的内核路径并重算校验和
#        （布局本身仍合法），指向扁平落点或与清单产物不符时必须拒绝启动（E-CORE-84 / E-CORE-85）；
#        还有一档把布局的 StateBytes 改成不足一个区域（布局仍然合法），状态区参数非法必须停机（E-CORE-90）；
#        以及一档把内核路径改成含空分量的非法语法，布局校验必须在选版前拒绝（E-CORE-93）
#      · 介质与控制器：NVMe 控制器（BIOS / UEFI）与 4Kn 设备 + 按 4096 单位组装的镜像（两档）
#      · 引导状态机多轮：候选连试三次未确认 → 用尽 → 稳态仍可启动；以及确认提交后走已确认集
#      · G7 三类失败注入载体（T7.1）：Baleen FAULT_INJECT 测试变体 1–6（交接包缺失 / 清单坏 / 记录错 /
#        BuildId 不符 / 主版门 / 模块摘要错 → E-KRN-50/52/53/56/56/58）、合法签名 + 坏容器（E-KRN-57）、
#        Init 失败（E-KRN-59）；每档断言 进内核 / FATAL 编号 / 复位循环 / 状态推进 四列
#    扩展验证还包含 4Kn 介质上的状态推进（Pending→Trying、候选失败→Failed 真实落盘）、
#    只读 ISO 上的状态写入降级（E-CORE-70）、吊销与防回滚三档（E-CORE-66 / 75 / 74）
#
#    启动矩阵（--with-boot）：十二档介质各起一次真实引导，每档抽九列——
#    进内核（内核横幅 LikesProgramOS kernel）、刷新路径（Flush= 或“无刷新原语”）、
#    诊断编号（E-XXX-nn）、预装（boot closure preloaded: count=13）、闭包 Init
#    （module init ok: ×13）、挂卷（system volume mounted: uuid=…）、清单
#    （system manifest read: bytes=…）、ISO 源（ISO9660 第二来源行）、运行期模块
#    （正式镜像为 0；运行期夹具档另表验证必需失败 / 可选失败 / 可选成功），任一档没进内核即判失败
#    主线档（QEMU HDD/ISO × BIOS/UEFI + UEFI NVMe 4Kn）这几列必须齐且内核侧不得报错
#    （E-KRN 编号 / FATAL 行），引导器自身的 WARN 不算；USB 两档等 G6c 驱动就绪后再收紧
#    主线档的停机标记是阶段边界行而不是内核横幅：盯横幅会在闭包 Init 与挂卷行之前就把模拟器停掉
#    其中 BIOS 六档（QEMU 四档 + Bochs 两档）与 UEFI 五档（HDD / ISO / U 盘 / 4Kn U 盘 / NVMe 4Kn，
#    走 OVMF）；32 位固件与 qemu-system-i386 都在时再加 IA32 一档（OVMF32 加载 BOOTIA32.EFI，只验证 E-UEFI-11）
#    UEFI 档不只认内核横幅：还要求出现“清单已验证 / 已退出启动服务 / 内核已放到装载地址”三条步骤标记
#    G6d 追加两段：运行期模块夹具三档（Fault 必需失败 → E-KRN-81 + 复位循环；Optional 失败 → WARN
#    E-KRN-83 不重启；Optional 成功 → runtime module init ok）与同集两修订切换三档（rev2 直启断言
#    Rev=2、rev1 直启 Rev=1、状态区移植后发布假候选 → 回退修订 1）。
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
#      --with-boot          宿主检查之后再跑十二档启动矩阵（含 UEFI NVMe 4Kn、运行期夹具与两修订切换）与引导状态演练
#      --accept             扩展验证：在上面基础上加失败注入、NVMe / 4Kn 介质与控制器、吊销与防回滚、引导状态机多轮
#      --image-4kn <文件>   4Kn 验证镜像，默认 Out/LikesProgram.4kn.hdd
#      --image-controller <文件>      强制自建控制器路径的 AHCI 镜像，默认 Out/LikesProgram.controller.hdd
#      --image-controller-4kn <文件>  强制自建控制器路径的 NVMe 4Kn 镜像，默认 Out/LikesProgram.controller-4kn.hdd
#      --image-degrade <文件>  置了降级策略位的镜像，默认 Out/LikesProgram.degrade.hdd
#      --image-revocation-floor <文件>  安全版本低于下限的变体，默认 Out/LikesProgram.revocation-floor.hdd
#      --image-revocation-bad <文件>    吊销记录损坏的变体，默认 Out/LikesProgram.revocation-bad.hdd
#      --image-revocation-key <文件>    签名密钥被吊销的变体，默认 Out/LikesProgram.revocation-key.hdd
#      --image-runtime-fault <文件>       运行期夹具：Fault 必需模块 Init 失败，默认 Out/LikesProgram.runtime-fault.hdd
#      --image-runtime-optional <文件>    运行期夹具：Optional 可选模块 Init 失败，默认 Out/LikesProgram.runtime-optional.hdd
#      --image-runtime-optional-ok <文件> 运行期夹具：Optional 成功版（OPTIONAL_INIT_OK），默认 Out/LikesProgram.runtime-optional-ok.hdd
#      --image-revision2 <文件>           同集两修订夹具：修订 2 为当前、修订 1 为旧快照，默认 Out/LikesProgram.rev2.hdd
#      （T7.1 故障注入镜像固定从 Out/LikesProgram.fault-{1..6}-/.fault-container/.fault-init 读取，
#        由 make 的 ACCEPT 依赖构建；不在发布/交付镜像清单里）
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
# 扩展验证开关：在启动矩阵与状态演练之外再加三组
accept=0
image_iso=$root/Out/LikesProgram.iso
image_hdd=$root/Out/LikesProgram.hdd
# 4Kn 验证镜像：同一批载荷按 4096 单位组装的另一份产物
image_4kn=$root/Out/LikesProgram.4kn.hdd
# 自建控制器路径验证镜像（强制路径变体，仅 --accept）：AHCI HDD 与 NVMe 4Kn 两档
image_controller=$root/Out/LikesProgram.controller.hdd
image_controller_4kn=$root/Out/LikesProgram.controller-4kn.hdd
# 降级变体镜像：布局描述里置了"记录损坏时退到缺省集"的策略位
image_degrade=$root/Out/LikesProgram.degrade.hdd
# 吊销与防回滚变体镜像：安全版本低于下限（E-CORE-66）、吊销记录损坏（E-CORE-75 WARN）、密钥被吊销（E-CORE-74）
image_revocation_floor=$root/Out/LikesProgram.revocation-floor.hdd
image_revocation_bad=$root/Out/LikesProgram.revocation-bad.hdd
image_revocation_key=$root/Out/LikesProgram.revocation-key.hdd
# 运行期模块夹具镜像（G6d 验证用）：三个变体各带一份测试变体集（运行期 .osm 以清单 Type=2 条目
# 与 Modules.list 条目存在，Baleen 只预装闭包成员；不进正式发布集）；正式镜像的各档「运行期模块」列为 0
image_runtime_fault=$root/Out/LikesProgram.runtime-fault.hdd
image_runtime_optional=$root/Out/LikesProgram.runtime-optional.hdd
image_runtime_optional_ok=$root/Out/LikesProgram.runtime-optional-ok.hdd
# 同集两修订夹具（G6d）：修订 2 为当前列表，修订 1 为旧快照；Baleen 按 BootState.ModuleRevision 选回退目标
image_revision2=$root/Out/LikesProgram.rev2.hdd
# T7.1 故障注入测试镜像（只由 make 的 ACCEPT 依赖构建，不进发布产物）：
# Baleen 交接包注入 1..6 与“合法签名 + 坏容器”两档；路径固定为 Out/ 下的测试变体名
image_fault_baleen_prefix=$root/Out/LikesProgram.fault-
image_fault_container=$root/Out/LikesProgram.fault-container.hdd
image_fault_init=$root/Out/LikesProgram.fault-init.hdd
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
# 定长观察窗口：需要保留复位循环的档（运行期必需失败、故障注入）要跑满一段时间；
# 窗口太短会在宿主负载下少看一轮，把复位循环误判成缺失
reset_window_s=45
qemu=${QEMU:-qemu-system-x86_64}
qemu_mem=${QEMU_MEM:-512}
bochs=${BOCHS:-bochs}
helper=${RUN_BOOT_CASE:-$root/Tools/Bin/RunBootCase}

# 宿主侧的检查工具：顺序即执行顺序，也是表格的行序
host_tools='CheckIpl CheckStub CheckStorage CheckExt4 CheckCrypto CheckSelect CheckMemory CheckRevocation CheckEfiBlockDevice CheckOsm CheckPreload CheckRevision CheckHandoff CheckScreen CheckObjects CheckContainer CheckRegistry CheckDependency CheckRelocation CheckLoader CheckDevModel CheckVfs CheckExt4Module CheckNvme CheckUsbCore CheckXhci CheckIso9660 CheckFat32 CheckUsbHub CheckUsbStorage'
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
        --image-controller) [ $# -ge 2 ] || die "--image-controller 缺少参数"; image_controller=$2; shift 2 ;;
        --image-controller-4kn) [ $# -ge 2 ] || die "--image-controller-4kn 缺少参数"; image_controller_4kn=$2; shift 2 ;;
        --image-degrade) [ $# -ge 2 ] || die "--image-degrade 缺少参数"; image_degrade=$2; shift 2 ;;
        --image-revocation-floor) [ $# -ge 2 ] || die "--image-revocation-floor 缺少参数"; image_revocation_floor=$2; shift 2 ;;
        --image-revocation-bad) [ $# -ge 2 ] || die "--image-revocation-bad 缺少参数"; image_revocation_bad=$2; shift 2 ;;
        --image-revocation-key) [ $# -ge 2 ] || die "--image-revocation-key 缺少参数"; image_revocation_key=$2; shift 2 ;;
        --image-runtime-fault) [ $# -ge 2 ] || die "--image-runtime-fault 缺少参数"; image_runtime_fault=$2; shift 2 ;;
        --image-runtime-optional) [ $# -ge 2 ] || die "--image-runtime-optional 缺少参数"; image_runtime_optional=$2; shift 2 ;;
        --image-runtime-optional-ok) [ $# -ge 2 ] || die "--image-runtime-optional-ok 缺少参数"; image_runtime_optional_ok=$2; shift 2 ;;
        --image-revision2) [ $# -ge 2 ] || die "--image-revision2 缺少参数"; image_revision2=$2; shift 2 ;;
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
# 其中 ACCEPT 才跑 4Kn 状态演练：G7 汇总把主线 19 档与 4Kn 附加档分开报
state_4kn=0
# 启动矩阵的两种特殊判据：UEFI 档要额外的步骤标记；IA32 档要求出现提示且绝不进内核
boot_require_steps=0
boot_expect_no_kernel=0
# M5 主线四档判据：闭包预装/Init/挂卷/清单四列必须齐；其余档只显示
boot_require_m5=0
# ISO 档的额外判据：光盘档必须出现 ISO9660 第二来源标记（iso9660: system volume file=…）
boot_require_iso=0
# 额外文本判据：UEFI USB/NVMe 档断言块大小标识行（usbstorage: ... block=0x200 / nvme: ... block=0x1000）
boot_require_text=
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

# 从一例的串口/调试日志里抽启动矩阵的列：进内核、刷新路径、诊断编号，以及 M5 的四列
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
        grep -a -o -E 'E-[A-Z]+-[0-9][0-9][0-9]*' | sort | uniq -c | awk '{ printf "%s%s×%s", (NR > 1 ? " " : ""), $2, $1 }')
    [ -n "$boot_codes" ] || boot_codes=无
    # M5 四列：闭包预装条数、闭包 Init 条数、挂卷行、清单摘要行
    # 模块行在串口与调试口各写一份：先去重再计数，避免把一份现场算成两份
    boot_preload=-
    preload_line=$(grep -a -h -o 'boot closure preloaded: count=[0-9][0-9]*' "$dir/serial.log" "$dir/debug.log" 2>/dev/null | head -n 1)
    if [ -n "$preload_line" ]; then boot_preload=${preload_line##*count=}; fi
    boot_closure=-
    closure_lines=$({ cat "$dir/serial.log" 2>/dev/null; printf '\n'; cat "$dir/debug.log" 2>/dev/null; printf '\n'; } |
        tr -d '\r' | grep -a 'module init ok:' | sort -u | grep -a -c . || true)
    if [ "$closure_lines" -gt 0 ]; then boot_closure=$closure_lines; fi
    boot_mount=否
    if grep -a -q 'system volume mounted: uuid=' "$dir/serial.log" "$dir/debug.log" 2>/dev/null; then boot_mount=是; fi
    boot_manifest=否
    if grep -a -q 'system manifest read: bytes=' "$dir/serial.log" "$dir/debug.log" 2>/dev/null; then boot_manifest=是; fi
    # 运行期模块列：取汇总行的 loaded 数（正式发布集没有运行期条目时为 0）；没有该行显示 -
    boot_runtime=-
    runtime_line=$(grep -a -h -o 'runtime modules: loaded=[0-9][0-9]*' "$dir/serial.log" "$dir/debug.log" 2>/dev/null | head -n 1)
    if [ -n "$runtime_line" ]; then boot_runtime=${runtime_line##*loaded=}; fi
    # ISO9660 第二来源列：光盘档应出现 iso9660: system volume file=… 稳定标记（非光盘档为 -）
    boot_iso=-
    if grep -a -q 'iso9660: system volume file=' "$dir/serial.log" "$dir/debug.log" 2>/dev/null; then boot_iso=是; fi
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
    # M5 主线四档的硬判定：闭包预装 13、闭包 Init 13 条、挂卷行与清单摘要行都在，且内核侧不得报错；
    # 其余档只显示这四列，不据此判失败（USB / 4Kn / Bochs 档的驱动在 M6 才落地）
    # 判据只看内核侧：E-KRN 编号与 FATAL 行一律失败；引导器自身的 WARN（如 E-UEFI-55 耗时预算）
    # 不改变内核观测，不算失败
    if [ "$boot_require_m5" -eq 1 ]; then
        [ "$boot_preload" = 13 ] || boot_status=失败
        [ "$boot_closure" = 13 ] || boot_status=失败
        [ "$boot_mount" = 是 ] || boot_status=失败
        [ "$boot_manifest" = 是 ] || boot_status=失败
        [ "$boot_runtime" = 0 ] || boot_status=失败
        if [ "$boot_require_iso" -eq 1 ]; then [ "$boot_iso" = 是 ] || boot_status=失败; fi
        if [ -n "$boot_require_text" ] && ! grep -a -q -F -- "$boot_require_text" "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null; then boot_status=失败; fi
        case "$boot_codes" in *E-KRN-*) boot_status=失败 ;; esac
        if grep -a -q -F '[ FATAL ]' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null; then boot_status=失败; fi
    fi
    boot_total=$((boot_total + 1))
    if [ "$boot_status" = 通过 ]; then
        boot_passed=$((boot_passed + 1))
    else
        boot_failed=$((boot_failed + 1))
    fi
    row "$(printf '%-22s %-8s %-8s %-14s %-6s %-8s %-6s %-8s %-6s %-8s %s' "$name" "$boot_reached" "$boot_flush" "$boot_codes" "$boot_preload" "$boot_closure" "$boot_mount" "$boot_manifest" "$boot_iso" "$boot_runtime" "$boot_status")"
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
        # 工作树状态：dirty 时本记录不对应任何提交，验证证据必须能一眼看出这一点
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
        printf '工具链 g++：%s\n' "$(g++ --version 2>/dev/null | head -n 1)"
        printf '工具链 ld：%s\n' "$(ld --version 2>/dev/null | head -n 1)"
        printf '工具链 nasm：%s\n' "$(nasm -v 2>/dev/null | head -n 1)"
        printf '工具链 mke2fs：%s\n' "$(mke2fs -V 2>&1 | head -n 1)"
        printf '工具链 mformat：%s\n' "$(mformat --version 2>/dev/null | head -n 1)"
        # —— T7.3 集产物与模块输入哈希：离线复核链（清单→集内产物→模块 .osm→构建输入）——
        kernel_os=$root/Packages/LikesProgramOS/Core/Out/Bin/LikesProgram.os
        printf '内核原型 .os：%s sha256=%s bytes=%s\n' "$kernel_os" "$(sha256_of "$kernel_os")" "$(bytes_of "$kernel_os")"
        kernel_path_file=$root/Packages/LikesProgramOS/Out/KernelPath
        if [ -r "$kernel_path_file" ]; then
            set_rel=$(cat "$kernel_path_file")
            set_dir=$root/Packages/LikesProgramOS/Static$(dirname "$set_rel")
            printf '内核集目录：%s\n' "$set_dir"
            for set_file in LikesProgram.os Manifest.mft Modules.list; do
                printf '集产物 %s：sha256=%s bytes=%s\n' "$set_file" "$(sha256_of "$set_dir/$set_file")" "$(bytes_of "$set_dir/$set_file")"
            done
            for revision_file in "$set_dir"/Manifest.*.mft "$set_dir"/Modules.*.list; do
                [ -f "$revision_file" ] || continue
                printf '修订快照 %s：sha256=%s bytes=%s\n' "$(basename "$revision_file")" "$(sha256_of "$revision_file")" "$(bytes_of "$revision_file")"
            done
            if [ -d "$set_dir/Modules" ]; then
                for module_file in "$set_dir"/Modules/*.osm; do
                    [ -f "$module_file" ] || continue
                    printf '集内模块 %s：sha256=%s bytes=%s\n' "$(basename "$module_file")" "$(sha256_of "$module_file")" "$(bytes_of "$module_file")"
                done
            fi
        else
            printf '内核集目录：未能读取 %s\n' "$kernel_path_file"
        fi
        # 构建输入：模块产物目录里的全部 .osm（与集内副本逐字节同源）
        for module_file in "$root"/Packages/KernelModules/Out/Modules/*.osm; do
            [ -f "$module_file" ] || continue
            printf '模块产物 %s：sha256=%s bytes=%s\n' "$(basename "$module_file")" "$(sha256_of "$module_file")" "$(bytes_of "$module_file")"
        done
        # 工具链二进制：证据里的清单/签名/打包/组装产物由这些工具生成
        for tool_file in MakeManifest SignManifest MakeContract PackModule TestInstaller MakeImage PackImage; do
            printf '工具 %s：%s sha256=%s\n' "$tool_file" "$root/Tools/Bin/$tool_file" "$(sha256_of "$root/Tools/Bin/$tool_file")"
        done
        # 测试变体镜像：不进发布清单，只在验证证据里登记哈希，便于复核“测试集与发布集分离”
        for variant_file in "$image_runtime_fault" "$image_runtime_optional" "$image_runtime_optional_ok" "$image_revision2" \
            "$image_fault_baleen_prefix"1.hdd "$image_fault_baleen_prefix"2.hdd "$image_fault_baleen_prefix"3.hdd \
            "$image_fault_baleen_prefix"4.hdd "$image_fault_baleen_prefix"5.hdd "$image_fault_baleen_prefix"6.hdd \
            "$image_fault_container" "$image_fault_init"; do
            [ -e "$variant_file" ] || continue
            printf '测试变体镜像 %s：sha256=%s bytes=%s\n' "$(basename "$variant_file")" "$(sha256_of "$variant_file")" "$(bytes_of "$variant_file")"
        done
        # 发布镜像清单（IMAGES + 验证变体）与测试变体互斥的核对依据：变量名与数量写进证据
        printf '发布镜像清单：IMAGES 2 份（ISO/HDD）+ 验证变体 5 份（4Kn/降级/吊销三档）；测试变体仅在上面的“测试变体镜像”里\n'
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
    say "启动矩阵                                                          九列"
    say "介质档                进内核   刷新路径 诊断编号       预装   闭包Init 挂卷   清单   ISO源  运行期 结果"
    mkdir -p "$boot_dir"

    # QEMU 四档：HDD 直挂、ISO 直挂、HDD 当 U 盘、ISO 写入 U 盘（规格把最后一种列为必查）
    if command -v "$qemu" >/dev/null 2>&1; then
        for spec in hdd-built iso-built hdd-usb iso-usb; do
            media=${spec%%-*}
            mode=${spec#*-}
            if [ "$media" = iso ]; then image=$image_iso; else image=$image_hdd; fi
            case_dir=$boot_dir/$spec
            mkdir -p "$case_dir"
            # M5 选择：ISO 档用 i440fx（PIIX IDE 提供 0x1F0/0x170，AtaPio 的 ATAPI 路径才能读光盘）；
            # HDD 档保持 q35 走 Ahci（SATA 盘，READ DMA EXT）。非 M5 四档的档位不改机器
            machine=q35
            if [ "$media" = iso ]; then machine=pc; fi
            set -- "$qemu" -machine "$machine" -m "$qemu_mem" -monitor none -display none -no-reboot \
                -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
            if [ "$mode" = usb ]; then
                # M6 G6c：USB 档用 QEMU 的 xHCI 控制器，根端口直连 usb-storage（无 hub 下游枚举，
                # 见 M6 已知限制）；内核侧 Xhci/UsbStorage 驱动按 xHCI 规范枚举
                set -- "$@" -device qemu-xhci,id=xhci \
                    -drive "if=none,id=usbstick,format=raw,snapshot=on,file=$image" \
                    -device usb-storage,bus=xhci.0,drive=usbstick -boot order=c
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
            # 主线档（HDD / ISO 直挂 + 两档 USB）：要求闭包与挂卷列齐、运行期模块列为 0
            # ISO 直挂档另要求 ISO9660 第二来源标记（iso9660: system volume file=…）
            # 停机标记换成阶段边界行：盯内核横幅会立刻停机，闭包 Init 与挂卷行都还没打出来
            boot_require_m5=1
            if [ "$media" = iso ] && [ "$mode" = built ]; then boot_require_iso=1; fi
            boot_marker="kernel core stage ends here"
            run_boot_case "$label" "$spec" '' "$@"
            boot_require_m5=0
            boot_require_iso=0
            boot_marker=$kernel_marker
        done
    else
        for spec in hdd-built iso-built hdd-usb iso-usb; do
            boot_missing "$spec" "找不到 QEMU：$qemu"
        done
    fi

    # UEFI 五档：走 OVMF，介质与 BIOS 三档对齐，加只有 UEFI 固件才看得见的 4Kn U 盘，
    # 以及 M6 G6a 的 NVMe 4Kn 档（NVMe 控制器 + 4096 逻辑块，从 NVMe 启动到挂卷）
    # 每档的介质参数：uefi 分支在这里只负责挂介质，固件参数由上面统一加
    for spec in hdd-uefi iso-uefi usb-uefi usb4kn-uefi nvme4kn-uefi; do
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
        if [ "$uefi_media" = nvme4kn ]; then image=$image_4kn; fi
        case_dir=$boot_dir/$spec
        mkdir -p "$case_dir"
        # OVMF 的变量卷必须每例一份独立副本，否则上一例的启动项会污染下一例
        cp "$ovmf_vars" "$case_dir/vars.fd"
        # M5 选择与 BIOS 侧一致：ISO 档用 i440fx 走 PIIX IDE（AtaPio ATAPI），HDD 档保持 q35（Ahci）
        machine=q35
        if [ "$uefi_media" = iso ]; then machine=pc; fi
        set -- "$qemu" -machine "$machine" -m "$qemu_mem" -monitor none -display none -no-reboot \
            -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
            -drive "if=pflash,format=raw,file=$case_dir/vars.fd" \
            -serial "file:$case_dir/serial.log" -debugcon "file:$case_dir/debug.log"
        case "$spec" in
            hdd-uefi)
                set -- "$@" -drive "if=ide,format=raw,snapshot=on,file=$image" -boot order=c ;;
            iso-uefi)
                set -- "$@" -drive "if=ide,media=cdrom,format=raw,readonly=on,file=$image" -boot order=d ;;
            usb-uefi)
                set -- "$@" -device qemu-xhci,id=xhci \
                    -drive "if=none,id=usbstick,format=raw,snapshot=on,file=$image" \
                    -device usb-storage,bus=xhci.0,drive=usbstick -boot order=c ;;
            usb4kn-uefi)
                # 4Kn：SeaBIOS 不枚举这种设备，只有 UEFI 固件能看到，正是按 8 MiB 那份 ESP 启动
                set -- "$@" -device qemu-xhci,id=xhci \
                    -drive "if=none,id=usbstick,format=raw,snapshot=on,file=$image" \
                    -device "usb-storage,bus=xhci.0,drive=usbstick,logical_block_size=4096,physical_block_size=4096" -boot order=c ;;
            nvme4kn-uefi)
                # M6 G6a：4Kn 镜像挂成 NVMe 控制器（逻辑块 4096），UEFI 从 NVMe 上的 ESP 启动；
                # 内核侧由 Nvme 模块经 Dev.Model 取 PCI 事实、Drv.Loader 绑定，挂卷走同一套 70–76 诊断
                set -- "$@" -drive "if=none,id=nvme0,format=raw,snapshot=on,file=$image" \
                    -device "nvme,drive=nvme0,serial=LIKESNVME,logical_block_size=4096,physical_block_size=4096" ;;
        esac
        # UEFI 档额外要求三条固件阶段步骤标记，见 run_boot_case
        # G6c/G6a 后五档全部是主线：HDD / ISO / USB / 4Kn U 盘 / NVMe 4Kn，都要求闭包与挂卷列齐
        # 且运行期模块列为 0；USB 两档断言驱动报告的块大小（512e=0x200、4Kn=0x1000），
        # NVMe 4Kn 档断言 `nvme: ... block=0x1000`；停机标记换成阶段边界行
        boot_require_m5=1
        boot_require_text=
        if [ "$uefi_media" = iso ]; then boot_require_iso=1; fi
        if [ "$uefi_media" = usb ]; then boot_require_text="usbstorage: device bound block=0x200"; fi
        if [ "$uefi_media" = usb4kn ]; then boot_require_text="usbstorage: device bound block=0x1000"; fi
        if [ "$uefi_media" = nvme4kn ]; then boot_require_text="block=0x1000"; fi
        boot_marker="kernel core stage ends here"
        boot_require_steps=1
        run_boot_case "QEMU $uefi_media + uefi" "$spec" '' "$@"
        boot_require_steps=0
        boot_require_m5=0
        boot_require_iso=0
        boot_require_text=
        boot_marker=$kernel_marker
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
    matrix_total=$boot_total
    matrix_passed=$boot_passed
    say ""
    say "启动小计：通过 $boot_passed/$boot_total，失败 $boot_failed，基础设施缺失 $boot_skipped"
    say ""

    # —— 运行期模块夹具档（G6d）：三份变体镜像各带一份测试变体集 ——
    # 正式镜像的「运行期模块」列是 0（发布集不含运行期条目）；这里验证三条发行策略：
    #   必需模块失败 → E-KRN-81 FATAL + 等待复位；可选模块失败 → WARN E-KRN-83 不重启并继续启动；
    #   可选模块成功 → runtime module init ok + 汇总行 loaded=1 / failed=0 / optional=1
    # 夹具约定（M6，正式清单 Type 语义留后续）：运行期 .osm 以清单 Type=2 与 Modules.list 条目存在，
    # Baleen 只预装闭包成员；内核按「列表条目不属于引导闭包期望集」识别运行期模块，
    # Modules/ 下直接子项 = 必需，Modules/Optional/ 子目录 = 可选
    for runtime_image in "$image_runtime_fault" "$image_runtime_optional" "$image_runtime_optional_ok"; do
        [ -r "$runtime_image" ] || die "缺少运行期夹具镜像 $runtime_image；先跑 make（或用 --image-runtime-* 指定）"
    done
    runtime_dir=$boot_dir/runtime
    mkdir -p "$runtime_dir"
    say "运行期模块档                                                        期望"
    say "变体                   进内核   运行期标记  诊断编号        结果"

    # 跑一档运行期夹具：run_runtime_case <名称> <目录名> <镜像> <停机标记> <期望编号-> <期望运行期数> <必须出现的子串->
    run_runtime_case() {
        rt_name=$1
        rt_spec=$2
        rt_image=$3
        rt_marker=$4
        rt_want_code=$5
        rt_want_runtime=$6
        rt_want_text=$7
        rt_dir=$runtime_dir/$rt_spec
        mkdir -p "$rt_dir"
        : > "$rt_dir/serial.log"
        : > "$rt_dir/debug.log"
        : > "$rt_dir/console.log"
        rt_status=通过
        if ! "$helper" --timeout "$timeout_s" --marker "$rt_marker" \
                --cwd "$rt_dir" --console "$rt_dir/console.log" \
                --log "$rt_dir/serial.log" --log "$rt_dir/debug.log" -- \
                "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                -drive "if=ide,format=raw,snapshot=on,file=$rt_image" -boot order=c \
                -serial "file:$rt_dir/serial.log" -debugcon "file:$rt_dir/debug.log" > "$rt_dir/runner.log" 2>&1; then
            rt_status=失败
        fi
        boot_columns "$rt_dir"
        [ "$boot_reached" = 是 ] || rt_status=失败
        if [ "$rt_want_code" != "-" ] && ! printf '%s' "$boot_codes" | grep -q -- "$rt_want_code"; then rt_status=失败; fi
        [ "$boot_runtime" = "$rt_want_runtime" ] || rt_status=失败
        if [ "$rt_want_text" != "-" ] && ! grep -a -q -F -- "$rt_want_text" "$rt_dir/serial.log" "$rt_dir/debug.log" 2>/dev/null; then rt_status=失败; fi
        # 可选降级与可选成功都必须停在阶段边界、不发生复位循环（一次启动只有一条 BALEENSTUB 行）
        if [ "$rt_spec" != fault ]; then
            restarts=$(grep -a -c 'BALEENSTUB' "$rt_dir/serial.log" 2>/dev/null || true)
            [ "$restarts" = 1 ] || rt_status=失败
            if grep -a -q -F '[ FATAL ]' "$rt_dir/serial.log" "$rt_dir/debug.log" 2>/dev/null; then rt_status=失败; fi
        fi
        boot_total=$((boot_total + 1))
        if [ "$rt_status" = 通过 ]; then boot_passed=$((boot_passed + 1)); else boot_failed=$((boot_failed + 1)); fi
        row "$(printf '%-22s %-8s %-10s %-14s %s' "$rt_name" "$boot_reached" "$boot_runtime" "$boot_codes" "$rt_status")"
    }

    run_runtime_case "runtime-fault" fault "$image_runtime_fault" "E-KRN-81" "E-KRN-81" "-" "Modules/Fault.1.0.0.osm"
    run_runtime_case "runtime-optional" optional "$image_runtime_optional" "kernel core stage ends here" "E-KRN-83" "0" "E-KRN-83"
    run_runtime_case "runtime-optional-ok" optional-ok "$image_runtime_optional_ok" "kernel core stage ends here" "-" "1" "runtime module init ok: Optional"

    # 必需失败还要证明「等待 + 复位循环」：定长运行一段，按出现的启动次数与编号次数核对
    if [ "$with_boot" -eq 1 ]; then
        rt_dir=$runtime_dir/fault-reset
        mkdir -p "$rt_dir"
        rt_status=通过
        # 这一档要看到复位循环：不能带 -no-reboot（它会让 QEMU 在复位请求时直接退出，只看得到一轮）
        # 观察窗口用 reset_window_s：要覆盖两轮完整启动，固定 26s 在宿主负载高时会少看一轮
        ( timeout -s KILL "$reset_window_s" "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none \
              -drive "if=ide,format=raw,snapshot=on,file=$image_runtime_fault" -boot order=c \
              -serial "file:$rt_dir/serial.log" -debugcon "file:$rt_dir/debug.log" >/dev/null 2>&1 ) || true
        restarts=$(grep -a -c 'BALEENSTUB' "$rt_dir/serial.log" 2>/dev/null || true)
        fatals=$(grep -a -c 'E-KRN-81' "$rt_dir/serial.log" 2>/dev/null || true)
        [ "$restarts" -ge 2 ] || rt_status=失败
        [ "$fatals" -ge 2 ] || rt_status=失败
        boot_total=$((boot_total + 1))
        if [ "$rt_status" = 通过 ]; then boot_passed=$((boot_passed + 1)); else boot_failed=$((boot_failed + 1)); fi
        row "$(printf '%-22s %-8s %-10s %-14s %s' "runtime-fault 复位循环" "是" "-" "E-KRN-81×$fatals" "$rt_status")"
    fi
    runtime_total=$((boot_total - matrix_total))
    runtime_passed=$((boot_passed - matrix_passed))
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
    # 状态演练与验证都要用它造“能通过验证的候选”，因此在这里定义一次
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
        state_4kn=$((state_4kn + 1))
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
        # 验证模式才跑：候选 BuildId 从 4Kn 镜像自己的布局描述里取，digest 用上面那个假摘要
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

        # —— 同集两修订切换（G6d）——
        # 修订 2 = 当前列表（MODULE_REVISION=2 的 Manifest.2/Modules.2），修订 1 = 旧快照；
        # 步骤：① 修订 2 镜像正常启动，内核打印 Rev=2；
        #      ② 修订 1 镜像启动一次，把状态区（含 Confirmed 的 ModuleRevision=1）搬到修订 2 副本；
        #      ③ 修订 2 副本上发布假候选 → 候选验证失败 → 回退到记录里的回退目标修订 1 → 内核打印 Rev=1。
        # 状态区偏移/长度取布局约定（0x21000 / 4096）；两份镜像几何相同，按 512 字节扇区 dd 搬运
        if [ -r "$image_revision2" ]; then
            rev_dir=$state_dir/revisions
            mkdir -p "$rev_dir"
            # 启动一档修订夹具：rev_boot <镜像副本> <目录> <期望 Rev 文本或->
            rev_boot() {
                rb_image=$1
                rb_dir=$2
                rb_want=$3
                mkdir -p "$rb_dir"
                : > "$rb_dir/serial.log"
                : > "$rb_dir/debug.log"
                : > "$rb_dir/console.log"
                rb_status=通过
                if ! "$helper" --timeout "$timeout_s" --marker "kernel core stage ends here" \
                        --cwd "$rb_dir" --console "$rb_dir/console.log" \
                        --log "$rb_dir/serial.log" --log "$rb_dir/debug.log" -- \
                        "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                        -drive "file=$rb_image,format=raw,if=ide" -boot order=c \
                        -serial "file:$rb_dir/serial.log" -debugcon "file:$rb_dir/debug.log" > "$rb_dir/runner.log" 2>&1; then
                    rb_status=失败
                fi
                if ! grep -a -q -F -- "$kernel_banner" "$rb_dir/serial.log" "$rb_dir/debug.log" 2>/dev/null; then rb_status=失败; fi
                if [ "$rb_want" != "-" ] && ! grep -a -q -F -- "$rb_want" "$rb_dir/serial.log" "$rb_dir/debug.log" 2>/dev/null; then rb_status=失败; fi
                if grep -a -q -F '[ FATAL ]' "$rb_dir/serial.log" "$rb_dir/debug.log" 2>/dev/null; then rb_status=失败; fi
                state_total=$((state_total + 1))
                if [ "$rb_status" = 通过 ]; then state_passed=$((state_passed + 1)); else boot_failed=$((boot_failed + 1)); fi
                printf '%-20s %-8s %-8s %-14s %s\n' "$4" "是" "$(if [ "$rb_want" = "-" ]; then printf -- '-'; else printf '%s' "$rb_want" | sed 's/.*Rev=//'; fi)" "$(if [ "$rb_want" = "-" ]; then printf -- '-'; else printf 'Rev=%s' "$(printf '%s' "$rb_want" | sed 's/.*Rev=//')"; fi)" "$rb_status"
                printf '%s\t%s\t%s\t%s\t%s\n' "$4" "是" "$rb_want" "修订" "$rb_status" >> "$summary"
            }
            # ① 修订 2 正常启动：当前列表是修订 2，内核 Rev 应为 2
            cp "$image_revision2" "$rev_dir/rev2/image.hdd" 2>/dev/null || { mkdir -p "$rev_dir/rev2"; cp "$image_revision2" "$rev_dir/rev2/image.hdd"; }
            rev_boot "$rev_dir/rev2/image.hdd" "$rev_dir/rev2" 'Rev=0x0000000000000002' "两修订 修订2 直启"
            # ② 修订 1 镜像启动一次，让状态区写下 Confirmed 的 ModuleRevision=1
            mkdir -p "$rev_dir/rev1"
            cp "$image_hdd" "$rev_dir/rev1/image.hdd"
            rev_boot "$rev_dir/rev1/image.hdd" "$rev_dir/rev1" 'Rev=0x0000000000000001' "两修订 修订1 直启"
            # ③ 把修订 1 的状态区搬进修订 2 副本，再发布假候选，逼选择器回退到回退目标修订 1
            mkdir -p "$rev_dir/fallback"
            cp "$image_revision2" "$rev_dir/fallback/image.hdd"
            dd if="$rev_dir/rev1/image.hdd" of="$rev_dir/fallback/image.hdd" bs=512 \
                skip=$((0x21000 / 512)) seek=$((0x21000 / 512)) count=$((4096 / 512)) conv=notrunc 2>/dev/null
            "$fixboot" --image "$rev_dir/fallback/image.hdd" --publish --build-id "$bogus_build_id" --digest "$bogus_digest" > "$rev_dir/fallback/prepare.log" 2>&1 || true
            rev_boot "$rev_dir/fallback/image.hdd" "$rev_dir/fallback" 'Rev=0x0000000000000001' "两修订 候选失败回退修订1"
        else
            boot_missing "两修订切换（三档）" "缺少 $image_revision2；先跑 make（或用 --image-revision2 指定）"
        fi

        # 留证据、省空间：每档的最终状态报告落成文本，镜像副本删掉（要复跑就再跑一次本脚本）
        for case_dir in "$state_dir"/*; do
            [ -d "$case_dir" ] || continue
            [ -f "$case_dir/image.hdd" ] || continue
            "$fixboot" --image "$case_dir/image.hdd" --report > "$case_dir/state-report.txt" 2>&1
            rm -f "$case_dir/image.hdd"
        done
        # 验证与覆盖核验留下的副本同理：日志与逐例报告已经落盘，副本不必留（一次全量跑会攒几百兆）
        # 全部 .hdd 都是本脚本造的副本（真产物在 Out/ 下，不在这个目录里），收尾一并回收
        find "$out_dir" -name '*.hdd' -type f -delete 2>/dev/null || true

        say ""
        say "状态演练小计：通过 $state_passed/$state_total（每档最终状态见 Out/check/state/<档>/state-report.txt）"
        say ""
    fi

    # —— 扩展验证：失败注入、介质与控制器、引导状态机多轮 ——
    if [ "$accept" -eq 1 ]; then
        accept_dir=$out_dir/accept
        mkdir -p "$accept_dir"
        accept_total=0
        accept_passed=0
        # G7 故障注入载体单独计数（也计入 accept/boot 的失败出口）
        inject_total=0
        inject_passed=0

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

        # 扩展验证的一档：expect 为 code:E-XXX-nn（日志里必须出现该编号且不进内核）
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
            boot_volume_ok=否
            if grep -a -q -F 'The system volume matches the layout descriptor' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null; then boot_volume_ok=是; fi
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

        # 扩展验证的一档：UEFI 自建控制器路径（强制路径变体）
        # 判据：进内核、自建路径绑定行、控制器类型、闭包预装 13、挂卷与清单、无 E-KRN / FATAL
        run_controller_case() {
            name=$1
            case_dir=$accept_dir/$2
            want_controller=$3
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
            boot_volume_ok=否
            if grep -a -q -F 'The system volume matches the layout descriptor' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null; then boot_volume_ok=是; fi
            [ "$boot_reached" = 是 ] || accept_status=失败
            [ "$boot_preload" = 13 ] || accept_status=失败
            # 控制器用例在进入内核后立即停机，内核侧的挂卷/清单行不会出现：
            # 改判 UEFI 阶段已经完成的“卷标识核对 / 清单验证 / 步骤标记”三条
            grep -a -q -F 'The system volume matches the layout descriptor' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null || accept_status=失败
            grep -a -q -F 'The kernel set is verified' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null || accept_status=失败
            grep -a -q -F 'The firmware boot services are exited' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null || accept_status=失败
            grep -a -q -F 'The self-built controller path is bound' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null || accept_status=失败
            grep -a -q -F "Controller=$want_controller" "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null || accept_status=失败
            case "$boot_codes" in *E-KRN-*) accept_status=失败 ;; esac
            if grep -a -q -F '[ FATAL ]' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null; then accept_status=失败; fi
            accept_total=$((accept_total + 1))
            if [ "$accept_status" = 通过 ]; then
                accept_passed=$((accept_passed + 1))
            else
                boot_failed=$((boot_failed + 1))
            fi
            printf '%-20s %-8s %-8s %-6s %-8s %s\n' "$name" "$want_controller" "$boot_reached" "$boot_preload" "$boot_volume_ok" "$accept_status"
            printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$want_controller" "$boot_reached" "$boot_preload" "$boot_volume_ok" "$accept_status" >> "$summary"
        }

        # 扩展验证的一档（WARN 档）：必须进内核，且日志里必须出现指定编号
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
            boot_volume_ok=否
            if grep -a -q -F 'The system volume matches the layout descriptor' "$case_dir/serial.log" "$case_dir/debug.log" 2>/dev/null; then boot_volume_ok=是; fi
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

        say "扩展验证：失败注入"
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
        say "扩展验证：只读介质与状态写入"
        say "验证档               进内核   诊断编号       期望           结果"
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
        say "扩展验证：介质与控制器"
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

        say ""
        say "扩展验证：UEFI 自建控制器路径"
        say "验证档               控制器   进内核   预装   卷核对   结果"
        if ! command -v "$qemu" >/dev/null 2>&1; then
            boot_missing "AHCI 强制路径" "找不到 QEMU：$qemu"
            boot_missing "NVMe 4Kn 强制路径" "找不到 QEMU：$qemu"
        elif [ ! -r "$ovmf_code" ] || [ ! -r "$ovmf_vars" ]; then
            boot_missing "AHCI 强制路径" "缺少 OVMF CODE/VARS：$ovmf_code / $ovmf_vars"
            boot_missing "NVMe 4Kn 强制路径" "缺少 OVMF CODE/VARS：$ovmf_code / $ovmf_vars"
        else
            if [ ! -r "$image_controller" ]; then
                boot_missing "AHCI 强制路径" "缺少 $image_controller；先跑 make ACCEPT（或用 --image-controller 指定）"
            else
                mkdir -p "$accept_dir/controller-ahci"
                cp "$ovmf_vars" "$accept_dir/controller-ahci/vars.fd"
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                    -drive "if=pflash,format=raw,file=$accept_dir/controller-ahci/vars.fd" \
                    -drive "if=ide,format=raw,snapshot=on,file=$image_controller" -boot order=c \
                    -serial "file:$accept_dir/controller-ahci/serial.log" -debugcon "file:$accept_dir/controller-ahci/debug.log"
                run_controller_case "AHCI 强制路径" controller-ahci AHCI "$@"
            fi
            if [ ! -r "$image_controller" ]; then
                boot_missing "USB 类码不支持" "缺少 $image_controller；先跑 make ACCEPT（或用 --image-controller 指定）"
            else
                # 负例：强制路径面对 xHCI 控制器，类码不是 AHCI / NVMe，必须停在 E-UEFI-102
                mkdir -p "$accept_dir/controller-usb"
                cp "$ovmf_vars" "$accept_dir/controller-usb/vars.fd"
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                    -drive "if=pflash,format=raw,file=$accept_dir/controller-usb/vars.fd" \
                    -device qemu-xhci,id=xhci \
                    -drive "if=none,id=usbctrl,format=raw,snapshot=on,file=$image_controller" \
                    -device usb-storage,bus=xhci.0,drive=usbctrl -boot order=c \
                    -serial "file:$accept_dir/controller-usb/serial.log" -debugcon "file:$accept_dir/controller-usb/debug.log"
                run_accept_case "USB 类码不支持" controller-usb code:E-UEFI-102 "$@"
            fi
            if [ ! -r "$image_controller_4kn" ]; then
                boot_missing "NVMe 4Kn 强制路径" "缺少 $image_controller_4kn；先跑 make ACCEPT（或用 --image-controller-4kn 指定）"
            else
                mkdir -p "$accept_dir/controller-nvme"
                cp "$ovmf_vars" "$accept_dir/controller-nvme/vars.fd"
                set -- "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none -no-reboot \
                    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
                    -drive "if=pflash,format=raw,file=$accept_dir/controller-nvme/vars.fd" \
                    -drive "file=$image_controller_4kn,format=raw,if=none,id=nvctrl,snapshot=on" \
                    -device "nvme,drive=nvctrl,serial=LPOSCTRL,logical_block_size=4096,physical_block_size=4096" -boot order=c \
                    -serial "file:$accept_dir/controller-nvme/serial.log" -debugcon "file:$accept_dir/controller-nvme/debug.log"
                run_controller_case "NVMe 4Kn 强制路径" controller-nvme NVME "$@"
            fi
        fi

        # 吊销与防回滚三档：变体镜像由 make 生成（各自 json 记录偏移）；缺文件按基础设施缺失跳过
        # 三档的判据分别是：E-CORE-66 停机、E-CORE-75 WARN 后仍进内核、E-CORE-74 停机
        say ""
        say "扩展验证：吊销与防回滚"
        say "验证档               进内核   诊断编号       期望           结果"
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
        say "扩展验证：引导状态机多轮"
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
            # 确认写 Confirmed 必须带本次装载的模块修订号：取构建暂存的 Modules.list 的 revision；
            # 没有构建产物时按 1（测试介质默认修订）
            module_revision=$(sed -n 's/^revision //p' "$root/Packages/LikesProgramOS/Out/KernelSetStage/Modules.list" 2>/dev/null | head -1)
            [ -n "$module_revision" ] || module_revision=1
            if [ -n "$attempt_id" ] && [ "$attempt_id" != 0 ] &&
               "$fixboot" --image "$seq2_dir/image.hdd" --confirm "$attempt_id" --revision "$module_revision" > "$seq2_dir/confirm.log" 2>&1; then
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

        # —— G7 故障注入载体（T7.1①Baleen 交接包 / ②合法签名坏容器）——
        # 判据：进内核=是（内核横幅出现）、内核侧 FATAL 编号在日志里、26 秒定长跑出现复位循环
        # （≥2 次 BaleenStub / ≥2 条编号）、状态区第一轮已写 Confirmed（代次≥1）。
        # Baleen 自己的判定与日志不动：故障在交接前才注入，失败可见性由内核侧统一给出
        say ""
        say "扩展验证：G7 故障注入载体"
        say "注入档               进内核   FATAL编号   复位循环  状态推进  结果"
        run_inject_case() {
            inj_name=$1
            inj_image=$2
            inj_code=$3
            inj_dir=$accept_dir/inject-$inj_name
            mkdir -p "$inj_dir"
            : > "$inj_dir/serial.log"
            : > "$inj_dir/debug.log"
            : > "$inj_dir/console.log"
            # 拷贝到段目录再跑（不写构建产物）；不带 -no-reboot，才能观察复位循环；
            # 观察窗口与运行期失败档共用 reset_window_s
            cp "$inj_image" "$inj_dir/image.hdd"
            ( timeout -s KILL "$reset_window_s" "$qemu" -machine q35 -m "$qemu_mem" -monitor none -display none \
                  -drive "file=$inj_dir/image.hdd,format=raw,if=ide" -boot order=c \
                  -serial "file:$inj_dir/serial.log" -debugcon "file:$inj_dir/debug.log" >/dev/null 2>&1 ) || true
            inj_status=通过
            inj_starts=$(grep -a -c 'BALEENSTUB' "$inj_dir/serial.log" 2>/dev/null || true)
            inj_fatals=$(grep -a -c -- "$inj_code" "$inj_dir/serial.log" 2>/dev/null || true)
            inj_reached=否
            if grep -a -q -F -- "$kernel_banner" "$inj_dir/serial.log" "$inj_dir/debug.log" 2>/dev/null; then inj_reached=是; fi
            inj_reset=否
            [ "$inj_starts" -ge 2 ] && inj_reset=是
            inj_state=否
            if [ -x "$fixboot" ] && "$fixboot" --image "$inj_dir/image.hdd" --report > "$inj_dir/state-report.txt" 2>&1; then
                inj_generation=$(sed -n 's/.*代次=\([0-9]*\).*/\1/p' "$inj_dir/state-report.txt" | head -n 1)
                [ -n "$inj_generation" ] && [ "$inj_generation" -ge 1 ] && inj_state=是
            fi
            [ "$inj_reached" = 是 ] || inj_status=失败
            [ "$inj_fatals" -ge 2 ] || inj_status=失败
            [ "$inj_reset" = 是 ] || inj_status=失败
            [ "$inj_state" = 是 ] || inj_status=失败
            rm -f "$inj_dir/image.hdd"
            inject_total=$((inject_total + 1))
            if [ "$inj_status" = 通过 ]; then inject_passed=$((inject_passed + 1)); else boot_failed=$((boot_failed + 1)); fi
            accept_total=$((accept_total + 1))
            if [ "$inj_status" = 通过 ]; then accept_passed=$((accept_passed + 1)); fi
            printf '%-20s %-8s %-12s %-10s %-8s %s\n' "$inj_name" "$inj_reached" "$inj_code×$inj_fatals" "$inj_reset" "$inj_state" "$inj_status"
            printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$inj_name" "$inj_reached" "$inj_code×$inj_fatals" "$inj_reset" "$inj_state" "$inj_status" >> "$summary"
        }
        run_inject_case baleen-handoff-missing "$image_fault_baleen_prefix"1.hdd E-KRN-50
        run_inject_case baleen-manifest-bad "$image_fault_baleen_prefix"2.hdd E-KRN-52
        run_inject_case baleen-record-bad "$image_fault_baleen_prefix"3.hdd E-KRN-53
        run_inject_case baleen-buildid-bad "$image_fault_baleen_prefix"4.hdd E-KRN-56
        run_inject_case baleen-major-gate "$image_fault_baleen_prefix"5.hdd E-KRN-56
        run_inject_case baleen-digest-bad "$image_fault_baleen_prefix"6.hdd E-KRN-58
        run_inject_case container-bad "$image_fault_container" E-KRN-57
        run_inject_case init-fail "$image_fault_init" E-KRN-59

        say ""
        say "验大小计：通过 $accept_passed/$accept_total（明细在 Out/check/accept/）"
        say ""
    fi

    # —— T7.2 收口：G7 验证总计（数字与 summary 开头批次行、flush 各表一致）——
    # 总检查项 = 宿主用例数 + 启动矩阵档数 + 运行期档数 + 状态演练档数 + 失败注入档数；
    # 不含“每种载体里的每一条断言”，断言逐条落在各自的逐例日志里
    if [ "$accept" -eq 1 ]; then
        inject_note="$inject_passed/$inject_total 档（Baleen 交接包 6 + 坏容器 1 + Init 失败 1）"
    else
        inject_note="未运行（需要 --accept；三类载体见 T7.1）"
    fi
    say ""
    say "G7 验证总计（$(if [ "$accept" -eq 1 ]; then printf -- '--accept'; else printf -- '--with-boot'; fi)）"
    say "  宿主检查：$host_passed/$host_total 工具，用例合计 $host_cases"
    say "  启动矩阵：$matrix_passed/$matrix_total 档（含 NVMe 4Kn 与 4 个 xHCI USB 档）"
    say "  运行期模块：$runtime_passed/$runtime_total 档（含必需失败复位循环）"
    if [ "$state_4kn" -gt 0 ]; then
        say "  状态演练：$state_passed/$state_total 档（主线 $((state_total - state_4kn)) + 4Kn 状态 $state_4kn，含两修订切换 3 档）"
    else
        say "  状态演练：$state_passed/$state_total 档（含两修订切换 3 档）"
    fi
    say "  失败注入（G7 三类载体）：$inject_note"
    say "  总检查项：$((host_cases + matrix_total + runtime_total + state_total + inject_total))（宿主用例 + 启动/演练/注入档数）"
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
