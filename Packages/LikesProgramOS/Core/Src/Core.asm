; Core.asm
;    内核侧 Core 的入口与内核镜像数据：文件起点是 128 字节数据块，0x80 起是长模式入口；
;    引导器把映像装到 KERNEL_LOAD，按数据里的 EntryOffset 跳进 0x80，RDI 指向 BootInfo，
;    这里换到自己的栈、把 BootInfo 交给 _Kernel_Main，不返回
;
;    数据块里的 ImageBytes 与 MemoryBytes 由链接器按镜像、未初始化区末尾填入，
;    BuildId 与 Digest 由 Tools/Bin/PackImage 填充；数据不参与执行，字段布局与
;    Packages/Common/Include/KernelImage.hpp 一致，偏移断言把它们盯在约定位置上

BITS 64
%INCLUDE "Const.inc"

; 自身内核镜像数据：排在文件最前，字段偏移由紧随其后的断言核对
SECTION .kernel.data progbits alloc nowrite

    DB "LPOSKRN", 0                     ; Magic：格式标记，含终止零
    DW KERNEL_DATA_VERSION              ; Version：未发布的开发格式标记
    DW KERNEL_DATA_BYTES                ; DataBytes：数据长度
    DD 0                                ; Flags：当前不定义可选标志
    DD __image_end - KERNEL_LOAD        ; ImageBytes：文件字节数，按镜像末尾由链接器填入
    DD __bss_end - KERNEL_LOAD          ; MemoryBytes：静态内存跨度，按未初始化区末尾填入
    DD KERNEL_ENTRY_OFFSET              ; EntryOffset：入口的文件偏移
    DD KERNEL_LOAD                      ; LoadAddress：装载物理地址，与链接脚本一致
    DW KERNEL_DIGEST_SHA256             ; DigestAlgorithm：1 为 SHA-256
    DW KERNEL_BUILDID_BYTES             ; BuildIdBytes：BuildId 长度
    DD KERNEL_BOOTINFO_VER              ; BootInfoVersion：内核要求的 BootInfo 版本
%IF ($ - $$) != 0x28
    %ERROR "BuildId 未落在数据内约定的偏移"
%ENDIF
    TIMES KERNEL_BUILDID_BYTES DB 0     ; BuildId：由打包器填充
%IF ($ - $$) != 0x48
    %ERROR "Digest 未落在数据内约定的偏移"
%ENDIF
    TIMES 32 DB 0                       ; Digest：由打包器按摘要覆盖区间填充
%IF ($ - $$) != 0x68
    %ERROR "保留区未落在数据内约定的偏移"
%ENDIF
    TIMES 24 DB 0                       ; Reserved：全 0
%IF ($ - $$) != KERNEL_DATA_BYTES
    %ERROR "内核镜像数据长度不是约定的 0x80"
%ENDIF

; 入口节：数据之后的第一条指令，必须落在 KERNEL_ENTRY_OFFSET，否则引导器跳错位置
SECTION .text.start progbits alloc exec nowrite

GLOBAL _Kernel_Start
GLOBAL _Kernel_Exception_Stubs
GLOBAL _Kernel_Stack_Top
extern _Kernel_Main
extern _Kernel_Exception_Handler
; 链接脚本给出的镜像与未初始化区边界，供数据字段使用
extern __image_end
extern __bss_end

; 引导器按长模式、平坦段、分页已开启、IF=0、DF=0 跳到 KERNEL_LOAD + EntryOffset，RDI 指向 BootInfo
; 进入状态由交接契约保证，这里只补齐方向标志并换栈：引导映射与栈都按恒等映射有效
_Kernel_Start:
    CLI
    CLD
    MOV RSP, _Kernel_Stack_Top      ; 自己的栈在未初始化区，由引导器按 MemoryBytes 一并清零
    CALL _Kernel_Main               ; RDI 已是 BootInfo 物理地址，_Kernel_Main 的唯一参数
.Hang:
    HLT
    JMP .Hang

; 运行期异常入口桩：每个向量一个桩，把错误码与向量号整理成统一栈帧后进公共入口
; 处理器压入错误码的向量（8、10 至 14、17、21、29、30）自带错误码，其余补一个零，
; 因此公共入口看到的帧自低到高恒为：向量号、错误码、返回偏移、CS、RFLAGS
SECTION .text.exceptions progbits alloc exec nowrite
%ASSIGN VECTOR 0
%REP 32
Exc_%+VECTOR:
%IF VECTOR = 8 || (VECTOR >= 10 && VECTOR <= 14) || VECTOR = 17 || VECTOR = 21 || VECTOR = 29 || VECTOR = 30
    PUSH QWORD VECTOR               ; 处理器已压错误码，这里只补向量号
%ELSE
    PUSH QWORD 0                    ; 不压错误码的向量：补零保持帧一致
    PUSH QWORD VECTOR
%ENDIF
    JMP Exc_Common
%ASSIGN VECTOR VECTOR+1
%ENDREP

; 公共入口：帧不返回，栈指针对齐后直接把帧指针交给 C++ 侧
Exc_Common:
    MOV RDI, RSP                    ; 帧指针指向最低的向量号
    AND RSP, ~0xF                   ; 调用前对齐；对齐只向下移动栈指针，不影响已压入的帧
    CALL _Kernel_Exception_Handler
.Hang:
    HLT
    JMP .Hang

; 桩地址表：C++ 侧按向量号索引，省得在 C++ 里逐个声明入口
_Kernel_Exception_Stubs:
%ASSIGN VECTOR 0
%REP 32
    DQ Exc_%+VECTOR
%ASSIGN VECTOR VECTOR+1
%ENDREP

; 运行期栈：放在未初始化区，不落盘也不进镜像；由引导器按数据里的 MemoryBytes 清零
SECTION .bss
ALIGN 16
_Kernel_Stack_Bottom: RESB KERNEL_STACK_BYTES
_Kernel_Stack_Top:
