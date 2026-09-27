; 按本地扇区单位读盘（INT 13h AH=0x42 DAP）。入口：EAX=LBA，CX=扇区数，ES:BX=缓冲
; DL 以 [_Saved_Dl] 为准（DS 由本例程切到 CS）。成功 CF=0；失败 CF=1
; 主路径整批读：真机上命令开销是主体，stage2a 只有十几扇区，一批（上限 127 块）就够；
; 整批失败才退回逐扇区，逐扇区路径里还有无 EDD 机器的 AH=02 回退

%IFNDEF SECT_SHIFT
%DEFINE SECT_SHIFT 9
%ENDIF

; 读 CX 个扇区到 ES:BX。整批（一次 AH=42 读 min(剩余,127) 块）读不完才逐扇区
_Read_Sectors:
    MOV BP, CX
.Rs_Loop:
    TEST BP, BP
    JZ .Rs_Ok
    ; 一次 EDD 多块读 min(剩余,127) 块（EDD 上限，Phoenix 007Fh）
    MOV CX, BP
    CMP CX, 127
    JBE .Rs_Batch
    MOV CX, 127
.Rs_Batch:
    CALL Read_Range
    JNC .Rs_Advance
.Rs_One:
    MOV CX, 1
    CALL Read_Range
    JC .Rs_Fail
    INC EAX
    ADD BX, (1 << SECT_SHIFT)
    DEC BP
    JNZ .Rs_One
.Rs_Ok:
    RET
.Rs_Advance:
    MOVZX EDX, CX               ; 本批块数（Read_Range 不改 CX）
    ADD EAX, EDX                ; LBA 前进
    SHL EDX, SECT_SHIFT
    ADD BX, DX                  ; 缓冲前进
    SUB BP, CX                  ; 剩余减少
    JMP .Rs_Loop
.Rs_Fail:
    STC
    RET

; 读 CX 个连续扇区到当前 ES:BX（EAX=LBA）。成功 CF=0，失败 CF=1；入口寄存器不改
; 多块走 AH=42；单块失败且落第 0 道内时按 AH=02 重试
Read_Range:
    PUSH SI
    PUSH DS
    PUSH EAX
    PUSH BX
    PUSH CS
    POP DS                      ; DAP 在 IPL 镜像内
    MOV SI, _Dap
    MOV WORD [SI], 16           ; 部分 BIOS 会改 DAP 头，每次重写
    MOV [SI + 2], CX            ; 块数
    MOV [SI + 4], BX            ; 缓冲偏移
    MOV [SI + 6], ES            ; 缓冲段
    MOV [SI + 8], EAX           ; LBA 低 32 位
    MOV DWORD [SI + 12], 0      ; LBA 高 32 位
    MOV DL, [_Saved_Dl]
    STI
    MOV AH, 0x42
    INT 0x13
    JNC .Ok
%IF SECT_SHIFT = 9
    ; 单块、且在第 63 扇区以内：用 C=0 H=0 的 AH=02。无 INT 13h 扩展时 AH=42 会失败
    CMP CX, 1
    JNE .Fail
    CMP EAX, 63
    JAE .Fail
    MOV CL, AL
    INC CL                      ; S = LBA+1
    XOR CH, CH
    XOR DH, DH
    MOV DL, [_Saved_Dl]
    MOV AL, 1
    MOV AH, 0x02
    INT 0x13
    JC .Fail
%ENDIF
.Ok:
    POP BX
    POP EAX
    POP DS
    POP SI
    RET
.Fail:
    STC
    POP BX
    POP EAX
    POP DS
    POP SI
    RET
