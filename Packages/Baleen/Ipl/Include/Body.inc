; Body.inc
;    两种介质入口共用的描述符区解析、Stub 装载与交权
;
;    进入前提：描述符区首扇区已读入缓冲，DS:SI 指向缓冲起点
;    头部给出本区起止位置；其后是段序列：每段以段头开头（标记、版本、段长），
;    读取方按段长跳到下一个、按标记认段；顺序不作要求，加段不必改本文件
;    头部与段内的位置字段都是 512 字节基准（值 = 字节偏移 / 512），与目标设备的逻辑扇区
;    单位无关；本文件按本地单位右移换算成本地 LBA 后再读盘

    CMP DWORD [SI], DESC_AREA_MAGIC
    JNE _Err_Desc

    ; 头部本区起始位置必须与槽一致：两者都是 512 字节基准，不必换算
    MOV EAX, [SI + DESC_AREA_LBA]
    CMP EAX, [DESC_SLOT_OFF]
    JNE _Err_Desc                ; 与装载方据以定位的槽值不符

    ; 本扇区字节数：段头必须落在读入的缓冲内
%IFDEF IPL_CD
    MOV BP, 1 << SECT_SHIFT
%ELSE
    MOV BP, [_Sector_Bytes]
%ENDIF

    ; 段序列：从头部之后开始，按段长前进，按标记找 Stub 段
    MOV BX, [SI + DESC_AREA_HEAD_BYTES]
.Walk:
    MOV AX, BX
    ADD AX, DESC_SEG_HEAD_BYTES
    CMP AX, BP
    JA _Err_Desc
    CMP DWORD [SI + BX], DESC_BOOT_MAGIC
    JE .Stub
    MOV AX, [SI + BX + DESC_SEG_LEN]
    CMP AX, DESC_SEG_HEAD_BYTES
    JB _Err_Desc                 ; 段长小于段头会原地打转
    ADD BX, AX
    JMP .Walk

    ; Stub 段的起止位置：512 字节基准，换算成本地 LBA 后由两者相减得出扇区数
.Stub:
    MOV AX, [SI + BX + DESC_SEG_LEN]
    CMP AX, DESC_SEG_HEAD_BYTES + 8
    JB _Err_Desc                 ; 段长容不下起止位置
    MOV EAX, [SI + BX + DESC_SEG_LBA]
    MOV EDX, [SI + BX + DESC_SEG_END_LBA]
    ; 位置字段（512 字节基准）换算成本地 LBA：光盘单位固定 2048，磁盘按运行期探测的移位量
%IFDEF IPL_CD
    SHR EAX, 2
    SHR EDX, 2
%ELSE
    MOV CL, [_Sect_Shift]
    SHR EAX, CL
    SHR EDX, CL
%ENDIF
    TEST EAX, EAX
    JZ _Err_Desc
    CMP EDX, EAX
    JB _Err_Desc
    CMP EAX, [_Desc_Lba]
    JBE _Err_Desc                ; 载荷不得落回描述符扇区之前
    SUB EDX, EAX
    INC EDX                      ; EDX = 扇区数
    ; 扇区数上限：窗口字节数按本地单位右移
%IFDEF IPL_CD
    MOV BP, (LOAD_CEIL - LOAD_TOP) >> SECT_SHIFT
%ELSE
    ADD CL, BASE_SHIFT           ; CL 仍是上面的换算量
    MOV BP, LOAD_CEIL - LOAD_TOP
    SHR BP, CL
%ENDIF
    CMP DX, BP
    JA _Err_Size

    ; 校验全部通过：读入 Stub，再按契约交权
    MOV CX, DX
    MOV BX, LOAD_TOP
    CALL _Read_Sectors
    JC _Fail_Load
    MOV DL, [_Saved_Dl]
    MOV DH, IPL_MEDIA
    ; ESI 给描述符区的起始扇区：Stub 按它读同一区并自行遍历段
    MOV ESI, [_Desc_Lba]
%IFDEF IPL_CD
    MOV CX, 1 << SECT_SHIFT
%ELSE
    MOV CX, [_Sector_Bytes]
%ENDIF
    CLD
    STI
    JMP 0x0000:LOAD_TOP
