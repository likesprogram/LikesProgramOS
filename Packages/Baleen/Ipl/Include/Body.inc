; Body.inc
;    两种介质入口共用的描述符校验、Stub 限长与范围检查、装入和交权

; 进入前提：入口已验证魔数，DS:SI 指向刚读入的 BootDescriptor
    CMP DWORD [SI + DESC_VER], (DESC_HEADER << 16) | DESC_VERSION
    JNE _Err_Desc
    MOV EDX, [SI + DESC_RESV]
    OR EDX, [SI + DESC_RESV + 4]
    OR EDX, [SI + DESC_LOFF + 4]
    JNZ _Err_Desc
    MOV EAX, [SI + DESC_LOFF]
%IFDEF IPL_CD
    TEST AX, (1 << SECT_SHIFT) - 1
    JNZ _Err_Desc
%ELSE
    MOV BX, [_Sector_Bytes]
    DEC BX
    TEST AX, BX
    JNZ _Err_Desc
%ENDIF
    CMP EAX, STUB_MIN_OFFSET
    JB _Err_Desc                 ; Stub 起点须在描述符区之后，两种单位下都安全

    CMP DWORD [SI + DESC_LBYTES + 4], 0
    JNE _Err_Size
    MOV ECX, [SI + DESC_LBYTES]
    DEC ECX
    CMP ECX, MAX_STUB_BYTES - 1  ; 先限原始长度；同时拒绝零和上取整溢出
    JA _Err_Size
    INC ECX
    MOV EDX, EAX
    ADD EDX, ECX
    JC _Err_Desc                 ; 整个文件范围必须落在低 4 GiB
%IFDEF IPL_CD
    SHR EAX, SECT_SHIFT
    ADD CX, (1 << SECT_SHIFT) - 1
    SHR CX, SECT_SHIFT
%ELSE
    ADD CX, BX                   ; 文件长度已限到 0x8200，加最多 4095 不溢出
    NOT BX
    AND CX, BX                   ; 按本地扇区上取整后的实际读取跨度
    CMP CX, LOAD_CEIL - LOAD_TOP
    JA _Err_Size
    BSF DX, BX                   ; BX 是对齐掩码，最低置位给出扇区移位量
    XCHG CX, DX
    SHR EAX, CL
    SHR DX, CL
    MOV CX, DX
%ENDIF

    ; 校验全部通过：读入 Stub，再按契约交权
    MOV BX, LOAD_TOP
    CALL _Read_Sectors
    JC _Fail_Load
    MOV DL, [_Saved_Dl]
    MOV DH, IPL_MEDIA
%IFDEF IPL_CD
    MOV CX, 1 << SECT_SHIFT
%ELSE
    MOV CX, [_Sector_Bytes]
%ENDIF
    CLD
    STI
    JMP 0x0000:LOAD_TOP
