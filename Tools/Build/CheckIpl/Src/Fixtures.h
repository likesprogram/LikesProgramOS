// Fixtures.h
// CheckIpl 用的两段 NASM 夹具源码：交接校验载荷与 BIOS 故障注入引导器

#pragma once
#include <string_view>

namespace checkipl {
    // 校验载荷只核对 IPL 行为，不属于 BaleenStub 正式实现
    inline constexpr std::string_view kProbeAsm = R"ASM(
BITS 16
ORG 0x7E00
%ifndef PAYLOAD_BYTES
%define PAYLOAD_BYTES 4096
%endif
%ifndef EXPECT_MEDIA
%define EXPECT_MEDIA 2
%endif
%ifndef EXPECT_DRIVE
%define EXPECT_DRIVE 0x80
%endif
%ifndef EXPECT_RESET
%define EXPECT_RESET 0
%endif
%ifndef EXPECT_CHS
%define EXPECT_CHS 0
%endif

    PUSHF
    POP AX
    AND AX, 0x0600
    CMP AX, 0x0200
    JNE Fail
    MOV AX, CS
    OR AX, AX
    JNZ Fail
    MOV AX, DS
    CMP AX, 0x07C0
    JNE Fail
    MOV AX, ES
    OR AX, AX
    JNZ Fail
    MOV AX, SS
    OR AX, AX
    JNZ Fail
    CMP SP, 0x7C00
    JNE Fail
    CMP DX, (EXPECT_MEDIA << 8) | EXPECT_DRIVE
    JNE Fail
%if EXPECT_RESET
    CMP WORD [CS:0x500], EXPECT_RESET
    JB Fail
%endif
%if EXPECT_CHS
    CMP WORD [CS:0x502], EXPECT_CHS
    JB Fail
%endif
    MOV DI, Payload
    MOV CX, PAYLOAD_BYTES - (Payload - $$) - 4
    MOV AL, 0xA5
    REPE SCASB
    JNE Fail
    CMP DWORD [CS:0x7E00 + PAYLOAD_BYTES - 4], 0xA1B2C3D4
    JNE Fail
    MOV SI, Ok
    MOV BL, 0x10
    JMP Print
Fail:
    MOV SI, Bad
    MOV BL, 0x11
Print:
    MOV AL, [CS:SI]
    INC SI
    TEST AL, AL
    JZ Exit
    OUT 0xE9, AL
    JMP Print
Exit:
    MOV AL, BL
    OUT 0xF4, AL
    CLI
.Halt:
    HLT
    JMP .Halt
Ok: DB 'IPL-PROBE-OK', 0
Bad: DB 'IPL-PROBE-FAIL', 0
Payload:
    TIMES PAYLOAD_BYTES - ($-$$) - 4 DB 0xA5
    DD 0xA1B2C3D4
)ASM";

    // 引导夹具：先加载真实 IPL，再用 INT 13h 钩子注入有界 BIOS 故障
    // 0=破坏寄存器，1=无 EDD，2=批量失败并改写 DAP，3=前两次读取失败，
    // 4=永久失败，5=只有 CD 驱动器 E1 可用
    inline constexpr std::string_view kFaultAsm = R"ASM(
BITS 16
ORG 0x1000

    JMP 0:0x7C00 + (Entry - $$)
Entry:
    CLI
    XOR AX, AX
    MOV DS, AX
    MOV ES, AX
    MOV SS, AX
    MOV SP, 0x7C00
    CLD
    MOV SI, 0x7C00
    MOV DI, 0x1000
    MOV CX, 256
    REP MOVSW
    JMP 0:Relocated
Relocated:
    STI
    MOV SI, Extension
    MOV AH, 0x42
    INT 0x13
    JC Harness_Fail
    JMP Loaded
Harness_Fail:
    MOV AL, '!'
    OUT 0xE9, AL
    CLI
    HLT
    JMP $
    ALIGN 16
