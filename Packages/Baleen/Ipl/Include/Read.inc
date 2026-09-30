; Read.inc
;    EDD 读盘、有限 HDD CHS 回退、复位与有界重试、请求状态保护

; EAX=LBA、CX=块数、ES:BX=缓冲；只允许描述符或已限长的 Stub
; 一次 EDD 批量读取；失败则从原始 LBA/缓冲开始逐扇区重读
_Read_Sectors:
    MOV BP, CX
    CALL Read_Range
    JNC .Ok
.One:
    MOV CX, 1
    CALL Read_Range
    JC .Fail
    INC EAX
%IFDEF IPL_CD
    ADD BX, (1 << SECT_SHIFT)
%ELSE
    ADD BX, [_Sector_Bytes]
%ENDIF
    DEC BP
    JNZ .One
.Ok:
    CLC                         ; 最后一次 ADD BX 可能刚好回绕到 0
.Fail:
    RET

; 每个请求至多尝试三次，失败之间复位；所有 BIOS 调用恢复通用和段寄存器
; 只走 EDD：描述符与载荷位于 GPT 保留区之后（512 字节扇区下 LBA 272 起），
; 超出 C0H0 兼容范围，CHS 回退不适用
Read_Range:
    PUSH DI
    MOV DI, 3
.Retry:
    CALL Read_Edd
    JNC .Done
.Reset:
    DEC DI
    JZ .Failed
    CALL _Disk_Reset
    JMP .Retry
.Failed:
    STC
.Done:
    POP DI
    RET

; 一次 EDD 批量读取（INT 13h/AH=42h）；CF 表示结果
Read_Edd:
    PUSHAD
    PUSH DS
    PUSH ES
    MOV SI, Dap
    MOV WORD [SI], 16
    MOV [SI + 2], CX
    MOV [SI + 4], BX
    MOV [SI + 6], ES
    MOV [SI + 8], EAX
    MOV DWORD [SI + 12], 0
    MOV AH, 0x42
    JMP Disk_Int

%IFNDEF IPL_CD
; AH=48 只查询参数，寄存器保护与读盘共用
Read_Params:
    PUSHAD
    PUSH DS
    PUSH ES
    MOV AH, 0x48
    JMP Disk_Int
%ENDIF

; 复位磁盘系统（INT 13h/AH=00h），驱动器号取保存的固件值
_Disk_Reset:
    PUSHAD
    PUSH DS
    PUSH ES
    XOR AX, AX
; 所有 INT 13h 的公共出口：装载 DL、开中断、预置 CF，返回前恢复寄存器
Disk_Int:
    MOV DL, [CS:_Saved_Dl]
    STI
    STC
    INT 0x13
    POP ES
    POP DS
    POPAD
    RET

%IFDEF IPL_CD
    ALIGN 16
%ELSE
    ALIGN 4
%ENDIF
; 16 字节 EDD 磁盘地址包（DAP）：地址按 DWORD 对齐，缓冲段:偏移与 LBA 由调用方填
Dap:
    DB 16, 0
    DW 1
    DW 0, 0
    DQ 0
; 固件给的驱动器号，所有 INT 13h 调用都用它；CD 候选探测会改写
_Saved_Dl:
    DB 0
%IFNDEF IPL_CD
; 本地逻辑扇区字节数，AH=48 查询失败时保留传统硬盘的 512 字节约定
_Sector_Bytes:
    DW 512
%ENDIF
