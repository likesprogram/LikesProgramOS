; BITS 16 / ORG 0。BIOS 加载到 0x7C00；IPL 留在原地，不拷到 0x500
; Stage1：读固定偏移的 BootDEScriptor，把 Stub 读到 0x600，不解析 Superblock
; Stub 不得踏进 0x7C00。出口：JMP 0000:0600，DL=驱动器，DH=IPL_MEDIA

%INCLUDE "Const.inc"

; 0xE9 调试口开关，默认开；构建期 -DENABLE_E9=0 关闭（BR-21）
%IFNDEF ENABLE_E9
%DEFINE ENABLE_E9 1
%ENDIF

; 本地扇区大小的 2 对数：9=512 B（硬盘），11=2048 B（光盘）
%IFNDEF SECT_SHIFT
%DEFINE SECT_SHIFT 9
%ENDIF
; 跳入 Stub 时放入 DH 的 Boot::Media
%IFNDEF IPL_MEDIA
%DEFINE IPL_MEDIA MEDIA_HDD
%ENDIF
; 段内调用标记：IPL 全程固定 CS、不跨段，CALI 展开为近 CALL
%IFNDEF CALI
%DEFINE CALI CALL
%ENDIF

; BootDEScriptor 绝对偏移与字段偏移，与 BootDEScriptor.hpp 一致
%DEFINE DESC_PHYS   0x300
%DEFINE DESC_MAGIC  0x52445342
%DEFINE DESC_SEC    (DESC_PHYS >> SECT_SHIFT)              ; 所在扇区
%DEFINE DESC_OFF    (DESC_PHYS & ((1 << SECT_SHIFT) - 1))  ; 扇区内偏移
%DEFINE DESC_VER    4
%DEFINE DESC_HDR    6
%DEFINE DESC_LOFF   8
%DEFINE DESC_LBYTES 16
%DEFINE DESC_RESV   24

CPU 386

; BIOS 入口。统一 CS=0x07C0，使 ORG 0 标签落在物理 0x7C00
_Start:
    JMP 0x07C0:After_CS

After_CS:
    CLI
    MOV AX, CS
    MOV DS, AX
    XOR AX, AX
    MOV ES, AX
    MOV SS, AX
    MOV SP, STACK_TOP
    STI                         ; 部分 BIOS 的 INT 13h 要 IF=1
    CLD
    MOV [_Saved_Dl], DL

%IFDEF IPL_CD
    CALI Cd_Try_Desc
    JC Err_Desc
%ELSE
    MOV DL, [_Saved_Dl]          ; 先复位：硬盘与 U 盘/软盘仿真都适用
    MOV AH, 0
    INT 0x13
    XOR EAX, EAX
    MOV AX, DESC_SEC
    MOV CX, 1
    MOV BX, LOAD_TOP            ; 描述符所在扇区，随后被 Stub 覆盖
    CALI _Read_Sectors
    JC Err_Read
