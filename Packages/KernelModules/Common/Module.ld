# Module.ld
#    .osm 模块 ELF 的链接布局：四个段按 4 KiB 页隔离、入口固定，产物交给 PackModule
#
#    容器要求段表里的 MemoryOffset/MemoryBytes 是 4 KiB 整数倍，且一页只属于一个段
#    （W^X：TEXT 只读可执行、RODATA 只读、DATA/BSS 可读写），所以每个输出段都从页边界起
#
#    首段不从 0 起：容器要求 EntryInit 非零，而 _Module_Init 可能就是 .text 的第一个
#    函数，链接基址为 0 时它的偏移会是 0；让 .text 从 0x1000 起即可固定非零
#
#    链接基址为 0（ET_DYN，装载器按 Base + 偏移写入），因此 ELF 里的绝对地址就是相对
#    模块基址的偏移：R_X86_64_RELATIVE 的 r_offset 即容器 Target、r_addend 即容器
#    Addend，PackModule 直接搬运，不需要再减基址
#
#    .rela.dyn 由 ModuleRules.mk 在链接后用 objcopy --set-section-flags 清成非 alloc：
#    重定位表只给 PackModule 读文件用，不进入装载映像，也不参与 ImageBytes 计算
#
#    --orphan-handling=error 让静态构造、TLS、栈展开节在链接期直接失败，不做静默丢弃：
#    容器不允许它们，静默丢弃会产出“能装载但不执行构造”的假模块
#    .plt/.plt.got 是 ld 为 -shared 生成的空段，模块不需要 PLT（--no-undefined 禁止
#    未定义符号），显式丢弃；ld 自己生成的 .eh_frame 由 --no-ld-generated-unwind-info 关掉

ENTRY(_Module_Init)

# 三个 LOAD 段：代码只读可执行、只读数据只读、数据与未初始化区可读写；段间按页对齐
PHDRS {
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0x1000;
    .text : { *(.text .text.*) } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
        # 需要重定位的指针表（如自描述表）由编译器放进 .data.rel*，归入可写段
        *(.data.rel*)
        # GOT 槽位若出现同样归入可写段：装载器只按重定位表改写可写段
        *(.got .got.*)
        *(.got.plt)
    } :data

    . = ALIGN(0x1000);
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    # 重定位表：只读文件内容，不分配内存；链接后由 objcopy 去掉 alloc 标志
    # .rela.* 通配吞掉输入对象的 .rela.<段>：链接器已应用它们，不产出文件字节，
    # 但 --orphan-handling=error 会把未匹配的输入重定位节当 orphan 报错，必须显式接住；
    # 输出里只剩链接器生成的动态重定位（本配方下仅 R_X86_64_RELATIVE）
    .rela.dyn : { *(.rela.dyn) *(.rela.*) }

    /DISCARD/ : {
        # 构建元数据：确定性要求不落时间戳、路径与调试信息（T1.5 去 .comment）
        *(.comment)
        *(.note*)
        # 容器不承载动态链接材料：.dynsym/.dynamic 等一律不产出
        *(.interp)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.dynbss)
        *(.hash)
        *(.gnu.hash)
        *(.gnu.version*)
        *(.rela.plt)
        # ld 为 -shared 生成的空段；模块没有未定义符号，不需要 PLT
        *(.plt)
        *(.plt.got)
        *(.plt.sec)
        # 异常表：-fno-exceptions 下不应出现；.eh_frame 由 orphan 检查兜底
        *(.gcc_except_table*)
    }
}
