/* Core.ld
    内核侧 Core 的链接布局：装入地址 0x1000000，内核镜像数据在文件起点，未初始化区不落盘

    引导器把映像整幅装到 0x1000000（与数据里的 LoadAddress 一致），按数据里的 EntryOffset
    跳进入口；文件起点的 128 字节数据与 0x80 起的入口都由这一步固定
    未初始化区不落盘，由引导器在交权前按数据里的 MemoryBytes 清零，内核侧不重复清零
    __image_start / __image_end / __bss_end 同时供数据字段与运行期核对使用：
    数据里的 ImageBytes、MemoryBytes 由它们算出，内核再拿数据字段与它们比对
    段权限按代码与数据分开声明，产物是 objcopy 出的扁平镜像，段边界不落盘
*/

ENTRY(_Kernel_Start)

/* 两个 LOAD 段：text 只读可执行，data 可读写，未初始化区不占文件 */
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0x1000000;
    __image_start = .;

    .kernel.data : { *(.kernel.data) } :text
    ASSERT(ADDR(.text.start) == __image_start + 0x80, "内核入口节未落在镜像数据之后的 0x80")

    .text.start : { *(.text.start) } :text
    .text : { *(.text.exceptions) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    .data : { *(.data*) } :data

    __image_end = .;

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) } :data
    __bss_end = .;

    ASSERT(__image_end - __image_start <= 0x800000, "内核映像文件超出 0x800000 上限")
    ASSERT(__bss_end - __image_start <= 0x1000000, "内核静态内存跨度超出 0x1000000 上限")
}
