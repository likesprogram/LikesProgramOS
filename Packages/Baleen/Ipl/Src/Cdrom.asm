; El Torito 无仿真 IPL，本地扇区 2048 B
; 做成 2048 B，对应 El Torito SectorCount = 4（以 512 字节计）
; 读盘必须整段落在前 512 字节：有的 BIOS 只装入 1 个 512 扇区
; 偏移 8 起 56 字节留给 XORRISO -BOOT-INFO-TABLE（固件/工具写入 ISO LBA）

BITS 16                         ; 十六位模式
ORG 0

%DEFINE SECT_SHIFT 11           ; 2048 字节扇区
%DEFINE IPL_MEDIA MEDIA_CDROM   ; DH = Cdrom
%DEFINE IPL_CD                  ; body.inc 走 CD 专用路径选设备与描述符扇区

    jmp SHORT _Start             ; 跳过 BOOT-INFO-TABLE 空洞
    TIMES 8 - ($-$$) DB 0       ; 填到偏移 8
    TIMES 56 DB 0               ; XORRISO -BOOT-INFO-TABLE 写入区（内容由工具填，IPL 不读）

%INCLUDE "../Include/Body.inc"

%IF ($-$$) > 510
%ERROR "光盘 IPL 超过 510 字节"
%ENDIF
    TIMES 510 - ($-$$) DB 0
    DW 0xAA55                   ; 部分 BIOS 仍检查签名
    TIMES 2048 - ($-$$) DB 0    ; 凑满 1 个 CD 扇区，给 LoadSize 4
