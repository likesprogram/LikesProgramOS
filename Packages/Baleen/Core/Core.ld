/* Core.ld
    BaleenCore 镜像的链接布局：装入地址 0x100000，入口前缀与完整性头在最前，未初始化区不落盘

    Stub 把镜像整幅装到 1MiB，并按 CORE_LOAD + 头里的 EntryOffset 跳到入口；文件起点的
    前缀、0x10 起的 128 字节头与 0x90 起的代码都由这一步固定
    未初始化区不落盘，由 Stub 在交权前按头里的 MemoryBytes 清零，Core 侧不重复清零
    __image_start / __image_end / __bss_end 同时供头字段与运行期核对使用：
    头里的 ImageBytes、MemoryBytes 由它们算出，Core 再拿头字段与它们比对

    段权限按代码与数据分开声明，代码段只读可执行、数据段可读写，免得链接器
    对单一读写执行段告警；产物是 objcopy 出的扁平镜像，段边界不落盘
*/

ENTRY(_Core_Start)

/* 两个 LOAD 段：text 只读可执行，data 可读写，未初始化区不占文件 */
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0x100000;
    __image_start = .;

    .core.head : { *(.core.head) } :text
    ASSERT(ADDR(.text.start) == __image_start + 0x90, "Core 入口节未落在头之后的 0x90")

    .text.start : { *(.text.start) } :text
    .text : { *(.text.exceptions) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    .data : { *(.data*) } :data

    __image_end = .;

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) } :data
    __bss_end = .;

    ASSERT(__image_end - __image_start <= 0x400000, "Core 文件超出 0x400000 上限")
    ASSERT(__bss_end - __image_start <= 0x400000, "Core 静态内存跨度超出 0x400000 上限")
}
