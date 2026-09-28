; Const.inc
;    Stub 运行期常量：这里只放 Stub 自己的项
;
;    交权契约常量（装载窗口、实模式栈顶与介质编号）由 Baleen 共享定义提供，
;    不要在本文件重复定义

%INCLUDE "Contract.inc"

LOADER_PM_STACK  EQU 0x7000      ; 保护模式栈顶，在 IPL 区下方，向下增长
