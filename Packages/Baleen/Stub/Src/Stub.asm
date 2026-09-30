; Stub.asm
;    Stub 实模式入口与运行期异常入口桩：IPL 跳到 0000:7E00 后经入口前缀进入，保存 DL/DH/CX，切到 32 位后调用 _Stub_Main；
;   异常桩把向量号压栈后交给 C++ 侧的停机诊断，整个文件须留在低 64KiB
;
;    文件偏移 0..2 是 16 位近跳转入口前缀，[0x03,0x10) 保留，[0x10,0x90) 是自身完整性头，
;    0x90 起才是代码；头里的 ImageBytes 与 MemoryBytes 由链接器按镜像、未初始化区末尾填入，
;    BuildId 与 Digest 由 Tools/Bin/PackImage 填充，布局与断言见 Stub.ld
BITS 16
%INCLUDE "Const.inc"
%INCLUDE "StubHeader.inc"

CPU 386

; 自身完整性头与入口前缀：排在镜像最前，三处关键偏移由紧随其后的断言盯住
SECTION .stub.head progbits alloc exec nowrite

    JMP NEAR $ + STUB_ENTRY_OFFSET      ; 从固定装入点跳到头之后的入口，位移由本行位置直接算出
%IF ($ - $$) != 3
    %ERROR "入口前缀不是 3 字节的 16 位近跳转"
%ENDIF
    TIMES STUB_HEAD_OFFSET - ($ - $$) DB 0
%IF ($ - $$) != STUB_HEAD_OFFSET
    %ERROR "Stub 头未落在约定的文件偏移"
%ENDIF
    DB "BLNSTUB", 0                     ; Magic：格式标记，含终止零
    DW STUB_VERSION                     ; Version：未发布的开发格式标记
    DW STUB_HEAD_BYTES                  ; HeaderBytes：头固定长度
    DD STUB_FLAGS                       ; Flags：当前不定义可选标志
    DD __image_end - LOAD_TOP           ; ImageBytes：文件字节数，按镜像末尾由链接器填入
    DD __bss_end - LOAD_TOP             ; MemoryBytes：静态内存跨度，按未初始化区末尾由链接器填入
    DD STUB_ENTRY_OFFSET                ; EntryOffset：早期初始化入口的文件偏移
    DW STUB_DIGEST_SHA256               ; DigestAlgorithm：1 为 SHA-256，无“禁用摘要”值
    DW STUB_BUILDID_BYTES               ; BuildIdBytes：BuildId 长度
%IF ($ - $$) != STUB_HEAD_OFFSET + STUB_BUILDID_OFFSET
    %ERROR "BuildId 未落在头内约定的偏移"
%ENDIF
    TIMES STUB_BUILDID_BYTES DB 0       ; BuildId：由打包器填充
%IF ($ - $$) != STUB_HEAD_OFFSET + STUB_DIGEST_OFFSET
    %ERROR "Digest 未落在头内约定的偏移"
%ENDIF
    TIMES 32 DB 0                       ; Digest：由打包器按第四节填充
%IF ($ - $$) != STUB_HEAD_OFFSET + STUB_HEAD_BYTES - STUB_RESERVE_BYTES
    %ERROR "保留区未落在头内约定的偏移"
%ENDIF
    TIMES STUB_RESERVE_BYTES DB 0       ; Reserved：全 0
%IF ($ - $$) != STUB_HEAD_OFFSET + STUB_HEAD_BYTES
    %ERROR "Stub 头长度不是约定的 0x80"
%ENDIF

; 入口节：头之后的第一条指令，必须落在 STUB_ENTRY_OFFSET，否则入口前缀跳错位置
SECTION .text.start progbits alloc exec nowrite

GLOBAL _Start
GLOBAL _Boot_Drive
GLOBAL _Boot_Media
GLOBAL _Boot_Sector_Bytes
GLOBAL _Stub_Exception_Stubs
extern _Stub_Main
extern _Stub_Exception_Handler
; 链接脚本给出的镜像与未初始化区边界
extern __image_end
extern __bss_start
extern __bss_bytes
extern __bss_end

; IPL 约定 CS:IP=0000:7E00。出口：32 位、调用 _Stub_Main
_Start:
    XOR AX, AX
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV SP, STACK_TOP
    MOV BP, CX                  ; IPL 给出的本地扇区大小，清零 BSS 时 CX 会被使用
    CLD                         ; 下面的 REP 串操作按递增方向
    ; 未初始化区不落盘，介质上没有它的内容，这里在实模式下按链接脚本给出的边界整体清零
    ; 清零必须赶在下面写 GDT 工作副本之前：工作副本也在未初始化区
    MOV DI, __bss_start
    MOV CX, __bss_bytes         ; 长度由链接脚本按未初始化区起止算出
    XOR AX, AX
    MOV BX, CX
    SHR CX, 1
    REP STOSW
    MOV CX, BX
    AND CX, 1
    REP STOSB
    ; 段表模板在镜像里只读；CPU 加载段选择子会置位描述符的 Accessed 位并写回，
    ; 把副本拷进未初始化区再加载，写回就落在镜像之外
    MOV SI, Gdt_Template
    MOV DI, Gdt_Work
    MOV CX, (Gdt_Template_End - Gdt_Template) / 2
    REP MOVSW
    CLI
    O32 LGDT [Gdt_Desc]         ; 指向未初始化区的工作副本，运行期表由 C++ 侧建立
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
    CLD
    ; 驱动器与介质号写进未初始化区：自检要求镜像字节在装入后保持原样，落盘的变量不能在这里写
    MOV [_Boot_Drive], DL
    MOV [_Boot_Media], DH
    MOV [_Boot_Sector_Bytes], BP
    CALL _Stub_Main
.Hang:
    HLT
    JMP .Hang

; 进入保护模式所需的最小段表模板：空描述符、32 位代码段与 32 位数据段
; 模板只读，运行时由 _Start 拷进未初始化区的工作副本再加载：
; CPU 会在加载选择子时置位描述符的 Accessed 位并写回，写回必须落在镜像之外
; 运行期段表由 Platform::Gdt 在 _Stub_Main 里建立，选择子 0x08 与 0x10 的含义与这里一致
Gdt_Template:
    DQ 0                        ; 空
    DQ 0x00CF9A000000FFFF       ; 0x08：32 位代码
    DQ 0x00CF92000000FFFF       ; 0x10：32 位数据
Gdt_Template_End:

; GDTR：限长取自模板，基址指向工作副本；CPU 不会改写它，可以留在镜像里
Gdt_Desc:
    DW Gdt_Template_End - Gdt_Template - 1
    DD Gdt_Work

; 启动后写入的运行期状态与段表工作副本：放在未初始化区，不落盘，也不参与镜像自检
; 落盘数据区只留只读内容，否则写入会改掉自检要读的镜像字节
SECTION .bss
Gdt_Work:      RESB Gdt_Template_End - Gdt_Template   ; 入口段表的工作副本
_Boot_Drive:   RESB 1           ; BIOS 驱动器号，IPL 交权时的 DL
_Boot_Media:   RESB 1           ; Boot::Media，IPL 交权时的 DH
_Boot_Sector_Bytes: RESW 1      ; IPL 交权时的 CX：512 / 2048 / 4096

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
