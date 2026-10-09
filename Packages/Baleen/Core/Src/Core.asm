; Core.asm
;    BaleenCore 入口与交接入口：文件起点是 16 位近跳转前缀与完整性头，0x90 起是 32 位保护模式入口；
;    Stub 把镜像装到 1MiB 并按 CORE_LOAD + 头里的 EntryOffset 直接跳进 0x90，ESI 指向交权块，
;    这里换到自己的栈、把交权块指针交给 _Core_Main，不返回
;
;    文件起点的前缀不被执行：Stub 跳的是头里的入口偏移，不经过它；它按与 Stub 镜像同构的
;    格式要求写出，并由宿主打包器核对与 EntryOffset 一致，只有 16 位跳转语义
;    头里的 ImageBytes 与 MemoryBytes 由链接器按镜像、未初始化区末尾填入，
;    BuildId 与 Digest 由 Tools/Bin/PackImage 填充，布局与断言见 Core.ld
;    _Core_Enter_Kernel 是交接入口：切长模式与恒等映射分页后跳到内核入口，64 位段表在本文件内
BITS 16
%INCLUDE "Const.inc"
%INCLUDE "CoreHeader.inc"

; 目标处理器：文件里同时有 16 位前缀、32 位运行期代码与 64 位交接代码
CPU x64

; 自身完整性头与入口前缀：排在镜像最前，三处关键偏移由紧随其后的断言盯住
SECTION .core.head progbits alloc exec nowrite

    JMP NEAR $ + CORE_ENTRY_OFFSET      ; 从装入点跳到头之后的入口，位移由本行位置直接算出
%IF ($ - $$) != 3
    %ERROR "入口前缀不是 3 字节的 16 位近跳转"
%ENDIF
    TIMES CORE_HEAD_OFFSET - ($ - $$) DB 0
%IF ($ - $$) != CORE_HEAD_OFFSET
    %ERROR "BaleenCore 头未落在约定的文件偏移"
%ENDIF
    DB "BLNCORE", 0                     ; Magic：格式标记，含终止零
    DW CORE_VERSION                     ; Version：未发布的开发格式标记
    DW CORE_HEAD_BYTES                  ; HeaderBytes：头固定长度
    DD CORE_FLAGS                       ; Flags：当前不定义可选标志
    DD __image_end - CORE_LOAD          ; ImageBytes：文件字节数，按镜像末尾由链接器填入
    DD __bss_end - CORE_LOAD            ; MemoryBytes：静态内存跨度，按未初始化区末尾由链接器填入
    DD CORE_ENTRY_OFFSET                ; EntryOffset：早期初始化入口的文件偏移
    DW CORE_DIGEST_SHA256               ; DigestAlgorithm：1 为 SHA-256，无“禁用摘要”值
    DW CORE_BUILDID_BYTES               ; BuildIdBytes：BuildId 长度
%IF ($ - $$) != CORE_HEAD_OFFSET + CORE_BUILDID_OFFSET
    %ERROR "BuildId 未落在头内约定的偏移"
%ENDIF
    TIMES CORE_BUILDID_BYTES DB 0       ; BuildId：由打包器填充
%IF ($ - $$) != CORE_HEAD_OFFSET + CORE_DIGEST_OFFSET
    %ERROR "Digest 未落在头内约定的偏移"
%ENDIF
    TIMES 32 DB 0                       ; Digest：由打包器按摘要覆盖区间填充
%IF ($ - $$) != CORE_HEAD_OFFSET + CORE_HEAD_BYTES - CORE_RESERVE_BYTES
    %ERROR "保留区未落在头内约定的偏移"
%ENDIF
    TIMES CORE_RESERVE_BYTES DB 0       ; Reserved：全 0
%IF ($ - $$) != CORE_HEAD_OFFSET + CORE_HEAD_BYTES
    %ERROR "BaleenCore 头长度不是约定的 0x80"
%ENDIF

; 入口节：头之后的第一条指令，必须落在 CORE_ENTRY_OFFSET，否则 Stub 跳错位置
SECTION .text.start progbits alloc exec nowrite

BITS 32
GLOBAL _Core_Start
GLOBAL _Core_Exception_Stubs
extern _Core_Main
extern _Core_Exception_Handler
; 链接脚本给出的镜像与未初始化区边界，供头字段使用
extern __image_end
extern __bss_end

; Stub 按 32 位保护模式、平坦段、分页关闭、IF=0、DF=0 跳到 CORE_LOAD + EntryOffset，ESI 指向交权块
; 进入状态是交权契约保证的，这里只补齐方向标志并换栈：Stub 的保护模式栈随 Stub 冻结，不能继续用
_Core_Start:
    CLI
    CLD
    MOV ESP, _Core_Stack_Top        ; 自己的栈在未初始化区，由 Stub 交权前按 MemoryBytes 一并清零
    PUSH ESI                        ; 交权块指针，_Core_Main 的唯一参数
    CALL _Core_Main
