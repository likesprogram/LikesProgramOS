; Bios.asm
;    Stub 的 BIOS 服务：从 32 位保护模式弹回实模式执行固件调用，再回到保护模式返回
;
;    弹跳走 16 位保护模式中转：装自带段表切到 16 位代码段，在保护模式里清 CR0 的 PG 与 PE，
;    再远跳进实模式；返回时反向走一遍。自带段表而不复用 Stub.cpp 建立的运行期表，
;    弹跳路径不依赖那张表的内容，运行期 GDTR 在进弹跳前保存、回来时装回
;    实模式栈取 IPL 失效区，参数表与保存的上下文取镜像的低地址区，镜像整体在低 64KiB 内
;
;    固件调用期间 CR0 与 IDTR 都可能被改写（实测 SeaBIOS 的 INT 13h 会留下 CR0.PG=1），
;    两者都在这里保存、回来时整个还原；实模式的中断走 IVT，装回 IVT 再调固件
;
;    一次弹跳只做一个操作（Bios_Op 分派），不允许嵌套；实模式侧只能寻址低 64KiB，
;    要交给调用方的数据先落在映像内的低地址缓冲，回到 32 位平坦段后再拷出去
;
;    读盘分两层：批量 EDD 请求各自最多 READ_TRIES 次尝试；某批耗尽后不整体放弃，
;    而是从原始 LBA 与目标地址起把整段范围逐扇区重读，每个单扇区请求独立计尝试，
;    HDD 且 LBA<63 时可在 EDD 失败后退回 CHS；全部尝试有界，失败只返回 0，不停机

BITS 32

%INCLUDE "Contract.inc"

SECTION .text

GLOBAL _Bios_Sector_Size
GLOBAL _Bios_E820
GLOBAL _Bios_Read_Sectors
EXTERN _Boot_Drive
EXTERN _Boot_Media

; 每扇区字节数在 AH=48 参数表里的偏移，表结构见 EDD 规范
%DEFINE PARAM_SECTOR_BYTES_OFF 0x18
; AH=48 参数表长度：取 4 的倍数便于按 DWORD 清零，比规范要求的 66 字节更宽
%DEFINE PARAM_TABLE_BYTES 68
; 实模式调用期的栈顶：IPL 已失效，栈从那里向下增长
%DEFINE REAL_STACK_TOP STACK_TOP
; 弹跳段表的项数与选择子，与下面 Bios_Gdt 的项序一致
%DEFINE BIOS_GDT_ENTRIES 5
%DEFINE SEL_CODE32 0x08
%DEFINE SEL_DATA32 0x10
%DEFINE SEL_CODE16 0x18
%DEFINE SEL_DATA16 0x20
; 弹跳操作码
%DEFINE OP_SECTOR_SIZE 1
%DEFINE OP_E820 2
%DEFINE OP_READ 3
; E820 条目字节数与低地址缓冲的条目数上限
; 上限与 Core.cpp 的内存图静态缓冲一致：这张图要随交权块一并交给 Core
%DEFINE E820_ENTRY_BYTES 24
%DEFINE E820_MAX 64
; EDD 一次传输的块数上限（Phoenix EDD 记 007Fh）；一次传输还须落同一个 64KiB 窗口
%DEFINE BLOCK_MAX 127
%DEFINE WINDOW_BYTES 0x10000
; 实模式段基址决定的传输上界：1MiB，不含
%DEFINE REAL_CEIL 0x100000
; 每次读盘请求（批量或单扇区）与 E820 的重试次数，含首次
%DEFINE READ_TRIES 3
; CHS 回退的 LBA 上限，不含：只覆盖 C=0、H=0 的首 63 个扇区
%DEFINE CHS_LAST_LBA 63

; 探测启动驱动器的扇区大小：只认 512、2048、4096，其他结果与调用失败都返回 0
_Bios_Sector_Size:
    PUSH EBX
    PUSH ESI
    PUSH EDI
    PUSH EBP
    PUSHFD
    MOV EDI, Bios_Param_Table
    MOV ECX, PARAM_TABLE_BYTES / 4
    XOR EAX, EAX
    REP STOSD
    MOV WORD [Bios_Param_Table], PARAM_TABLE_BYTES   ; 调用前告知缓冲长度
    MOV BYTE [Bios_Ok], 0
    MOV DWORD [Bios_Op], OP_SECTOR_SIZE
    CALL Enter_Real
    TEST BYTE [Bios_Ok], 0xFF                        ; 已回到 32 位平坦段
    JZ .Zero
    MOVZX EAX, WORD [Bios_Param_Table + PARAM_SECTOR_BYTES_OFF]
    CMP EAX, 512
    JE .Out
    CMP EAX, 2048
    JE .Out
    CMP EAX, 4096
    JE .Out
