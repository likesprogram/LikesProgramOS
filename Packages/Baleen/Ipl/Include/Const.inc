; Const.inc
;    IPL 运行期常量：描述符位置、读盘上限与调试口开关

; 装载窗口、实模式栈顶与介质编号是交权契约的一部分，由 Baleen 共享定义提供
%INCLUDE "Contract.inc"

; 描述符的介质绝对字节偏移：取 512 / 2048 / 4096 三种逻辑扇区的公倍数，
; 落在 GPT 保留区（LBA 1..33，4Kn 下到 0x22000 之前）之后，三种单位下都是扇区边界
; 同一份镜像在三种单位下的描述符字节位置一致，不随介质单位变化
DESC_OFFSET      EQU 0x22000     ; BootDescriptor 偏移；CoreDescriptor 紧随其后 32 字节
DESC_CORE_DELTA  EQU 32          ; CoreDescriptor 相对 BootDescriptor 的偏移
DESC_MAGIC       EQU 0x52445342  ; 描述符魔数 'BSDR'
DESC_VERSION     EQU 1           ; 当前开发格式标记
DESC_HEADER      EQU 32          ; 描述符总长
DESC_VER         EQU 4           ; Version 与 HeaderBytes 字段偏移（同一个 DWORD）
DESC_LOFF        EQU 8           ; StubFileOffset 字段偏移
DESC_LBYTES      EQU 16          ; StubImageBytes 字段偏移
DESC_RESV        EQU 24          ; Reserved 字段偏移
; 描述符所在逻辑扇区号，按本地扇区单位换算
DESC_LBA_512     EQU DESC_OFFSET >> 9    ; 512 字节扇区：272
DESC_LBA_2048    EQU DESC_OFFSET >> 11   ; 2048 字节扇区（光盘）：68
DESC_LBA_4096    EQU DESC_OFFSET >> 12   ; 4096 字节扇区：34
; Stub 文件偏移的下限：描述符之后，且 512 与 4096 两种单位下都安全
STUB_MIN_OFFSET  EQU 0x23000
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