.Hang:
    HLT
    JMP .Hang

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
    CALL _Core_Exception_Handler
.Hang:
    HLT
    JMP .Hang

; 桩地址表：C++ 侧按向量号索引，省得在 C++ 里逐个声明入口
_Core_Exception_Stubs:
%ASSIGN VECTOR 0
%REP 32
    DD Exc_%+VECTOR
%ASSIGN VECTOR VECTOR+1
%ENDREP

; 交接入口：32 位保护模式按 C 调用约定调用，切长模式并按恒等映射开分页，跳到内核入口，不返回
; 参数：entry = 内核入口物理地址，bootInfo = BootInfo 物理地址，pageTables = PML4 物理地址
; 进入长模式的次序按处理器要求：先 CR3 与 CR4，再 EFER.LME 与 NXE，最后 CR0.PG，
; 置位后处在兼容模式，经远跳转（RETF 装 64 位代码段）进入 64 位模式
; CR4 除 PAE 外还要置 OSFXSR 与 OSXMMEXCPT：内核按 64 位编译，编译器用 SSE 做内存清零与拷贝，
; 缺这两位的 SSE 指令在进入内核后会立刻触发异常，而那时中断表还是 BaleenCore 的 32 位表
; 参数经未初始化区的两个全局量传给 64 位一侧：32 位段的 ESP 偏移在长模式下不再成立
SECTION .text.enter progbits alloc exec nowrite
BITS 32
GLOBAL _Core_Enter_Kernel
extern _Core_Stack_Top

_Core_Enter_Kernel:
    CLI
    MOV EAX, [ESP + 4]                  ; entry
    MOV [Core_Entry_Value], EAX
    MOV EAX, [ESP + 8]                  ; bootInfo
    MOV [Core_BootInfo_Value], EAX
    MOV EAX, [ESP + 12]                 ; pageTables
    LGDT [Gdt64_Pointer]
    MOV CR3, EAX
    MOV EAX, CR4
    OR EAX, (1 << 5) | (1 << 9) | (1 << 10)   ; PAE、OSFXSR 与 OSXMMEXCPT
    MOV CR4, EAX
    MOV ECX, 0xC0000080                 ; IA32_EFER
    RDMSR
    OR EAX, (1 << 8) | (1 << 11)        ; LME 与 NXE：内核的页保护要求不可执行位
    WRMSR
    MOV EAX, CR0
    OR EAX, 0x80000001                  ; PG 与 PE
    MOV CR0, EAX
    PUSH DWORD 0x08                     ; 64 位代码段选择子：RETF 同时完成 CS 装载与模式切换
    PUSH DWORD Core_Enter64
    RETF

; 64 位一侧：段寄存器走平坦数据选择子，换到 BaleenCore 的栈顶，按 RDI 传 BootInfo 后跳内核入口
; 栈与代码都在恒等映射里；内核入口即刻换成自己的栈，这里不设置 16 字节对齐以外的约束
; 寻址都用 32 位绝对形式：地址都在低 4GiB，ELF32 目标文件里也只有 32 位重定位可用
BITS 64
DEFAULT ABS
Core_Enter64:
    MOV AX, 0x10
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV ESP, _Core_Stack_Top            ; 32 位写零扩展到 RSP
    MOV EDI, [Core_BootInfo_Value]      ; 32 位读零扩展到 RDI
    MOV EAX, [Core_Entry_Value]
    JMP RAX

; 64 位段表：0x08 为 64 位代码段（L=1、G=1），0x10 为数据段
; 只在交接入口里装载一次，随后由内核换上自己的段表
SECTION .rodata
ALIGN 8
Gdt64:
    DQ 0
    DQ 0x00AF9A000000FFFF               ; 0x08：代码段，P=1、DPL=0、可读、L=1
    DQ 0x00CF92000000FFFF               ; 0x10：数据段，P=1、DPL=0、可写
Gdt64_End:
Gdt64_Pointer:
    DW Gdt64_End - Gdt64 - 1
    DD Gdt64

; 交接参数与运行期栈：放在未初始化区，不落盘也不进镜像；Stub 交权前按头里的 MemoryBytes 清零
; 参数排在栈空间之前（低地址侧），栈顶因此不压在参数上
SECTION .bss
ALIGNB 16
Core_Entry_Value:    RESD 1              ; 内核入口物理地址
Core_BootInfo_Value: RESD 1              ; BootInfo 物理地址
ALIGNB 16
_Core_Stack_Bottom: RESB CORE_STACK_BYTES
_Core_Stack_Top:
