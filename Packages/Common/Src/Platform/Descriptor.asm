; Descriptor.asm
;    Platform::Gdt 与 Platform::Idt 的纯汇编原语：表寄存器装载、段寄存器重载、任务寄存器装载
; 位宽随输出格式：-f elf32 按 32 位保护模式，-f elf64 按 IA-32e 长模式
; 出入口约定也随模式变化：32 位是 cdecl 栈传参，64 位是 System V 寄存器传参
; 段重载用远返回换 CS：长模式远返回的默认操作数宽度是 32 位，必须显式取 64 位形式
%IFIDN __OUTPUT_FORMAT__,elf64
BITS 64
%DEFINE DESCRIPTOR_LONG_MODE 1
%ELSE
BITS 32
%ENDIF

SECTION .text

GLOBAL _Gdt_LoadPointer
GLOBAL _Idt_LoadPointer
GLOBAL _Gdt_ReloadSegments
GLOBAL _Gdt_ReloadCodeSegment
GLOBAL _Gdt_LoadTaskRegister

; 装入 GDTR：LoadPointer(tablePointer)
; 长模式下操作数固定为 16 位限长加 64 位基址，32 位保护模式是 16 位限长加 32 位基址
_Gdt_LoadPointer:
%IFDEF DESCRIPTOR_LONG_MODE
    LGDT [RDI]
%ELSE
    MOV EAX, [ESP + 4]
    LGDT [EAX]
%ENDIF
    RET

; 装入 IDTR：LoadPointer(tablePointer)
_Idt_LoadPointer:
%IFDEF DESCRIPTOR_LONG_MODE
    LIDT [RDI]
%ELSE
    MOV EAX, [ESP + 4]
    LIDT [EAX]
%ENDIF
    RET

; 重载数据段寄存器：ReloadSegments(dataSelector)
; 32 位保护模式下五个数据段寄存器都要求有效选择子；长模式只重载 DS、ES、SS，
; FS 与 GS 的基址由内核按每 CPU 数据单独设置，重载选择子对内核没有用处
_Gdt_ReloadSegments:
%IFDEF DESCRIPTOR_LONG_MODE
    MOV AX, DI
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
%ELSE
    MOV AX, [ESP + 4]
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV FS, AX
    MOV GS, AX
%ENDIF
    RET

; 用远返回把 CS 换到新代码段：ReloadCodeSegment(codeSelector)
; 目标代码段必须与当前模式同宽；模式切换（实模式到长模式、回实模式）不在这里
; 远返回之后从这里继续，栈上两个压入的值已由远返回弹出
_Gdt_ReloadCodeSegment:
%IFDEF DESCRIPTOR_LONG_MODE
    MOVZX EAX, DI           ; 选择子零扩展到 64 位
    PUSH RAX                ; 远返回栈帧：先选择子，后返回偏移
    LEA RAX, [REL .Resume]
    PUSH RAX
    RETFQ
%ELSE
    MOV EAX, [ESP + 4]
    PUSH EAX
    PUSH .Resume
    RETF
%ENDIF
.Resume:
    RET

; 装入任务寄存器：LoadTaskRegister(selector)
; 选择子须指向 GDT 内可用的 TSS 描述符；长模式下处理器按 16 字节描述符取得 64 位基址
_Gdt_LoadTaskRegister:
%IFDEF DESCRIPTOR_LONG_MODE
    LTR DI
%ELSE
    LTR WORD [ESP + 4]
%ENDIF
    RET
