/* Stub.ld
    Stub 镜像的链接布局：装载地址 0x7E00，入口节在最前，未初始化区不落盘

    镜像整体必须留在 [0x7E00,0x10000)：实模式弹跳按物理地址寻址，超出 64KiB
    就无法用 16 位偏移进入；末尾断言把这条约束交给链接器检查
*/

ENTRY(_Start)

SECTIONS {
    . = 0x7E00;

    .text.start : { *(.text.start) }
    .text : { *(.text.exceptions) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    __bss_end = .;

    ASSERT(__bss_end <= 0xFFFF, "Stub 镜像连同未初始化区超出低 64KiB")
}
