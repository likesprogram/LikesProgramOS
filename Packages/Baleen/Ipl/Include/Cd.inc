; Cd.inc
; 光盘入口：按候选驱动器号探测可读且合法的描述符，再进入共同路径

%INCLUDE "Init.inc"

    CALL Cd_Try_Desc
    JC _Err_Desc

%INCLUDE "Body.inc"
%INCLUDE "Error.inc"
%INCLUDE "Read.inc"

; 保留固件 DL 优先，失败再试 E0/E1 的兼容路径；CF 表示是否取得合法描述符
Cd_Try_Desc:
    CMP BYTE [_Saved_Dl], 0x80
    JB .E0
    CALL Cd_Read_Desc
    JNC .Done
    CMP BYTE [_Saved_Dl], 0xE0
    JE .E1
.E0:
    MOV BYTE [_Saved_Dl], 0xE0
    CALL Cd_Read_Desc
    JNC .Done
.E1:
    MOV BYTE [_Saved_Dl], 0xE1
    CALL Cd_Read_Desc
.Done:
    RET

; 用当前候选号读描述符扇区并核魔数；命中时 SI 指向描述符、CF 清零
Cd_Read_Desc:
    XOR EAX, EAX
    MOV CX, 1
    MOV BX, LOAD_TOP
    CALL _Read_Sectors
    JC .Bad
    MOV SI, LOAD_TOP + DESC_OFF - BIOS_LOAD   ; 字段偏移按 BIOS_LOAD 归一
    CMP DWORD [SI], DESC_MAGIC
    JE .Done                    ; 相等时 CF 已清零，SI 交给共同校验段
.Bad:
    STC
.Done:
    RET
