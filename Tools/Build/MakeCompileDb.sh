#!/bin/sh
# MakeCompileDb.sh
#    扫描各包的 Makefile，取 C++ 编译命令写出 compile_commands.json
#
#    用法：MakeCompileDb.sh [输出文件]；默认在项目根写 compile_commands.json
#    命令取自 make -B -n 的干跑输出，编译选项与真实构建同源；
#    nasm 汇编与链接命令不进库，递归子 make 的命令也不进库；
#    同一份共享源被多个包编译时只保留首次出现的条目
set -eu

# 脚本所在目录的上两级即项目根，输出路径相对它
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
out=${1:-compile_commands.json}

# 编译库、去重清单与干跑日志的临时文件；编译库写完再替换到输出
db=$(mktemp "$root/.compile-db.XXXXXX")
seen=$(mktemp "$root/.compile-db-seen.XXXXXX")
dry=$(mktemp "$root/.compile-db-dry.XXXXXX")
cleanup() {
    rm -f -- "$db" "$seen" "$dry"
}
trap cleanup EXIT

# 转义 JSON 字符串里的反斜杠与引号
json_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

# 收录一条编译命令：$1 是包目录，$2 是干跑打印的整条命令
entry() {
    e_pkg=$1
    e_cmd=$2
    if [ -z "$e_cmd" ]; then return 0; fi
    case $e_cmd in *" -c "*) ;; *) return 0 ;; esac
    set -- $e_cmd
    case ${1##*/} in
        *g++|*clang++|*c++|*gcc|*clang|*cc) ;;
        *) return 0 ;;
    esac
    e_src=
    e_obj=
    e_prev=
    for e_tok in "$@"; do
        if [ "$e_prev" = -o ]; then e_obj=$e_tok; fi
        case $e_tok in
            *.c|*.cc|*.cpp|*.cxx) if [ "$e_prev" != -o ]; then e_src=$e_tok; fi ;;
        esac
        e_prev=$e_tok
    done
    [ -n "$e_src" ] || return 0
    e_abs_src=$(realpath -m -- "$e_pkg/$e_src")
    if grep -Fxq -- "$e_abs_src" "$seen"; then return 0; fi
    printf '%s\n' "$e_abs_src" >> "$seen"
    if [ "$first" -eq 0 ]; then printf ',\n' >> "$db"; fi
    first=0
    {
        printf '  {\n'
        printf '    "directory": "%s",\n' "$(json_escape "$root/$e_pkg")"
        printf '    "file": "%s",\n' "$(json_escape "$e_abs_src")"
        if [ -n "$e_obj" ]; then printf '    "output": "%s",\n' "$(json_escape "$(realpath -m -- "$e_pkg/$e_obj")")"; fi
        printf '    "arguments": [\n'
        e_total=$#
        e_i=0
        for e_tok in "$@"; do
            e_i=$((e_i + 1))
            if [ "$e_tok" = "$e_src" ]; then e_tok=$e_abs_src; fi
            if [ -n "$e_obj" ] && [ "$e_tok" = "$e_obj" ]; then e_tok=$(realpath -m -- "$e_pkg/$e_obj"); fi
            if [ "$e_i" -lt "$e_total" ]; then
                printf '      "%s",\n' "$(json_escape "$e_tok")"
            else
                printf '      "%s"\n' "$(json_escape "$e_tok")"
            fi
        done
        printf '    ]\n'
        printf '  }'
    } >> "$db"
    entries=$((entries + 1))
}

printf '[\n' > "$db"
first=1
entries=0
for mk in Packages/*/Makefile Packages/*/*/Makefile Tools/Build/*/Makefile; do
    if [ ! -f "$mk" ]; then continue; fi
    pkg_dir=${mk%/Makefile}
    if ! LC_ALL=C make -C "$pkg_dir" -w -B -n all > "$dry" 2>&1; then
        printf '%s\n' "MakeCompileDb: $pkg_dir 干跑失败，跳过该包" >&2
        continue
    fi
    # 只收顶层 make 自己（深度 1）的命令，递归子 make 的输出留给子包自己那一轮
    depth=0
    while IFS= read -r line; do
        case $line in
            *"Entering directory"*) depth=$((depth + 1)); continue ;;
            *"Leaving directory"*) depth=$((depth - 1)); continue ;;
        esac
        if [ "$depth" -eq 1 ]; then entry "$pkg_dir" "$line"; fi
    done < "$dry"
done
printf '\n]\n' >> "$db"
mv -- "$db" "$out"
printf '%s\n' "$out：$entries 条编译命令"
