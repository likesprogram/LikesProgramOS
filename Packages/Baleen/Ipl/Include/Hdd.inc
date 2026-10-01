; Hdd.inc
;    硬盘与 USB-HDD 入口：查询逻辑扇区大小，读取描述符并进入共同装载路径

%INCLUDE "Init.inc"

    CMP DL, 0x80
    JB _Err_Desc
    CALL _Disk_Reset
    ; AH=48 的最小参数表借用尚未装入 Stub 的缓冲；查询失败保留 512 字节兼容值与不移位
    ; 只声明表长，不预清大小字段：返回短表会被长度检查挡下，不会读到未写入的值
    MOV SI, LOAD_TOP - BIOS_LOAD
    MOV WORD [SI], 0x1A
    CALL Read_Params
    JC .SectorReady
    CMP WORD [SI], 0x1A
    JB _Err_Desc
    MOV AX, [SI + 0x18]
    CMP AX, 512
    JE .SaveSector
    CMP AX, 4096
    JNE _Err_Desc
    MOV BYTE [_Sect_Shift], 3   ; 4096 字节扇区：512 字节基准右移三位
.SaveSector:
    MOV [_Sector_Bytes], AX
.SectorReady:
    ; 描述符区位置取自 IPL 里的槽：槽里是 512 字节基准地址，按上面的移位换算成本地 LBA
    MOV EAX, [DESC_SLOT_OFF]
    MOV CL, [_Sect_Shift]
    SHR EAX, CL
.ReadDescriptor:
    MOV [_Desc_Lba], EAX        ; 交权时经 ESI 交给 Stub，用于读同一个描述符区
    MOV CX, 1
    MOV BX, LOAD_TOP
    CALL _Read_Sectors
    JC _Err_Read
    MOV SI, LOAD_TOP - BIOS_LOAD   ; 描述符独占整扇区，缓冲内偏移为 0
    CMP DWORD [SI], DESC_AREA_MAGIC
    JNE _Err_Desc

%INCLUDE "Body.inc"
%INCLUDE "Error.inc"
%INCLUDE "Read.inc"
