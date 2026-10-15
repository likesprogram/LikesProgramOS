/* Fixtures.h
    CheckStub 的内嵌 NASM 夹具：探针 BaleenCore 的源码

    探针 BaleenCore 由 CheckStub 汇编后组装入口前缀与完整性头，Stub 校验头与摘要后把它装到
    1MiB 并交权；代码先核对交权块的格式标记，再依次调用 write、sectorSize、readSectors、
    writeSectors 与 memoryMapQuery 五个服务，全部通过就向 isa-debug-exit 端口写约定值退出，
    任一失败经 write 服务打印带原因的行后停机；交权块本身不可用时退回 0xE9 与 COM1

    writeSectors 只按契约写引导状态区：源是低地址花码缓冲，写后经 readSectors 读回逐字节比对；
    只读介质（光盘）以服务返回 0 为通过，判定按 CheckStub 传入的 EXPECT_WRITE_SECTORS
    汇编常量由 CheckStub 以 -D 传入，取值来自 CoreHandoff.hpp 与本工具的命令行，不在这里重复
*/

#pragma once
#include <string_view>

namespace checkstub {
    // 探针 BaleenCore：BITS 32，ORG 为装入地址加入口偏移，代码落在镜像文件偏移 0x90 处
    inline constexpr std::string_view kProbeCoreAsm = R"ASM(
BITS 32
ORG CORE_ENTRY

%ifndef HANDOFF_MAGIC
%define HANDOFF_MAGIC 0x484E4C42
%endif
%ifndef HANDOFF_VERSION
%define HANDOFF_VERSION 1
%endif
%ifndef HANDOFF_BYTES
%define HANDOFF_BYTES 64
%endif
%ifndef HANDOFF_SECTOR_BYTES
%define HANDOFF_SECTOR_BYTES 0x14
%endif
%ifndef HANDOFF_WRITE
%define HANDOFF_WRITE 0x30
%endif
%ifndef HANDOFF_READ
%define HANDOFF_READ 0x34
%endif
%ifndef HANDOFF_SECT
%define HANDOFF_SECT 0x38
%endif
%ifndef HANDOFF_QUERY
%define HANDOFF_QUERY 0x3C
%endif
%ifndef HANDOFF_WRITE_SECTORS
%define HANDOFF_WRITE_SECTORS 0x50
%endif
%ifndef EXPECT_SECT
%define EXPECT_SECT 512
%endif
%ifndef READ_ADDR
%define READ_ADDR 0x30000
%endif
%ifndef MMAP_ADDR
%define MMAP_ADDR 0x30200
%endif
%ifndef TRUNC_ADDR
%define TRUNC_ADDR 0x30400
%endif
%ifndef WRITE_LBA
%define WRITE_LBA 264
%endif
%ifndef WRITE_COUNT
%define WRITE_COUNT 1
%endif
%ifndef WRITE_BYTES
%define WRITE_BYTES 512
%endif
%ifndef WRITE_ADDR
%define WRITE_ADDR 0x31000
%endif
%ifndef READBACK_ADDR
%define READBACK_ADDR 0x32000
%endif
%ifndef EXPECT_WRITE_SECTORS
%define EXPECT_WRITE_SECTORS 1
%endif
%ifndef EXIT_OK
%define EXIT_OK 0x10
%endif

_Entry:
    ; 交权块格式核对：三项任一不符都不能再用其中的服务
    CMP DWORD [ESI], HANDOFF_MAGIC
    JNE Bad_Handoff
    CMP DWORD [ESI + 4], HANDOFF_VERSION
    JNE Bad_Handoff
    CMP DWORD [ESI + 8], HANDOFF_BYTES
    JNE Bad_Handoff

    ; write 服务：打印开始行，这一行同时是服务可用的证据
    MOV EBX, Msg_Ok
    CALL Write_Service

    ; sectorSize 服务：探测结果必须与本次介质的扇区大小一致
    MOV EAX, [ESI + HANDOFF_SECT]
    CALL EAX
    CMP EAX, EXPECT_SECT
    JNE Bad_Sector

    ; readSectors 服务：读 LBA 0 一个扇区到低地址缓冲，核对引导签名
    PUSH DWORD [ESI + HANDOFF_SECTOR_BYTES]
    PUSH DWORD READ_ADDR
    PUSH 1
    PUSH 0
    MOV EAX, [ESI + HANDOFF_READ]
    CALL EAX
    ADD ESP, 16
    TEST EAX, EAX
    JZ Bad_Read
    CMP WORD [READ_ADDR + 510], 0xAA55
    JNE Bad_Read

