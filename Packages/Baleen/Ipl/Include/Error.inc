; Error.inc
;    IPL 失败诊断：把整数错误码转成一位十六进制字符写到电传与 0xE9，然后停机
;
;    码值在失败点装入 AL，取值见 Const.inc 的 ERR_*；码与含义、登记编号的对应
;    见 Docs/Specs/Baleen/失败诊断编号.md 第一节。一位十六进制是本阶段预算下的
;    上限，码值只取 1..7；超过 0xF 需要连同输出程序一起改

%IFNDEF IPL_CD
; 描述符所在扇区读取失败（仅硬盘入口）
_Err_Read:
    MOV AL, ERR_DESC_READ
    JMP Print_Err
; 驱动器号或 AH=48 查询结果不受支持（仅硬盘入口）
_Err_Media:
    MOV AL, ERR_MEDIA
    JMP Print_Err
%ENDIF

; 描述符头部或段序列非法
_Err_Head:
    MOV AL, ERR_DESC_HEAD
    JMP Print_Err
; Stub 段的起止位置非法
_Err_Range:
    MOV AL, ERR_STUB_RANGE
    JMP Print_Err

%IFDEF IPL_CD
; 光盘候选驱动器探测未取得合法描述符
_Err_Probe:
    MOV AL, ERR_CD_PROBE
    JMP Print_Err
%ENDIF

; Stub 扇区数超过装载窗口上限
_Err_Size:
    MOV AL, ERR_STUB_SIZE
    JMP Print_Err

; 描述符通过后 Stub 读盘最终失败
_Fail_Load:
    MOV AL, ERR_STUB_READ

; 输出 AL 中的错误码并停机；AH=0x0F 先取当前显示页，避免沿用读盘缓冲的高字节
Print_Err:
    PUSH AX
    MOV AH, 0x0F
    INT 0x10
    MOV BL, 7
    POP AX
    ; 0..15 转 ASCII：先加 0x90 再走两次 DAA，得到 '0'..'9' 与 'A'..'F'
    ADD AL, 0x90
    DAA
    ADC AL, 0x40
    DAA
%IF ENABLE_E9
    OUT 0xE9, AL
%ENDIF
    MOV AH, 0x0E
    INT 0x10
    CLI
; 失败路径不得跳入 Stub，一律停在这里
.Hang:
    HLT
    JMP .Hang