.Zero:
    XOR EAX, EAX
.Out:
    POPFD
    POP EBP
    POP EDI
    POP ESI
    POP EBX
    RET

; 取 E820 内存图：cdecl 参数为 dest、maxCount、truncated，返回写入的条目数
; 条目先写进映像内的低地址缓冲（实模式只认低 64KiB），回 32 位侧再拷给调用方指针；
; 缓冲放不下而固件还有后续条目时，置 truncated 为 1
_Bios_E820:
    PUSH EBP
    MOV EBP, ESP
    PUSH EBX
    PUSH ESI
    PUSH EDI
    MOV DWORD [Bios_E820_Dest], 0
    MOV DWORD [Bios_E820_Trunc], 0
    MOV DWORD [Bios_E820_Count], 0
    MOV DWORD [Bios_E820_Limit], 0
    MOV EAX, [EBP + 8]                  ; dest，可为 0：只要条数
    MOV [Bios_E820_Dest], EAX
    MOV EAX, [EBP + 12]                 ; maxCount
    TEST EAX, EAX
    JZ .Run                             ; 上限 0：只为取条数，缓冲截断即可
    CMP EAX, E820_MAX
    JBE .Store_limit
    MOV EAX, E820_MAX
.Store_limit:
    MOV [Bios_E820_Limit], EAX
.Run:
    MOV BYTE [Bios_Ok], 0
    MOV DWORD [Bios_Op], OP_E820
    CALL Enter_Real
    ; 已回到 32 位平坦段：把低地址缓冲里的条目拷回调用方指针
    MOV EAX, [Bios_E820_Dest]
    TEST EAX, EAX
    JZ .Trunc
    MOV ECX, [Bios_E820_Count]
    IMUL ECX, ECX, E820_ENTRY_BYTES
    MOV ESI, Bios_E820_Buf
    MOV EDI, EAX
    REP MOVSB
.Trunc:
    MOV EAX, [EBP + 16]                 ; truncated，可为 0
    TEST EAX, EAX
    JZ .Count
    MOV ECX, [Bios_E820_Trunc]
    MOV [EAX], ECX
.Count:
    MOV EAX, [Bios_E820_Count]
    POP EDI
    POP ESI
    POP EBX
    POP EBP
    RET

; 读盘：cdecl 参数为 lba、count、dest、sectBytes，成功返回 1，失败 0
; dest 须 16 字节对齐、整段落低 1MiB，且每批传输不跨 64KiB 窗口：EDD 的 DAP 用 seg:off
; 表达缓冲，超过就静默写错位置，所以这里显式拒绝；count 可以大于一次传输的上限，内部分批
_Bios_Read_Sectors:
    PUSH EBP
    MOV EBP, ESP
    PUSH EBX
    PUSH ESI
    PUSH EDI
    MOV EAX, [EBP + 8]                  ; lba
    MOV [Bios_Lba], EAX
    ; 原始 LBA 与目标地址另存：批量失败后的逐扇区重读必须从它们开始，而不是从失败点
    MOV [Bios_Orig_Lba], EAX
    MOV EAX, [EBP + 12]                 ; count
    MOV [Bios_Count], EAX
    MOV EAX, [EBP + 16]                 ; dest
    MOV [Bios_Dest], EAX
    MOV [Bios_Orig_Dest], EAX
    MOV EAX, [EBP + 20]                 ; sectBytes
    MOV [Bios_Sect], EAX
    MOV DWORD [Bios_Done], 0
    MOV BYTE [Bios_Ok], 0
    ; 参数检查：块数非 0、扇区大小是可辨识的值、目标 16 字节对齐且整段落低 1MiB 内
    CMP DWORD [Bios_Count], 0
    JE .Reject
    MOV EAX, [Bios_Sect]
    CMP EAX, 512
    JE .Check_range
    CMP EAX, 2048
    JE .Check_range
    CMP EAX, 4096
    JE .Check_range
    JMP .Reject
