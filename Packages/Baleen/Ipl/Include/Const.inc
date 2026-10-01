; Const.inc
;    IPL 运行期常量：描述符位置、读盘上限与调试口开关

; 装载窗口、实模式栈顶与介质编号是交权契约的一部分，由 Baleen 共享定义提供
%INCLUDE "Contract.inc"

; 描述符区位置槽：IPL 二进制里留给装载器回填的位置
; 槽里是描述符区的 512 字节基准地址（值 = 字节偏移 / 512），与目标设备的逻辑扇区单位无关；
; 运行期按探测到的本地单位右移换算成本地 LBA，所以同一份字节在 512e、4Kn 与 2048 字节单位的光盘上
; 都指向同一处物理位置，写入方不需要按目标设备回填
; 槽的位置在代码之后、分区表或签名之前，由下面的常量固定
%IFDEF IPL_CD
DESC_SLOT_OFF    EQU 494         ; 光盘：代码之后、510 字节代码区之内（其后留 16 字节余量）
%ELSE
DESC_SLOT_OFF    EQU 440         ; 磁盘：代码之后、446 字节分区表之前
%ENDIF
DESC_LBA_512     EQU 272         ; 描述符区在字节偏移 0x22000：0x22000 / 512 = 272

; 位置字段的基准单位是 512 字节：本地 LBA = 字段值 >> _Sect_Shift，字段移位量见 Read.inc
BASE_SHIFT       EQU 9

; —— 描述符区：头部与段的字段偏移，与 Packages/Baleen/Common/Include/Desc.inc 一致 ——
DESC_AREA_MAGIC      EQU 0x43534442  ; 'BDSC'，头部标记
DESC_AREA_HEAD_BYTES EQU 6           ; 头部长度字段，2 字节小端
DESC_AREA_LBA        EQU 8           ; 本区起始扇区，4 字节小端
DESC_AREA_END_LBA    EQU 12          ; 本区结束扇区，4 字节小端，含
DESC_AREA_HEAD_LEN   EQU 16          ; 头部固定长度，段序列从它之后开始
DESC_SEG_LEN         EQU 6           ; 段内段长，2 字节小端，含段头
DESC_SEG_HEAD_BYTES  EQU 8           ; 段头长度
DESC_SEG_LBA         EQU 8           ; 段内载荷起始扇区，4 字节小端
DESC_SEG_END_LBA     EQU 12          ; 段内载荷结束扇区，4 字节小端，含
DESC_BOOT_MAGIC      EQU 0x52445342  ; 'BSDR'，Stub 段标记
; 单幅读取的字节上限：窗口按本地扇区向下取整到整扇区（512 字节扇区下 0x8200，光盘 0x8000）
MAX_STUB_BYTES   EQU ((LOAD_CEIL - LOAD_TOP) >> SECT_SHIFT) << SECT_SHIFT

; EDD 一次传输最多 127 块；块数按运行期扇区大小换算，512 字节扇区下最多 65 块
%IF (MAX_STUB_BYTES >> 9) > 127
%ERROR "IPL 单批读取超过 EDD 的 127 块上限"
%ENDIF

; 0xE9 调试口开关，默认开；构建期用 -DENABLE_E9=0 关闭
%IFNDEF ENABLE_E9
%DEFINE ENABLE_E9 1
%ENDIF
