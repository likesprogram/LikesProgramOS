; Cdrom.asm
; El Torito 无仿真光盘入口：产物恰好 2048 字节，运行必需内容全部落在前 510 字节
;
; 目录约定 LoadSize = 4（以 512 字节计），本地扇区 2048 字节。读盘必须整段落在
; 前 512 字节，有的 BIOS 只装入 1 个 512 字节扇区；偏移 8 起 56 字节留给
; XORRISO -BOOT-INFO-TABLE，由固件/工具写入 ISO LBA，IPL 不读它的内容

BITS 16                         ; 十六位模式
ORG 0

%DEFINE SECT_SHIFT 11           ; 2048 字节扇区
%DEFINE IPL_MEDIA MEDIA_CDROM   ; DH = Cdrom
%DEFINE IPL_CD                  ; 光盘仅通过 EDD 读取

    JMP SHORT _Start            ; 跳过 BOOT-INFO-TABLE 空洞
    TIMES 8 - ($-$$) DB 0       ; 填到偏移 8
    TIMES 56 DB 0               ; XORRISO -BOOT-INFO-TABLE 写入区（内容由工具填，IPL 不读）

%INCLUDE "Cd.inc"

; 代码区末端：CheckIpl 从汇编清单读这个地址，核对 510 字节与 16 字节余量
Code_End:
%IF ($-$$) > 494
%ERROR "光盘 IPL 须为 510 字节代码区保留至少 16 字节余量"
%ENDIF
%IF ($-$$) > 510
%ERROR "光盘 IPL 超过 510 字节"
%ENDIF
    TIMES 510 - ($-$$) DB 0
    DW 0xAA55                   ; 部分 BIOS 仍检查签名
    TIMES 2048 - ($-$$) DB 0    ; 凑满 1 个 CD 扇区，给 LoadSize 4