.Check_range:
    MOV EAX, [Bios_Dest]
    TEST EAX, 0xF
    JNZ .Reject                         ; DAP 的段基址是线性地址 >> 4，未对齐会丢低 4 位
    MOV ECX, [Bios_Count]
    IMUL ECX, [Bios_Sect]
    ADD ECX, EAX
    JC .Reject                          ; 字节数溢出
    CMP ECX, REAL_CEIL
    JA .Reject                          ; 超低 1MiB：需要 unreal 拷贝的路径另行实现
    MOV DWORD [Bios_Op], OP_READ
    CALL Enter_Real
    MOVZX EAX, BYTE [Bios_Ok]
    POP EDI
    POP ESI
    POP EBX
    POP EBP
    RET
.Reject:
    XOR EAX, EAX                        ; 参数非法：失败返回，由调用方按既有语义降级
    POP EDI
    POP ESI
    POP EBX
    POP EBP
    RET

; 进入实模式：保存运行期上下文，装自带段表切到 16 位代码段
; 返回点是紧跟 CALL 之后的那条指令，由 Bios_Prot_Entry 的 RET 回到这里；不允许嵌套
Enter_Real:
    MOV [Bios_Saved_Esp], ESP
    MOV EAX, CR0
    MOV [Bios_Saved_Cr0], EAX
    SGDT [Bios_Saved_Gdtr]      ; 运行期段表：弹跳期间换成自带表，回来要装回
    SIDT [Bios_Saved_Idtr]
    CLD                         ; 参数表清零与本文件的 REP 例程都依赖 DF=0
    CLI
    ; 段表模板只读，每次弹跳前把副本拷进未初始化区：
    ; 加载 16 位选择子会置位描述符的 Accessed 位并写回，写回必须落在镜像之外
    MOV ESI, Bios_Gdt
    MOV EDI, Bios_Gdt_Work
    MOV ECX, (BIOS_GDT_ENTRIES * 8) / 4
    REP MOVSD
    LGDT [Bios_Gdtr]
    JMP DWORD SEL_CODE16:Real_Mode_Setup

BITS 16
Real_Mode_Setup:
    MOV AX, SEL_DATA16          ; 16 位保护模式里先给段寄存器一个有效选择子
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV EAX, [Bios_Saved_Cr0]
    AND EAX, 0x7FFFFFFE         ; 一次写完：中间不出现实模式加分页的状态
    MOV CR0, EAX
    JMP 0x0000:Real_Mode_Entry  ; 远跳转把 CS 重载成实模式段

Real_Mode_Entry:
    XOR AX, AX
    MOV SS, AX                  ; 先建实模式栈：清 PE 后段基址按选择子重新计算
    MOV SP, REAL_STACK_TOP      ; 栈在 IPL 失效区，不占用镜像的数据区
    MOV DS, AX
    MOV ES, AX
    LIDT [Bios_Real_Idtr]       ; 实模式的中断走 IVT，显式装回以免 INT 踩空表
    STI                         ; 磁盘完成靠 IRQ 判定，IF=0 时部分 BIOS 永不返回
    MOV EAX, [Bios_Op]
    CMP EAX, OP_SECTOR_SIZE
    JE Real_Sector_Size
    CMP EAX, OP_E820
    JE Real_E820
    CMP EAX, OP_READ
    JE Real_Read
    MOV BYTE [Bios_Ok], 0
    JMP Real_Leave

; INT 13h AH=48：取驱动器参数，结果写到参数表
Real_Sector_Size:
    MOV AH, 48h
    MOV DL, [_Boot_Drive]
    MOV SI, Bios_Param_Table
    INT 13h
    CLI
    SETC BL
    XOR AX, AX
    MOV DS, AX
    MOV ES, AX
    TEST BL, BL
    JNZ .Fail
    MOV BYTE [Bios_Ok], 1
    JMP Real_Leave
.Fail:
    MOV BYTE [Bios_Ok], 0
    JMP Real_Leave

; INT 15h E820：条目写进映像内的低地址缓冲，条数与截断标志留给 32 位侧
Real_E820:
    MOV EBX, 0                  ; 固件要求首次调用前清零，其后由它回传续篇标记
    MOV EDI, Bios_E820_Buf
    MOV ESI, [Bios_E820_Limit]
    MOV DWORD [Bios_E820_Retry], 0
.Loop:
    TEST ESI, ESI
    JZ .CapFull
    MOV EAX, 0xE820
    MOV EDX, 0x534D4150
    MOV ECX, E820_ENTRY_BYTES
    INT 15h
    CLI
    JC .MaybeEnd
    CMP EAX, 0x534D4150         ; 固件回显签名才算这一条有效
    JNE .Done
    CMP ECX, 20                 ; 返回长度不足 20 的条目不含完整字段，丢弃
    JB .Next
    ; ACPI 3.0 起字节 20 是属性位：bit0 为 0 表示该条目应被忽略
    CMP ECX, E820_ENTRY_BYTES
    JAE .Attr
    MOV DWORD [EDI + 20], 1     ; 旧式条目没有属性字段，按「应保留」处理
    JMP .Keep
