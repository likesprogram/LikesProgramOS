; EnterKernel.asm
;    UEFI 侧的交接入口：在已经运行的长模式里把 BootInfo 交给内核入口，不返回
;
;    内核入口契约：长模式、平坦段、分页已开启、IF=0、DF=0，RDI 指向 BootInfo，
;    跳到装载地址加入口偏移
;    UEFI 固件本来就把机器放在长模式与平坦段上，分页也开着，因此这里只补方向标志、
;    换到本阶段自己的栈、把 BootInfo 放进 RDI，然后跳过去
;    栈与代码都在固件建立的恒等映射里；内核入口第一步就换成自己的栈

BITS 64

SECTION .bss
ALIGN 16
GLOBAL _Uefi_Stack_Top
; 交接期间用的栈：内核入口立刻换成自己的栈，这里只保证交接前后有一个可用的栈
Uefi_Stack_Bottom: RESB 16384
_Uefi_Stack_Top:

SECTION .text
GLOBAL _Uefi_Enter_Kernel

; 交接入口：entry 在 RDI、bootInfo 在 RSI（System V 调用约定），不返回
; 栈顶按 RIP 相对方式取址：映像被固件重定位后不需要重定位条目也能算对
_Uefi_Enter_Kernel:
    CLI
    CLD
    MOV RAX, RDI                        ; 内核入口地址
    MOV RDI, RSI                        ; BootInfo 物理地址：内核入口契约要求的寄存器
    LEA RSP, [REL _Uefi_Stack_Top]
    JMP RAX