Extension:
    DB 16, 0
    DW 3, 0x1200, 0
    DQ 1
    TIMES 510 - ($-$$) DB 0
    DW 0xAA55
Loaded:
    MOV SI, Fixture
    MOV DL, 0x80
    MOV AH, 0x42
    INT 0x13
    JC Harness_Fail
    XOR AX, AX
    MOV ES, AX
    MOV DI, 0x500
    MOV CX, 8
    REP STOSW
    MOV WORD [ES:0x13*4], Hook
    MOV WORD [ES:0x13*4+2], 0
    MOV AX, 0x2000
    MOV DS, AX
    XOR SI, SI
    MOV DI, 0x7C00
    MOV CX, 1024
    REP MOVSW
    MOV DL, ENTRY_DRIVE
    JMP 0:0x7C00

    ALIGN 16
Fixture:
    DB 16, 0
    DW 80, 0, 0x2000
    DQ 4
Remaining: DB 2

Hook:
    PUSH BP
    MOV BP, SP
    PUSHAD
    PUSH DS
    PUSH ES
    CMP AH, 0
    JE .Reset
    CMP AH, 0x42
    JE .Edd
    CMP AH, 0x02
    JE .Chs
    JMP .Error
.Reset:
    INC WORD [CS:0x500]
    JMP .Ok
.Edd:
%if FAULT = 1 || FAULT = 4
    JMP .Error
%endif
%if FAULT = 3
    CMP BYTE [CS:Remaining], 0
    JE .Ready
    DEC BYTE [CS:Remaining]
    JMP .Error
.Ready:
%endif
%if FAULT = 5
    CMP DL, 0xE1
    JNE .Error
%endif
    CMP WORD [SI], 16
    JNE .Error
    MOV CX, [SI + 2]
%if FAULT = 2
    CMP CX, 1
    JE .Single
    MOV WORD [SI], 0
    MOV WORD [SI + 2], 0
    JMP .Error
.Single:
%endif
    CMP DWORD [SI + 12], 0
    JNE .Error
    MOV EAX, [SI + 8]
    LES DI, [SI + 4]
    JMP .Copy
.Chs:
%if FAULT = 4 || SECT_SHIFT != 9
    JMP .Error
%endif
    CMP AX, 0x0201
    JNE .Error
    CMP DH, 0
    JNE .Error
    TEST CX, 0xFFC0
    JNZ .Error
    MOVZX EAX, CL
    DEC EAX
    MOV CX, 1
    MOV DI, BX
    INC WORD [CS:0x502]
.Copy:
    TEST CX, CX
    JZ .Error
    CMP CX, 127
    JA .Error
    CMP EAX, 40960 >> SECT_SHIFT
    JAE .Error
    SHL EAX, SECT_SHIFT
    MOVZX EDX, CX
    SHL EDX, SECT_SHIFT
    ADD EDX, EAX
    CMP EDX, 40960
    JA .Error
    MOV SI, AX
    MOV AX, 0x2000
    MOV DS, AX
    SHL CX, SECT_SHIFT - 1
    CLD
    REP MOVSW
.Ok:
    AND WORD [SS:BP + 6], ~1
    MOV BYTE [CS:Status], 0
    JMP .Return
.Error:
    OR WORD [SS:BP + 6], 1
    MOV BYTE [CS:Status], 1
.Return:
    POP ES
    POP DS
    POPAD
    POP BP
    MOV EAX, 0xDEAD0000
    MOV AH, [CS:Status]
    MOV ECX, 0xCAFECAFE
    MOV EDX, 0xBAD0BAD0
    MOV EBX, 0xBAD1BAD1
    MOV ESI, 0xBAD2BAD2
    MOV EDI, 0xBAD3BAD3
    MOV EBP, 0xBAD4BAD4
    PUSH AX
    MOV AX, 0x40
    MOV DS, AX
    MOV ES, AX
    POP AX
    IRET
Status: DB 0
    TIMES 2048 - ($-$$) DB 0
)ASM";
}
