; Stub.asm
;    Stub 实模式入口与运行期异常入口桩：IPL 跳到 0000:7E00 后进入，保存 DL/DH，切到 32 位后调用 _Stub_Main；
;   异常桩把向量号压栈后交给 C++ 侧的停机诊断，整个文件须留在低 64KiB
BITS 16
%INCLUDE "Const.inc"

CPU 386

; 入口节必须排在最前，自定义节名要让链接器知道它是可执行代码
SECTION .text.start progbits alloc exec nowrite

GLOBAL _Start
GLOBAL _Boot_Drive
GLOBAL _Boot_Media
GLOBAL _Stub_Exception_Stubs
extern _Stub_Main
extern _Stub_Exception_Handler
; 链接脚本给出的未初始化区边界
extern __bss_start
extern __bss_end

; IPL 约定 CS:IP=0000:7E00。出口：32 位、调用 _Stub_Main
_Start:
    XOR AX, AX
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV SP, STACK_TOP
    MOV [_Boot_Drive], DL       ; BIOS 驱动器
    MOV [_Boot_Media], DH       ; Boot::Media
    CLI
    O32 LGDT [Gdt_Desc]         ; 只带进入保护模式所需的最小段表，运行期表由 C++ 侧建立
    MOV EAX, CR0
    OR AL, 1
    MOV CR0, EAX
    JMP DWORD 0x08:PM32

BITS 32
PM32:
    MOV AX, 0x10
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV FS, AX
    MOV GS, AX
    MOV ESP, LOADER_PM_STACK    ; 弹跳槽与 INT 13h 栈都在镜像的未初始化区，不占这里
    CLD                         ; 清零用 REP STOSD，须 DF=0
    ; 未初始化区不落盘，介质上没有它的内容，这里按链接脚本给出的边界整体清零
    MOV EDI, __bss_start
    MOV ECX, __bss_end
    SUB ECX, EDI
    XOR EAX, EAX
    MOV EDX, ECX
    SHR ECX, 2
    REP STOSD
    MOV ECX, EDX
    AND ECX, 3
    REP STOSB
    CALL _Stub_Main
.Hang:
    HLT
    JMP .Hang

; 进入保护模式所需的最小段表：空描述符、32 位代码段与 32 位数据段
; 运行期段表由 Platform::Gdt 在 _Stub_Main 里建立，选择子 0x08 与 0x10 的含义与这里一致
Gdt:
    DQ 0                        ; 空
    DQ 0x00CF9A000000FFFF       ; 0x08：32 位代码
    DQ 0x00CF92000000FFFF       ; 0x10：32 位数据
Gdt_End:

Gdt_Desc:
    DW Gdt_End - Gdt - 1        ; 限长
    DD Gdt                      ; 线性基址

_Boot_Drive:
    DB 0                        ; BIOS DL
_Boot_Media:
    DB 0                        ; IPL 放入 DH 的介质号

; Stub → Core 的交权入口：cdecl 参数为交权块指针与 Core 入口地址
; 入口状态在此固定：32 位保护模式、平坦段、分页关闭、IF=0、DF=0 都是调用前已有的状态，这里只补齐标志
; 跳过去不返回，Stub 从此常驻不动，交权块里的固件服务继续可用
SECTION .text.handoff progbits alloc exec nowrite
GLOBAL _Stub_Enter_Core
_Stub_Enter_Core:
    MOV ESI, [ESP + 4]          ; 交权块
    MOV EAX, [ESP + 8]          ; Core 入口
    CLI
    CLD
    JMP EAX

; 运行期异常入口桩：每个向量一个桩，把向量号压栈后跳公共入口
; C++ 侧按向量号建立门，处理器压入错误码的向量（8、10 至 14、17、21、29、30）与不压错误码的向量
; 共用同一段公共入口：C++ 侧按帧偏移读取错误码，因此这里不区分两类
SECTION .text.exceptions progbits alloc exec nowrite
%ASSIGN VECTOR 0
%REP 32
Exc_%+VECTOR:
    PUSH DWORD VECTOR
    JMP Exc_Common
%ASSIGN VECTOR VECTOR+1
%ENDREP

; 公共入口：栈顶是桩压入的向量号，其后按处理器压栈顺序是错误码（若有）、返回偏移、CS、EFLAGS
Exc_Common:
    PUSH EAX                    ; 统一数据段之前先保存 EAX
    MOV AX, 0x10                ; 不假定被中断代码的段寄存器：走平坦数据选择子
    MOV DS, AX
    MOV ES, AX
    POP EAX
    PUSH ESP                    ; 帧指针即 C++ 侧的唯一参数，指向向量号
    CALL _Stub_Exception_Handler
.Hang:
    HLT
    JMP .Hang

; 桩地址表：C++ 侧按向量号索引，省得在 C++ 里逐个声明入口
_Stub_Exception_Stubs:
%ASSIGN VECTOR 0
%REP 32
    DD Exc_%+VECTOR
%ASSIGN VECTOR VECTOR+1
%ENDREP
