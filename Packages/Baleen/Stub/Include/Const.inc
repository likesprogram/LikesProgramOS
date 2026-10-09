; Const.inc
;    Stub 运行期常量：这里只放 Stub 自己的项
;
;    交权契约常量（装载窗口、实模式栈顶与介质编号）由 Baleen 共享定义提供，
;    不要在本文件重复定义

%INCLUDE "Contract.inc"

STUB_STACK_BYTES EQU 0x1000      ; 保护模式栈长度，栈在未初始化区末尾，向下增长
