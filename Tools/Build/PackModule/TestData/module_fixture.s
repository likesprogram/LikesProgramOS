/* module_fixture.s
    PackModule 的最小 ELF64 ET_DYN 夹具：四个页面各放一个节，含三个入口与两张契约表符号

    用 BuildFixture.sh 组装成 ET_DYN；入口符号 _Module_Init 故意不放在偏移 0
    导出槽表 _Contract_Export_DemoIface_v1 是函数指针数组，链接后产生 R_X86_64_RELATIVE
    导入回填区 _Contract_Import_CorePage_v1 落在 .bss，由装载器写入函数地址
*/

    .section .text,"ax",@progbits
    .globl _Module_Padding
    .hidden _Module_Padding
    .type _Module_Padding, @function
_Module_Padding:
    nop
    ret
    .size _Module_Padding, . - _Module_Padding

    .globl _Module_Init
    .hidden _Module_Init
    .type _Module_Init, @function
_Module_Init:
    nop
    ret
    .size _Module_Init, . - _Module_Init

    .globl _Module_Activate
    .hidden _Module_Activate
    .type _Module_Activate, @function
_Module_Activate:
    ret
    .size _Module_Activate, . - _Module_Activate

    .globl _Module_Abort
    .hidden _Module_Abort
    .type _Module_Abort, @function
_Module_Abort:
    ret
    .size _Module_Abort, . - _Module_Abort

    .section .rodata,"a",@progbits
    .globl _FixtureRo
    .hidden _FixtureRo
    .type _FixtureRo, @object
_FixtureRo:
    .quad 0x1122334455667788
    .size _FixtureRo, . - _FixtureRo

    .section .data,"aw",@progbits
    .balign 8
    .globl _Contract_Export_DemoIface_v1
    .hidden _Contract_Export_DemoIface_v1
    .type _Contract_Export_DemoIface_v1, @object
_Contract_Export_DemoIface_v1:
    .quad _Module_Init
    .quad _Module_Activate
    .quad _Module_Abort
    .size _Contract_Export_DemoIface_v1, . - _Contract_Export_DemoIface_v1

    .section .bss,"aw",@nobits
    .balign 8
    .globl _Contract_Import_CorePage_v1
    .hidden _Contract_Import_CorePage_v1
    .type _Contract_Import_CorePage_v1, @object
_Contract_Import_CorePage_v1:
    .zero 32
    .size _Contract_Import_CorePage_v1, . - _Contract_Import_CorePage_v1
