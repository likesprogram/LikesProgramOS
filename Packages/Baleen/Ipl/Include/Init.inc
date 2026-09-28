; Init.inc
;    两种介质入口共用的初始化：归一化段与标志、建立栈、保存固件驱动器号

CPU 386
%INCLUDE "Const.inc"

; BIOS 入口：统一 CS=0x07C0，使 ORG 0 的标签落在物理 0x7C00
_Start:
    JMP 0x07C0:After_CS
; 远跳后 CS 已固定，再统一 DS/ES/SS 与栈顶
After_CS:
    CLI
    XOR AX, AX
    MOV SS, AX
    MOV SP, STACK_TOP
    MOV ES, AX
    PUSH CS
    POP DS
    CLD
    STI
    MOV [_Saved_Dl], DL
