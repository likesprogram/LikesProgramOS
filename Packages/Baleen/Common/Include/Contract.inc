; Contract.inc
;    Baleen 各级引导的交权契约常量：装载窗口、实模式栈顶、介质编号与 Stub → Core 交权
;
;    这些取值同时约束交权双方（IPL 与 Stub、以及后续的 Core），改动必须两边一致
;    取值依据见 Docs/Specs/Baleen/一级引导契约.md 与 Packages/Baleen/Stub/README.md

LOAD_TOP         EQU 0x7E00      ; Stub 装入点，即 [0x7E00,0x10000) 窗口下端
LOAD_CEIL        EQU 0x10000     ; Stub 窗口上端，不含
BIOS_LOAD        EQU 0x7C00      ; BIOS 装入 IPL 的物理地址，IPL 留在原地
STACK_TOP        EQU 0x7C00      ; 实模式栈顶，交权时的 SS:SP，从 IPL 之下向下增长

MEDIA_HDD        EQU 2           ; 交权时 DH 的介质编号：硬盘 / USB-HDD
MEDIA_CDROM      EQU 4           ; 交权时 DH 的介质编号：光盘

; —— Stub → Core：介质上的 CoreDescriptor 与 Core 装载 ——
; CoreDescriptor 与 BootDescriptor 同扇区，由组装器写入、由 Stub 读取，字节表见 Stub/README.md 第二节
; 校验规则与 BootDescriptor 一致：格式标记、头长、偏移对齐与取值范围、偏移加长度不进位
CORE_DESC_OFF    EQU 0x340       ; 描述符在介质上的绝对字节偏移
CORE_DESC_MAGIC  EQU 0x52444342  ; 'BCDR'，介质字节为 42 43 44 52
CORE_DESC_VER    EQU 1           ; 开发格式标记，未发布
CORE_DESC_HEADER EQU 32          ; 描述符固定总长
CORE_DESC_FILE   EQU 0x08        ; 描述符内文件偏移字段，8 字节小端
CORE_DESC_BYTES  EQU 0x10        ; 描述符内文件字节数字段，8 字节小端
CORE_DESC_RESV   EQU 0x18        ; 描述符内保留字段，8 字节全 0

CORE_LOAD        EQU 0x100000    ; Core 装入的物理地址，入口在文件偏移 0，装入地址即交权入口
CORE_MAX_BYTES   EQU 0x400000    ; Core 文件字节数上限
CORE_HANDOFF_MAGIC EQU 0x484E4C42 ; 'BLNH'，Stub 交给 Core 的交权块格式标记
CORE_HANDOFF_VER EQU 1           ; 交权块的开发格式标记
CORE_HANDOFF_BYTES EQU 64        ; 交权块长度，Core 侧据此核对
