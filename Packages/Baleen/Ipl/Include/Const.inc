; Const.inc
;    IPL 运行期常量：描述符位置、读盘上限、失败诊断码与调试口开关

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
; 段遍历的迭代上限：段头 8 字节、偏移每轮至少前进一个段头，最长缓冲 4096 字节，
; 因此能容纳的段数不超过 4096 / DESC_SEG_HEAD_BYTES；显式上限让畸形段序列也一定停下
DESC_SEG_WALK_MAX    EQU (4096 / DESC_SEG_HEAD_BYTES)
DESC_SEG_LBA         EQU 8           ; 段内载荷起始扇区，4 字节小端
DESC_SEG_END_LBA     EQU 12          ; 段内载荷结束扇区，4 字节小端，含
DESC_BOOT_MAGIC      EQU 0x52445342  ; 'BSDR'，Stub 段标记
; 单幅读取的字节上限：窗口按本地扇区向下取整到整扇区（512 字节扇区下 0x8200，光盘 0x8000）
MAX_STUB_BYTES   EQU ((LOAD_CEIL - LOAD_TOP) >> SECT_SHIFT) << SECT_SHIFT

; EDD 一次传输最多 127 块；块数按运行期扇区大小换算，512 字节扇区下最多 65 块
%IF (MAX_STUB_BYTES >> 9) > 127
%ERROR "IPL 单批读取超过 EDD 的 127 块上限"
%ENDIF

; —— 失败诊断码：大于零的整数，输出时转成一位十六进制字符 ——
; 码值与登记编号同号，按检查顺序连续编号
ERR_DESC_READ    EQU 1           ; 描述符所在扇区读取失败（仅硬盘入口）
ERR_MEDIA        EQU 2           ; 驱动器号或 AH=48 查询结果不受支持
ERR_DESC_HEAD    EQU 3           ; 描述符头部或段序列非法
ERR_STUB_RANGE   EQU 4           ; Stub 段起止位置非法
ERR_CD_PROBE     EQU 5           ; 光盘候选驱动器探测未取得合法描述符
ERR_STUB_SIZE    EQU 6           ; Stub 扇区数超过装载窗口上限
ERR_STUB_READ    EQU 7           ; 描述符通过后 Stub 读盘最终失败

; 0xE9 调试口开关，默认开；构建期用 -DENABLE_E9=0 关闭
%IFNDEF ENABLE_E9
%DEFINE ENABLE_E9 1
%ENDIF
