# BuildFixture.sh
#   把 module_fixture.s 组装成 PackModule 夹具用的 ELF64 ET_DYN 模块
#
#   用法：TestData/BuildFixture.sh [输出目录]
#   默认输出到 TestData/Out；外部工具用环境变量 CC 与 LD 覆盖，默认 cc 与 ld
#   产物是未 strip 的 ET_DYN：入口符号与两张契约表符号都在符号表里

set -eu

# 本脚本所在目录：夹具源码与链接脚本的落点
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# 输出目录：调用方给的第一个参数，默认 TestData/Out
out=${1:-"$here/Out"}
# C 编译器：只做汇编与连接，不用运行库
cc=${CC:-cc}
# 链接器：直接产出 ET_DYN
ld=${LD:-ld}

mkdir -p "$out"
"$cc" -c "$here/module_fixture.s" -o "$out/module_fixture.o"
"$ld" -shared -T "$here/module_fixture.ld" -o "$out/module_fixture.elf" "$out/module_fixture.o"
printf 'BuildFixture: %s\n' "$out/module_fixture.elf"