.Attr:
    TEST DWORD [EDI + 20], 1
    JZ .Next
.Keep:
    ADD EDI, E820_ENTRY_BYTES
    INC DWORD [Bios_E820_Count]
    DEC ESI
.Next:
    TEST EBX, EBX               ; 固件回传 0 表示表已结束
    JNZ .Loop
    JMP .Done
.CapFull:
    TEST EBX, EBX
    JZ .Done
    MOV DWORD [Bios_E820_Trunc], 1
    JMP .Done
.MaybeEnd:
    TEST EBX, EBX               ; CF 且 EBX=0 是正常结束；否则是瞬时错误，有限次重试
    JZ .Done
    INC DWORD [Bios_E820_Retry]
    CMP DWORD [Bios_E820_Retry], READ_TRIES
    JAE .Done
    JMP .Loop
.Done:
    MOV BYTE [Bios_Ok], 1
    JMP Real_Leave

; INT 13h AH=42：按 BLOCK_MAX 与 64KiB 窗口分批读，一批一次调用
; 某批耗尽尝试后转入 Real_Read_Fallback：逐扇区重读整段范围，不在这里整体判失败
Real_Read:
    MOV DWORD [Bios_Done], 0
.Loop:
    MOV EAX, [Bios_Count]
    SUB EAX, [Bios_Done]
    JZ .Ok
    CMP EAX, BLOCK_MAX
    JBE .Batch
    MOV EAX, BLOCK_MAX
.Batch:
    MOV [Bios_Blocks], EAX
    ; 本批目的地 = dest + 已搬块数 × 扇区大小；批起始地址决定它落在哪个 64KiB 窗口
    MOV EAX, [Bios_Done]
    IMUL EAX, DWORD [Bios_Sect]
    ADD EAX, [Bios_Dest]
    MOV [Bios_Block_Dest], EAX
    ; 本批不得跨 64KiB 窗口，否则 DAP 的 16 位偏移会绕回；超出就把批裁小
    MOV ECX, [Bios_Block_Dest]
    AND ECX, 0xFFFF
    MOV EDX, WINDOW_BYTES
    SUB EDX, ECX                        ; 本窗口内还剩多少字节
    MOV EBX, [Bios_Sect]
    MOV EAX, EDX
    XOR EDX, EDX
    DIV EBX                             ; 满打满算能放多少扇区
    TEST EAX, EAX
    JZ .Fail                            ; 连一个扇区都放不下：显式失败，不静默截断
    CMP EAX, [Bios_Blocks]
    JAE .Fill
    MOV [Bios_Blocks], EAX
.Fill:
    CALL Real_Fill_Dap
    MOV BYTE [Bios_Tries], READ_TRIES
.Retry:
    MOV DL, [_Boot_Drive]
    MOV SI, Bios_Dap
    MOV AH, 42h
    INT 13h
    CLI
    JNC .Batch_Ok
    CALL Real_Disk_Reset               ; 失败才复位再重试，正常路径一次都不发
    DEC BYTE [Bios_Tries]
    JNZ .Retry
    JMP Real_Read_Fallback             ; 本批耗尽：整段从原始 LBA 与目标地址逐扇区重读
.Batch_Ok:
    MOV EAX, [Bios_Blocks]
    ADD [Bios_Lba], EAX
    ADD [Bios_Done], EAX
    JMP .Loop
.Ok:
    MOV BYTE [Bios_Ok], 1
    JMP Real_Leave
.Fail:
    MOV BYTE [Bios_Ok], 0               ; 连一个扇区都放不下：布局错误，逐扇区同样放不下
    JMP Real_Leave

; 批量失败后的逐扇区重读：从原始 LBA 与目标地址起读完整段，不复用失败批次的位置
; 每个单扇区请求独立计 READ_TRIES 次尝试，每次先 EDD，再在适用时退回 CHS
; 任一扇区耗尽尝试即整段失败返回 0；与批量路径共用会话一次的复位策略
Real_Read_Fallback:
    MOV DWORD [Bios_Done], 0
