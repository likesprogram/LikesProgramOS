; Const.inc
;    内核侧 Core 的汇编常量：映像布局、数据块字段取值与各段大小
;
;    数据块的字段偏移与固定取值由 Packages/Common/Include/KernelImage.hpp 单点定义，
;    本文件与之一一对应，改动要同时同步；KERNEL_LOAD 必须与数据里的 LoadAddress 一致

KERNEL_LOAD          EQU 0x1000000   ; 内核映像的装载物理地址，2MiB 对齐
KERNEL_ENTRY_OFFSET  EQU 0x80        ; 入口的文件偏移，即数据块之后的第一个字节
KERNEL_STACK_BYTES   EQU 0x4000      ; 内核运行期栈，在未初始化区，向下增长

KERNEL_DATA_BYTES    EQU 0x80        ; 内核镜像数据长度
KERNEL_DATA_VERSION  EQU 1           ; 数据格式版本
KERNEL_DIGEST_SHA256 EQU 1           ; 摘要算法编号：SHA-256，没有“禁用摘要”值
KERNEL_BUILDID_BYTES EQU 32          ; BuildId 长度
KERNEL_BOOTINFO_VER  EQU 1           ; 内核要求的 BootInfo 版本，与 BootInfo.hpp 的版本一致
