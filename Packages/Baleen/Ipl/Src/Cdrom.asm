; Cdrom.asm
;    El Torito 无仿真光盘入口：产物恰好 2048 字节，运行必需内容全部落在前 510 字节
;
;    目录约定 LoadSize = 4（以 512 字节计），本地扇区 2048 字节。读盘必须整段落在
;    前 512 字节，有的 BIOS 只装入 1 个 512 字节扇区；偏移 8 起 56 字节按 El Torito
;    的 Boot Info Table 位置保留，入口跳过该区；本项目不使用该表：IPL 不读它，
;    混合镜像写入 U 盘后其中的 ISO LBA 也不再有效，该区保持全 0 并由 CheckIpl 核对

BITS 16                         ; 十六位模式
ORG 0

%DEFINE SECT_SHIFT 11           ; 2048 字节扇区
%DEFINE IPL_MEDIA MEDIA_CDROM   ; DH = Cdrom
%DEFINE IPL_CD                  ; 光盘仅通过 EDD 读取

    JMP SHORT _Start            ; 跳过 Boot Info Table 保留区
    TIMES 8 - ($-$$) DB 0       ; 填到偏移 8
    TIMES 56 DB 0               ; El Torito 的 Boot Info Table 位置，本项目保持全 0（理由见文件头）

%INCLUDE "Cd.inc"

; 代码区末端：CheckIpl 从汇编清单读这个地址，核对代码不越过描述符区槽与 510 字节代码区
Code_End:
%IF ($-$$) > DESC_SLOT_OFF
%ERROR "光盘 IPL 代码越过描述符区槽"
%ENDIF
%IF ($-$$) > 510
%ERROR "光盘 IPL 超过 510 字节"
%ENDIF
    ; 描述符区位置槽：512 字节基准地址，运行期按设备单位换算；装载器可覆盖，未覆盖时用本值
    TIMES DESC_SLOT_OFF - ($-$$) DB 0
    DD DESC_LBA_512
    TIMES 510 - ($-$$) DB 0
    DW 0xAA55                   ; 部分 BIOS 仍检查签名
    TIMES 2048 - ($-$$) DB 0    ; 凑满 1 个 CD 扇区，给 LoadSize 4
