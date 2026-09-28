; Cpu.asm
;    Platform::Cpu 的纯汇编原语：端口读写、绝对地址读写、标志与寄存器读取、CPUID、GDTR 读取
; 与 Cpu.cpp 同样只按 32 位保护模式编译；出入口约定是 cdecl：参数在栈上、调用方清栈，
; 被调用方不改动 EBX、ESI、EDI、EBP
BITS 32

SECTION .text

GLOBAL _Cpu_Out8
GLOBAL _Cpu_In8
GLOBAL _Cpu_Read32
GLOBAL _Cpu_Write32
GLOBAL _Cpu_ReadFlags
GLOBAL _Cpu_WriteFlags
GLOBAL _Cpu_ReadCr0
GLOBAL _Cpu_ReadCs
GLOBAL _Cpu_ReadMsr
GLOBAL _Cpu_Cpuid
GLOBAL _Cpu_ReadGdtr

; 写 8 位 I/O 端口：Out8(port, value)
_Cpu_Out8:
    MOV DX, [ESP + 4]       ; 端口
    MOV AL, [ESP + 8]       ; 数据
    OUT DX, AL
    RET

; 读 8 位 I/O 端口：In8(port)
_Cpu_In8:
    MOV DX, [ESP + 4]       ; 端口
    IN AL, DX
    MOVZX EAX, AL
    RET

; 读绝对地址处的 32 位值：Read32(address)
_Cpu_Read32:
    MOV EDX, [ESP + 4]      ; 地址
    MOV EAX, [EDX]
    RET

; 写绝对地址处的 32 位值：Write32(address, value)
_Cpu_Write32:
    MOV EDX, [ESP + 4]      ; 地址
    MOV EAX, [ESP + 8]      ; 数据
    MOV [EDX], EAX
    RET

; 读标志寄存器低 32 位：ReadFlags()
_Cpu_ReadFlags:
    PUSHFD
    POP EAX
    RET

; 写标志寄存器低 32 位：WriteFlags(flags)
_Cpu_WriteFlags:
    MOV EAX, [ESP + 4]      ; 标志值
    PUSH EAX
    POPFD
    RET

; 读 CR0 低 16 位：ReadCr0()；SMSW 的 16 位形式在实模式、保护模式与长模式下都合法
_Cpu_ReadCr0:
    SMSW AX
    MOVZX EAX, AX
    RET

; 读当前 CS 选择子：ReadCs()
_Cpu_ReadCs:
    XOR EAX, EAX
    MOV AX, CS
    RET

; 读 MSR 低 32 位：ReadMsr(index)；调用前须确认 CPU 支持该 MSR
_Cpu_ReadMsr:
    MOV ECX, [ESP + 4]      ; MSR 号
    RDMSR
    RET

; 读 GDTR：ReadGdtr(snapshot)；snapshot 指向 6 字节快照，前两字节为限长，其后 4 字节为线性基址
_Cpu_ReadGdtr:
    MOV EAX, [ESP + 4]      ; 快照地址
    SGDT [EAX]
    RET

; 执行 CPUID：Cpuid(leaf, eaxOut, ebxOut, ecxOut, edxOut)；调用前须确认 CPU 支持 CPUID
; 压入两个寄存器后入参整体下移 8 字节；ECX 的结果先写出，随后 ECX 用来取其余出参地址
_Cpu_Cpuid:
    PUSH EBX
    PUSH ESI
    MOV EAX, [ESP + 12]     ; leaf
    XOR ECX, ECX
    CPUID
    MOV ESI, [ESP + 24]     ; ecxOut
    MOV [ESI], ECX
    MOV ESI, [ESP + 16]     ; eaxOut
    MOV [ESI], EAX
    MOV ESI, [ESP + 20]     ; ebxOut
    MOV [ESI], EBX
    MOV ESI, [ESP + 28]     ; edxOut
    MOV [ESI], EDX
    POP ESI
    POP EBX
    RET