.Loop:
    MOV EAX, [Bios_Count]
    SUB EAX, [Bios_Done]
    JZ .Ok
    ; 本扇区 LBA = 原始 LBA + 已搬扇区数；目的地 = 原始目的地 + 已搬扇区数 × 扇区大小
    MOV EAX, [Bios_Done]
    ADD EAX, [Bios_Orig_Lba]
    MOV [Bios_Lba], EAX
    MOV EAX, [Bios_Done]
    IMUL EAX, DWORD [Bios_Sect]
    ADD EAX, [Bios_Orig_Dest]
    MOV [Bios_Block_Dest], EAX
    MOV DWORD [Bios_Blocks], 1
    ; 单扇区同样不得跨 64KiB 窗口：批量路径只裁剪过已处理的批次，这里要自己挡
    MOV ECX, [Bios_Block_Dest]
    AND ECX, 0xFFFF
    MOV EDX, WINDOW_BYTES
    SUB EDX, ECX
    CMP EDX, [Bios_Sect]
    JB .Fail
    CALL Real_Fill_Dap
    MOV BYTE [Bios_Tries], READ_TRIES
.Retry:
    MOV DL, [_Boot_Drive]
    MOV SI, Bios_Dap
    MOV AH, 42h
    INT 13h
    CLI
    JNC .Next
    CALL Real_Read_Chs                  ; 不适用或失败都返回 CF=1，继续走复位重试
    JNC .Next
    CALL Real_Disk_Reset
    DEC BYTE [Bios_Tries]
    JNZ .Retry
.Fail:
    MOV BYTE [Bios_Ok], 0
    JMP Real_Leave
.Next:
    INC DWORD [Bios_Done]
    JMP .Loop
.Ok:
    MOV BYTE [Bios_Ok], 1
    JMP Real_Leave

; 单扇区 CHS 回退：EDD 失败后的兼容路径，只在 LBA<CHS_LAST_LBA、非光盘、
; 驱动器号 ≥ 0x80 时尝试，编码固定 C=0、H=0、S=LBA+1，不做任意几何换算
; LBA 与目的地一律取本文件的存储值：BIOS 返回后 AH 是状态、寄存器和 DAP 都不可信
; CF=1 表示不适用或读失败；返回前中断已恢复关闭
Real_Read_Chs:
    CMP DWORD [Bios_Lba], CHS_LAST_LBA
    JAE .Skip
    CMP BYTE [_Boot_Media], MEDIA_CDROM
    JE .Skip
    CMP BYTE [_Boot_Drive], 0x80
    JB .Skip
    MOV EAX, [Bios_Block_Dest]
    MOV BX, AX
    AND BX, 0xF                         ; 段内偏移取线性地址低 4 位，段取其余高位
    SHR EAX, 4
    MOV ES, AX
    MOV EAX, [Bios_Lba]
    MOV CX, AX
    INC CX                              ; 扇区号 = LBA+1；LBA<63 保证不进位到柱面位
    XOR DH, DH
    MOV AX, 0x0201
    MOV DL, [_Boot_Drive]
    STI
    INT 13h
    CLI
    RET
.Skip:
    STC
    RET

; 按 Bios_Lba、Bios_Blocks、Bios_Block_Dest 填 EDD 的 DAP
; 偏移字段只有 16 位，段基址取线性地址 >> 4：目的地必须 16 字节对齐且不跨窗口
Real_Fill_Dap:
    MOV SI, Bios_Dap
    MOV WORD [SI], 16                   ; 包长
    MOV AX, [Bios_Blocks]
    MOV [SI + 2], AX                    ; 块数
    MOV EAX, [Bios_Block_Dest]
    MOV DX, AX
    AND DX, 0xF
    MOV [SI + 4], DX                    ; 偏移 = 线性地址低 4 位
    SHR EAX, 4
    MOV [SI + 6], AX                    ; 段 = 线性地址 >> 4
    MOV EAX, [Bios_Lba]
    MOV [SI + 8], EAX                   ; LBA 低 32 位
    MOV DWORD [SI + 12], 0              ; LBA 高 32 位
    RET

; INT 13h AH=00：设备复位。只在读失败的重试路径里发，一次会话至多一次
; 光盘不复位：复位对光驱的开销大且不解决读失败；号小于 0x80 的也不是硬盘
Real_Disk_Reset:
    CMP BYTE [Bios_Reset], 0
    JNE .Done
    CMP BYTE [_Boot_Media], MEDIA_CDROM
    JE .Done
    MOV DL, [_Boot_Drive]
    CMP DL, 0x80
    JB .Done
    MOV BYTE [Bios_Reset], 1
    XOR AH, AH
    INT 13h
    CLI