    ; writeSectors 服务：把低地址缓冲填成花码写到引导状态区，按介质性质核对返回值
    ; 可写介质再经 readSectors 读回同一批扇区逐字节比对；只读介质以返回 0 为通过
    CLD
    PUSH ESI
    MOV EDI, WRITE_ADDR
    MOV ECX, WRITE_BYTES / 4
    MOV EAX, 0xA5A55A5A
    REP STOSD
    PUSH DWORD [ESI + HANDOFF_SECTOR_BYTES]
    PUSH DWORD WRITE_ADDR
    PUSH DWORD WRITE_COUNT
    PUSH DWORD WRITE_LBA
    MOV EAX, [ESI + HANDOFF_WRITE_SECTORS]
    CALL EAX
    ADD ESP, 16
    CMP EAX, EXPECT_WRITE_SECTORS
    JNE Write_Bad
    TEST EAX, EAX
    JZ Write_ReadOnly
    ; 读回：先清读回缓冲，避免把旧内容当成写成功的证据
    MOV EDI, READBACK_ADDR
    MOV ECX, WRITE_BYTES / 4
    XOR EAX, EAX
    REP STOSD
    PUSH DWORD [ESI + HANDOFF_SECTOR_BYTES]
    PUSH DWORD READBACK_ADDR
    PUSH DWORD WRITE_COUNT
    PUSH DWORD WRITE_LBA
    MOV EAX, [ESI + HANDOFF_READ]
    CALL EAX
    ADD ESP, 16
    TEST EAX, EAX
    JZ Write_Bad
    MOV ESI, WRITE_ADDR
    MOV EDI, READBACK_ADDR
    MOV ECX, WRITE_BYTES
    REPE CMPSB
    JNE Write_Bad
    POP ESI
    MOV EBX, Msg_WriteOk
    CALL Write_Service
    JMP After_Write
Write_ReadOnly:
    POP ESI
    MOV EBX, Msg_WriteReadOnly
    CALL Write_Service
    JMP After_Write
Write_Bad:
    POP ESI
    MOV EBX, Msg_Write
    JMP Fail
After_Write:

    ; memoryMapQuery 服务：取一条记录，返回非零即视为可用
    PUSH DWORD TRUNC_ADDR
    PUSH 1
    PUSH DWORD MMAP_ADDR
    MOV EAX, [ESI + HANDOFF_QUERY]
    CALL EAX
    ADD ESP, 12
    TEST EAX, EAX
    JZ Bad_Mmap

    ; 全部通过：向退出端口写约定值，QEMU 以固定退出码结束
    MOV AL, EXIT_OK
    OUT 0xF4, AL
Hang:
    HLT
    JMP Hang

; 用交权块的 write 服务打印 EBX 指向的字符串
Write_Service:
    PUSH EBX
    MOV EAX, [ESI + HANDOFF_WRITE]
    CALL EAX
    ADD ESP, 4
    RET

Bad_Sector:
    MOV EBX, Msg_Sector
    JMP Fail
Bad_Read:
    MOV EBX, Msg_Read
    JMP Fail
Bad_Mmap:
    MOV EBX, Msg_Mmap
Fail:
    CALL Write_Service
    JMP Hang

; 交权块不可用时的直接通道：0xE9 与 COM1 各写一遍，不依赖任何服务
Bad_Handoff:
    MOV EBX, Msg_Handoff
Direct:
    MOV AL, [EBX]
    TEST AL, AL
    JZ Hang
    OUT 0xE9, AL
.WaitTx:
    MOV DX, 0x3FD
    IN AL, DX
    TEST AL, 0x20
    JZ .WaitTx
    MOV AL, [EBX]
    MOV DX, 0x3F8
    OUT DX, AL
    INC EBX
    JMP Direct

Msg_Ok:      DB "CHECKSTUB-BALEENCORE-OK services", 13, 10, 0
Msg_WriteOk: DB "CHECKSTUB-BALEENCORE-OK write-sectors verified", 13, 10, 0
Msg_WriteReadOnly: DB "CHECKSTUB-BALEENCORE-OK write-sectors read-only", 13, 10, 0
Msg_Sector:  DB "CHECKSTUB-CORE-FAIL sector-size", 13, 10, 0
Msg_Read:    DB "CHECKSTUB-CORE-FAIL read-sectors", 13, 10, 0
Msg_Write:   DB "CHECKSTUB-CORE-FAIL write-sectors", 13, 10, 0
Msg_Mmap:    DB "CHECKSTUB-CORE-FAIL memory-map", 13, 10, 0
Msg_Handoff: DB "CHECKSTUB-CORE-FAIL handoff", 13, 10, 0
)ASM";
}
