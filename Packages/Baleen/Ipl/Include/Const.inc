; Const.inc
;    IPL 运行期常量：描述符字段、读盘上限与调试口开关

; 装载窗口、实模式栈顶与介质编号是交权契约的一部分，由 Baleen 共享定义提供
%INCLUDE "Contract.inc"

DESC_IMAGE       EQU 0x300       ; 介质绝对字节偏移，不是内存地址
DESC_MAGIC       EQU 0x52445342  ; 描述符魔数 'BSDR'
DESC_VERSION     EQU 1           ; 当前开发格式标记
DESC_HEADER      EQU 32          ; 描述符总长
DESC_VER         EQU 4           ; Version 与 HeaderBytes 字段偏移（同一个 DWORD）
DESC_LOFF        EQU 8           ; StubFileOffset 字段偏移
DESC_LBYTES      EQU 16          ; StubImageBytes 字段偏移
DESC_RESV        EQU 24          ; Reserved 字段偏移
DESC_SEC         EQU DESC_IMAGE >> SECT_SHIFT                ; 描述符所在介质扇区
DESC_OFF         EQU DESC_IMAGE & ((1 << SECT_SHIFT) - 1)    ; 描述符在扇区内的偏移
MAX_STUB_SECTORS EQU (LOAD_CEIL - LOAD_TOP) >> SECT_SHIFT    ; 单批读取的扇区上限
MAX_STUB_BYTES   EQU MAX_STUB_SECTORS << SECT_SHIFT          ; 对应的文件字节上限

; 0xE9 调试口开关，默认开；构建期用 -DENABLE_E9=0 关闭
%IFNDEF ENABLE_E9
%DEFINE ENABLE_E9 1
%ENDIF

; IPL 只读描述符或整个 Stub，不需要通用的 127 块分批循环
%IF MAX_STUB_SECTORS > 127
%ERROR "IPL 单批读取超过 EDD 的 127 块上限"
%ENDIF
