; Const.inc
;    Core 运行期常量：这里只放 Core 自己的项
;
;    交权契约常量（Core 装载地址、交权块长度与介质编号）由 Baleen 共享定义提供，
;    不要在本文件重复定义

%INCLUDE "Contract.inc"

CORE_STACK_BYTES EQU 0x4000      ; Core 自己的保护模式栈，在未初始化区，向下增长