%ENDIF

    XOR AX, AX                  ; INT 13h 可能改 ES
    MOV ES, AX
    CMP DWORD [ES:LOAD_TOP + DESC_OFF], DESC_MAGIC
    JNE Err_Desc
    CMP WORD [ES:LOAD_TOP + DESC_OFF + DESC_VER], 1
    JNE Err_Desc
    CMP WORD [ES:LOAD_TOP + DESC_OFF + DESC_HDR], 32
    JNE Err_Desc
    CMP DWORD [ES:LOAD_TOP + DESC_OFF + DESC_RESV], 0
    JNE Err_Desc
    CMP DWORD [ES:LOAD_TOP + DESC_OFF + DESC_RESV + 4], 0
    JNE Err_Desc

    ; Stub_Offset 高 32 位必须为 0，且按扇区对齐
    MOV EDX, [ES:LOAD_TOP + DESC_OFF + DESC_LOFF + 4]
    TEST EDX, EDX
    JNZ Err_Desc
    MOV EAX, [ES:LOAD_TOP + DESC_OFF + DESC_LOFF]
    MOV EDX, EAX
    TEST EDX, (1 << SECT_SHIFT) - 1
    JNZ Err_Desc
    SHR EAX, SECT_SHIFT         ; Stub 起始 LBA

    ; Stub_Bytes 高 32 位必须为 0，且装得下、不盖住 0x7C00 的 IPL
    MOV EDX, [ES:LOAD_TOP + DESC_OFF + DESC_LBYTES + 4]
    TEST EDX, EDX
    JNZ Err_Size
    MOV ECX, [ES:LOAD_TOP + DESC_OFF + DESC_LBYTES]
    TEST ECX, ECX
    JZ Err_Size
    ; 读取按整扇区进行，先把字节数按扇区上取整再与上限比较，避免末扇区越过
    ; LOAD_CEIL（光盘 2048 时尤其明显，末端会回绕覆盖 IVT/BDA）
    MOV EDX, ECX
    ADD EDX, (1 << SECT_SHIFT) - 1
    SHR EDX, SECT_SHIFT
    SHL EDX, SECT_SHIFT
    CMP EDX, LOAD_CEIL - LOAD_TOP ; 上限：镜像含 BSS 不得越过低 64KiB
    JA Err_Size
    ADD ECX, (1 << SECT_SHIFT) - 1
    SHR ECX, SECT_SHIFT           ; 扇区数

    MOV BX, LOAD_TOP
    CALI _Read_Sectors
    JC Fail_Load
    MOV DL, [_Saved_Dl]
    MOV DH, IPL_MEDIA
    JMP 0x0000:LOAD_TOP

Fail_Load:
    MOV AL, 'L'
    JMP Print_Err
Err_Read:
    MOV AL, 'R'
    JMP Print_Err
Err_Desc:
    MOV AL, 'D'
    JMP Print_Err
Err_Size:
    MOV AL, 'S'
    JMP Print_Err

; 单字符诊断：AL 为错误码，写电传（VGA）与 0xE9 后停机
; 单字符不换行、不重定位
Print_Err:
%IF ENABLE_E9
    OUT 0xE9, AL
%ENDIF
    MOV AH, 0x0E
    INT 0x10
.Hang:
    HLT
    JMP .Hang

    ALIGN 16                    ; AH=0x42 要求 DAP 16 字节对齐
_Dap:
    DB 16, 0                    ; 长度 16、保留 0
    DW 1                        ; 块数（按批重写，见 Read.inc）
    DW 0, 0                     ; 缓冲偏移、段
    DQ 0                        ; LBA

_Saved_Dl:
    DB 0                        ; BIOS DL，供 INT 13h；放在 DAP 之后，不吃对齐填充

%INCLUDE "Read.inc"

%IFDEF IPL_CD
; 入口 DL（须 ≥0x80），再试 E0/E1；读到描述符扇区并验魔数
Cd_Try_Desc:
    MOV AL, [_Saved_Dl]
    CMP AL, 0x80
    JB .E0
    CALI Cd_Read_Desc
    JNC .Ok
    CMP BYTE [_Saved_Dl], 0xE0
    JE .E1
.E0:
    MOV BYTE [_Saved_Dl], 0xE0
    CALI Cd_Read_Desc
    JNC .Ok
.E1:
    MOV BYTE [_Saved_Dl], 0xE1
    CALI Cd_Read_Desc
.Ok:
    RET

; 读描述符所在扇区（2048 单位下为 LBA 0）并验魔数
Cd_Read_Desc:
    XOR EAX, EAX
    MOV CX, 1
    MOV BX, LOAD_TOP
    CALI _Read_Sectors
    JC .Bad
    XOR AX, AX
    MOV ES, AX
    CMP DWORD [ES:LOAD_TOP + DESC_OFF], DESC_MAGIC
    JNE .Bad
    CLC
    RET
.Bad:
    STC
    RET
%ENDIF

; 阶段失败单字符码（经电传与 0xE9 输出，见 Print_Err）：
;   'D' —— BootDescriptor 无效
;   'R' —— 读盘失败（描述符扇区或 Stub）
;   'S' —— Stub_bytes 为 0/越界，或 BootStub 镜像会越过低 64KiB
;   'L' —— Stub 扇区读盘失败
