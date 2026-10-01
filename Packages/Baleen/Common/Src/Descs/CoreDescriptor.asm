; CoreDescriptor.asm
;    段：BaleenCore 载荷的起止位置
;
;    段长只在本文件里声明：读取方按它跳到下一个段，段的顺序不作要求
;    起止位置由写入程序按 512 字节基准回填（值 = 字节偏移 / 512），这里留 0
;    结束位置含在范围内，用于装载方的范围判定：读盘请求必须落在声明区间里

%INCLUDE "Desc.inc"

; 本段长度：段头 8 + 起止位置 8
DESC_CORE_BYTES EQU DESC_SEG_HEAD_BYTES + 8

    DD DESC_CORE_MAGIC
    DW DESC_VERSION
    DW DESC_CORE_BYTES
    DD 0                        ; Core 起始位置，512 字节基准，写入程序回填
    DD 0                        ; Core 结束位置，512 字节基准，含，写入程序回填
%IF ($ - $$) != DESC_CORE_BYTES
    %ERROR "段长与 DESC_CORE_BYTES 不一致"
%ENDIF