.Done:
    RET

; 回保护模式：装回运行期段表与 CR0，远跳转到 32 位代码段
Real_Leave:
    CLI
    XOR AX, AX                  ; 固件调用会留下自己的 DS/ES，回保护模式前访问镜像内的数据
    MOV DS, AX                  ; 必须先把数据段归零，否则读到的偏移落在固件的段里
    MOV ES, AX
    LGDT [Bios_Saved_Gdtr]
    MOV EAX, [Bios_Saved_Cr0]
    MOV CR0, EAX
    JMP DWORD SEL_CODE32:Bios_Prot_Entry

BITS 32
Bios_Prot_Entry:
    LIDT [Bios_Saved_Idtr]      ; 还原保护模式 IDT，异常门才继续有效
    MOV AX, SEL_DATA32
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV FS, AX
    MOV GS, AX
    MOV ESP, [Bios_Saved_Esp]
    RET

SECTION .data
; 弹跳专用段表模板：空、32 位代码、32 位数据、16 位代码、16 位数据
; 模板只读，每次弹跳由 Enter_Real 拷进未初始化区的工作副本再加载
Bios_Gdt:
    DQ 0
    DQ 0x00CF9A000000FFFF       ; 0x08：32 位代码
    DQ 0x00CF92000000FFFF       ; 0x10：32 位数据
    DQ 0x00009A000000FFFF       ; 0x18：16 位代码
    DQ 0x000092000000FFFF       ; 0x20：16 位数据
Bios_Gdtr:
    DW BIOS_GDT_ENTRIES * 8 - 1
    DD Bios_Gdt_Work            ; 基址指向工作副本，CPU 的 Accessed 位写回不碰镜像
; 实模式 IVT 的伪描述符：16 位限长加 32 位基址
Bios_Real_Idtr:
    DW 0x3FF
    DD 0

SECTION .bss
ALIGNB 4
Bios_Gdt_Work:       RESB BIOS_GDT_ENTRIES * 8   ; 弹跳段表的工作副本
ALIGNB 4
Bios_Op:             RESD 1                      ; 本次弹跳的操作码
Bios_Saved_Esp:      RESD 1                      ; 弹跳前的 32 位栈指针
Bios_Saved_Cr0:      RESD 1                      ; 弹跳前的 CR0；固件调用会改写它
ALIGNB 4
Bios_Saved_Gdtr:     RESB 6                      ; 弹跳前的 GDTR
ALIGNB 4
Bios_Saved_Idtr:     RESB 6                      ; 弹跳前的 IDTR
ALIGNB 4
Bios_Ok:             RESB 1                      ; 本次操作是否成功
Bios_Reset:          RESB 1                      ; 本次会话是否已发过设备复位
Bios_Tries:          RESB 1                      ; 当前批剩余重试次数
ALIGNB 4
Bios_Lba:            RESD 1                      ; 读盘的当前 LBA
Bios_Count:          RESD 1                      ; 读盘总块数
Bios_Done:           RESD 1                      ; 已搬块数
Bios_Blocks:         RESD 1                      ; 本批块数；填 DAP 前会被窗口裁小
Bios_Dest:           RESD 1                      ; 目的地线性地址
Bios_Block_Dest:     RESD 1                      ; 本批目的地线性地址
Bios_Sect:           RESD 1                      ; 每扇区字节数
Bios_Orig_Lba:       RESD 1                      ; 调用方给出的原始 LBA，逐扇区回退从这里开始
Bios_Orig_Dest:      RESD 1                      ; 调用方给出的原始目的地，逐扇区回退从这里开始
ALIGNB 4
Bios_E820_Dest:      RESD 1                      ; E820 调用方缓冲指针，可为 0
Bios_E820_Limit:     RESD 1                      ; E820 本次接受的条目数上限
Bios_E820_Count:     RESD 1                      ; E820 已写入条数
Bios_E820_Trunc:     RESD 1                      ; E820 是否截断
Bios_E820_Retry:     RESD 1                      ; E820 瞬时失败重试计数
ALIGNB 4
Bios_Dap:            RESB 16                     ; EDD 的 DAP
ALIGNB 4
Bios_Param_Table:    RESB PARAM_TABLE_BYTES      ; AH=48 的设备参数表
ALIGNB 4
Bios_E820_Buf:       RESB E820_MAX * E820_ENTRY_BYTES   ; E820 条目的低地址缓冲
