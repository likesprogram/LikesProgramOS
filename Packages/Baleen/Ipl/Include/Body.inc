; Body.inc
;    两种介质入口共用的描述符区解析、Stub 装载与交权
;
;    进入前提：描述符区首扇区已读入缓冲，头部标记已由各入口核过，DS:SI 指向缓冲起点
;    头部给出本区起止位置；其后是段序列：每段以段头开头（标记、版本、段长），
;    读取方按段长跳到下一个、按标记认段；顺序不作要求，加段不必改本文件
;    头部与段内的位置字段都是 512 字节基准（值 = 字节偏移 / 512），与目标设备的逻辑扇区
;    单位无关；本文件按本地单位右移换算成本地 LBA 后再读盘
;    段遍历的偏移与步进用 32 位，并逐轮核对段头与匹配段的字段都落在本扇区缓冲内、
;    迭代次数不超过缓冲能容纳的段数：畸形段长不会把遍历拖成回绕或死循环

    ; 头部本区起始位置必须与槽一致：两者都是 512 字节基准，不必换算
    MOV EAX, [SI + DESC_AREA_LBA]
    CMP EAX, [DESC_SLOT_OFF]
    JNE _Err_Head                ; 与装载方据以定位的槽值不符

    ; 本扇区字节数：段头与整段字段都必须落在读入的缓冲内
    ; 上界预留「段头 + 起止位置」共 16 字节：匹配到的段要读这两枚字段，其它段的步进越界在下一轮拦下
%IFDEF IPL_CD
    MOV EBP, (1 << SECT_SHIFT) - (DESC_SEG_HEAD_BYTES + 8)
%ELSE
    MOVZX EBP, WORD [_Sector_Bytes]
    SUB EBP, DESC_SEG_HEAD_BYTES + 8
%ENDIF

    ; 段序列：从头部之后开始，按段长前进，按标记找 Stub 段
    ; 偏移与步进都用 32 位：头长与段长是 16 位字段，16 位累加会回绕、原地打转
    ; 每轮先核对段头落在缓冲内；迭代次数另有上限，段序列没有终点时也一定停下
    MOVZX EBX, WORD [SI + DESC_AREA_HEAD_BYTES]
    MOV CX, DESC_SEG_WALK_MAX
.Walk:
    CMP EBX, EBP
    JA _Err_Head                 ; 段头或匹配段的起止位置越过本扇区缓冲
    MOVZX EAX, WORD [SI + BX + DESC_SEG_LEN]
    CMP AX, DESC_SEG_HEAD_BYTES
    JB _Err_Head                 ; 段长小于段头会原地打转
    CMP DWORD [SI + BX], DESC_BOOT_MAGIC
    JE .Stub
    ADD EBX, EAX                 ; 32 位步进：段长累加不回绕
    LOOP .Walk
    JMP _Err_Head                ; 迭代上限用尽：段序列没有终点

    ; Stub 段的起止位置：512 字节基准，换算成本地 LBA 后由两者相减得出扇区数
    ; 走到这里时 AX 仍是本段的段长
.Stub:
    CMP AX, DESC_SEG_HEAD_BYTES + 8
    JB _Err_Head                 ; 段长容不下起止位置
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
    ; 起点必须落在描述符扇区之后；起点为零也由这一条拦下
    CMP EAX, [_Desc_Lba]
    JBE _Err_Range               ; 载荷不得落回描述符扇区之前
    ; 结束位置加一：起点加扇区数恰好到 0x100000000 时在这里进位，没有读盘空间，拒绝
    INC EDX
    JZ _Err_Range                ; 结束位置加一回到 0 即 32 位进位
    ; 再减起点得到扇区数：借位表示结束位置早于起始位置，为零表示结束位置就是起始位置的前一个
    SUB EDX, EAX
    JBE _Err_Range
    ; 扇区数上限：窗口按 512 字节基准是 65 个扇区，再按运行期移位量换算成本地扇区数
    ; 上限与扇区数必须按 32 位比较，只看低 16 位会放行扇区数 0x10000 的畸形描述符
    ; （DX 为 0），随后以 0 块请求继续装载
%IFDEF IPL_CD
    CMP EDX, (LOAD_CEIL - LOAD_TOP) >> SECT_SHIFT
%ELSE
    MOV EBP, (LOAD_CEIL - LOAD_TOP) >> BASE_SHIFT
    SHR EBP, CL                  ; CL 仍是上面的换算量
    CMP EDX, EBP
%ENDIF
    JA _Err_Size

    ; 校验全部通过：读入 Stub，再按契约交权
    ; 上限已按 32 位判定，扇区数不超过本介质窗口（512e 65、4Kn 8、CD 16），截到 16 位不丢高位
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
