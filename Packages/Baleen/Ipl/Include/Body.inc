; Body.inc
; 两种介质入口共用的描述符校验、Stub 限长与范围检查、装入和交权

; 进入前提：入口已验证魔数，DS:SI 指向刚读入的 BootDescriptor
    CMP DWORD [SI + DESC_VER], (DESC_HEADER << 16) | DESC_VERSION
    JNE _Err_Desc
    MOV EDX, [SI + DESC_RESV]
    OR EDX, [SI + DESC_RESV + 4]
    OR EDX, [SI + DESC_LOFF + 4]
    JNZ _Err_Desc
    MOV EAX, [SI + DESC_LOFF]
    TEST AX, (1 << SECT_SHIFT) - 1
    JNZ _Err_Desc
    CMP EAX, (DESC_SEC + 1) << SECT_SHIFT
    JB _Err_Desc                 ; Stub 不得覆盖介质上的描述符扇区

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
    SHR EAX, SECT_SHIFT
    ADD CX, (1 << SECT_SHIFT) - 1
    SHR CX, SECT_SHIFT

    ; 校验全部通过：读入 Stub，再按契约交权
    MOV BX, LOAD_TOP
    CALL _Read_Sectors
    JC _Fail_Load
    MOV DL, [_Saved_Dl]
    MOV DH, IPL_MEDIA
    CLD
    STI
    JMP 0x0000:LOAD_TOP
