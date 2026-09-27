; Error.inc
; IPL 失败诊断：把单字符错误码写到电传与 0xE9，然后停机

%IFNDEF IPL_CD
; 描述符所在扇区读取失败（仅硬盘入口）
_Err_Read:
    MOV AL, 'R'
    JMP Print_Err
%ENDIF
; 描述符通过后，Stub 所需扇区读取最终失败
_Fail_Load:
    MOV AL, 'L'
    JMP Print_Err
; 描述符魔数、版本、头长、保留字段或偏移非法
_Err_Desc:
    MOV AL, 'D'
    JMP Print_Err
; Stub 长度高 32 位非零、为零或超过介质上限
_Err_Size:
    MOV AL, 'S'

; 输出 AL 中的错误码并停机；AH=0x0F 先取当前显示页，避免沿用读盘缓冲的高字节
Print_Err:
%IF ENABLE_E9
    OUT 0xE9, AL
%ENDIF
    PUSH AX
    MOV AH, 0x0F
    INT 0x10
    MOV BL, 7
    POP AX
    MOV AH, 0x0E
    INT 0x10
    CLI
; 失败路径不得跳入 Stub，一律停在这里
.Hang:
    HLT
    JMP .Hang
