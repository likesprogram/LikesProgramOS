/* Stub.ld
    Stub 镜像的链接布局：装载地址 0x7E00，入口前缀与完整性头在最前，未初始化区不落盘

    镜像整体必须留在 [0x7E00,0x10000)：实模式弹跳按物理地址寻址，超出 64KiB
    就无法用 16 位偏移进入；末尾断言把这条约束与文件长度上限交给链接器检查
    __image_start / __image_end / __bss_end 同时供头字段与运行期自检使用：
    头里的 ImageBytes、MemoryBytes 由它们算出，自检再拿头字段与它们比对

    段权限按代码与数据分开声明，代码段只读可执行、数据段可读写，免得链接器
    对单一读写执行段告警；产物是 objcopy 出的扁平镜像，段边界不落盘
*/

ENTRY(_Start)

/* 两个 LOAD 段：text 只读可执行，data 可读写，未初始化区不占文件 */
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0x7E00;
    __image_start = .;

    .stub.head : { *(.stub.head) } :text
    ASSERT(ADDR(.text.start) == __image_start + 0x90, "Stub 入口节未落在头之后的 0x90")

    .text.start : { *(.text.start) } :text
    .text : { *(.text.exceptions) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    .data : { *(.data*) } :data

    __image_end = .;

    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) } :data
    __bss_end = .;
    __bss_bytes = __bss_end - __bss_start;   /* 入口在实模式下按它清零未初始化区 */

    ASSERT(__image_end - __image_start <= 0x8000, "Stub 文件超出两种介质共用的 0x8000 上限")
    ASSERT(__bss_end <= 0x10000, "Stub 镜像连同未初始化区超出低 64KiB")
}
