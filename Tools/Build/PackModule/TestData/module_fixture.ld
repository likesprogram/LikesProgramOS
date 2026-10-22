/* module_fixture.ld
    最小 ELF64 模块夹具的链接脚本：每个容器段各占一个 4 KiB 页

    地址从 0 起（模块基址为 0），页边界固定让 PackModule 得到 TEXT/RODATA/DATA/BSS 四段；
    /DISCARD/ 丢掉注释节，避免链接器把构建信息带进夹具
*/

SECTIONS
{
    . = 0x0;
    .text : { *(.text) }
    . = ALIGN(4096);
    .rodata : { *(.rodata) }
    . = ALIGN(4096);
    .data : { *(.data) }
    . = ALIGN(4096);
    .bss : { *(.bss) }
    /DISCARD/ : { *(.comment) *(.note*) }
}
