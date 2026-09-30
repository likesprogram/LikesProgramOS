; Hdd.inc
;    硬盘与 USB-HDD 入口：查询逻辑扇区大小，读取描述符并进入共同装载路径

%INCLUDE "Init.inc"

    CMP DL, 0x80
    JB _Err_Desc
    CALL _Disk_Reset
    ; AH=48 的最小参数表借用尚未装入 Stub 的缓冲；查询失败保留 512 字节兼容值
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
.SaveSector:
    MOV [_Sector_Bytes], AX
.SectorReady:
    ; 描述符在固定字节偏移 DESC_OFFSET：512 字节扇区下是 LBA 272，4096 字节扇区下是 LBA 34
    ; 探测失败按 512 兼容，因此默认取 512 的 LBA
    MOV EAX, DESC_LBA_512
    CMP WORD [_Sector_Bytes], 512
    JE .ReadDescriptor
    MOV EAX, DESC_LBA_4096
.ReadDescriptor:
    MOV CX, 1
    MOV BX, LOAD_TOP
    CALL _Read_Sectors
    JC _Err_Read
    MOV SI, LOAD_TOP - BIOS_LOAD   ; DESC_OFFSET 是两种单位下的扇区边界，缓冲内偏移为 0
    CMP DWORD [SI], DESC_MAGIC
    JNE _Err_Desc

%INCLUDE "Body.inc"
%INCLUDE "Error.inc"
%INCLUDE "Read.inc"
