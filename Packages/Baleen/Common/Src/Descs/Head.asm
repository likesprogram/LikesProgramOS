; Head.asm
;    描述符区头部：格式标记、版本、头部长度与本区的起止位置
;
;    头部不含段表：段序列紧跟头部，读取方按各段头里的长度字段自行遍历
;    起止位置由写入程序按 512 字节基准回填（值 = 字节偏移 / 512），这里留 0

%INCLUDE "Desc.inc"

    DD DESC_AREA_MAGIC
    DW DESC_VERSION
    DW DESC_AREA_HEAD_BYTES
    DD 0                        ; 描述符区起始位置，512 字节基准，写入程序回填
    DD 0                        ; 描述符区结束位置，512 字节基准，含，写入程序回填
%IF ($ - $$) != DESC_AREA_HEAD_BYTES
    %ERROR "头部长度与 DESC_AREA_HEAD_BYTES 不一致"
%ENDIF
