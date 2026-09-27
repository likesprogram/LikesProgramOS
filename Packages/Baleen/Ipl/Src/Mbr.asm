; 硬盘与 U 盘（USB-HDD）共用。BIOS 将 LBA 0 加载到 0x7C00，文件必须恰好 512 字节

BITS 16                         ; 十六位模式
ORG 0

%DEFINE SECT_SHIFT 9            ; 512 字节扇区
%DEFINE IPL_MEDIA MEDIA_HDD     ; DH 交给 Stub

%INCLUDE "../Include/Body.inc"

%IF ($ - $$) > 446
%ERROR "MBR IPL 代码超过 446 字节"
%ENDIF
    TIMES 446 - ($ - $$) DB 0   ; 填充到分区表；前 446 字节为引导代码

    ; 分区项 0：ESP（类型 0xEF），LBA 2048，放 EFI 启动 FAT；大小由 MKDISK 按 Efi.img 写实
    DB 0x00, 0x00, 0x02, 0x00   ; 非活动 + 起始 CHS（占位）
    DB 0xEF, 0xFE, 0xFF, 0xFF   ; 类型（EFI System Partition）+ 结束 CHS
    DD 2048                     ; 起始 LBA
    DD 0                        ; 扇区数（MKDISK 写入）

    ; 分区项 1..3 由构建期写盘工具填写（当前 ESP 与 Ext4 系统卷）；先置空
    TIMES 16 * 3 DB 0           ; 其余三个分区项空
    DW 0xAA55                   ; MBR 签名