; Hdd.inc
; 硬盘与 U 盘入口：复位读盘、读描述符扇区、核魔数后进入共同路径

%INCLUDE "Init.inc"

; 先复位：硬盘与 U 盘 / 软盘仿真都适用
    CALL _Disk_Reset
    XOR EAX, EAX
    MOV AX, DESC_SEC
    MOV CX, 1
    MOV BX, LOAD_TOP
    CALL _Read_Sectors
    JC _Err_Read
    MOV SI, LOAD_TOP + DESC_OFF - BIOS_LOAD   ; 字段偏移按 BIOS_LOAD 归一
    CMP DWORD [SI], DESC_MAGIC
    JNE _Err_Desc

%INCLUDE "Body.inc"
%INCLUDE "Error.inc"
%INCLUDE "Read.inc"
