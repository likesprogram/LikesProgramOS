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

; IPL → Stub：CX 携带本次读取使用的本地逻辑扇区字节数
; HDD / USB-HDD 为 512 或 4096，CD 为 2048；双方必须配套更新

; —— Stub → Core：介质上的 CoreDescriptor 与 Core 装载 ——
; CoreDescriptor 是描述符区里的 Core 段，其布局的单点定义在 Packages/Baleen/Common/Include/Desc.inc
; 起止位置由组装器或安装器按 512 字节基准写入（值 = 字节偏移 / 512），读取方按运行期单位换算，不必按目标设备回填

CORE_LOAD        EQU 0x100000    ; Core 映像装入的物理地址，即镜像基址；交权入口 = 它 + 头里的 EntryOffset
CORE_MAX_BYTES   EQU 0x400000    ; Core 文件字节数上限
CORE_HANDOFF_MAGIC EQU 0x484E4C42 ; 'BLNH'，Stub 交给 Core 的交权块格式标记
CORE_HANDOFF_VER EQU 1           ; 交权块的开发格式标记
CORE_HANDOFF_BYTES EQU 64        ; 交权块长度，Core 侧据此核对
