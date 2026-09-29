; StubHeader.inc
;    Stub 自身完整性头的常量：供 Stub.asm 组装镜像里的头字节
;
;    头的布局与固定取值由 Baleen/Common 的 ImageHeader.hpp 单点定义，Stub 与 Core 同构；
;    C++ 侧不再另设头文件，本文件只保留汇编需要的字面量与字节表编号
;    字节表与字段语义见 Packages/Baleen/Stub/README.md 第三节，取值与宿主侧
;    Tools/Build/Common/Include/BaleenImage.h 必须一致，改动要同时同步

STUB_HEAD_OFFSET    EQU 0x10     ; 头相对文件起点的偏移
STUB_HEAD_BYTES     EQU 0x80     ; 头固定长度
STUB_ENTRY_OFFSET   EQU 0x90     ; 早期初始化入口的文件偏移，即入口前缀的目标
STUB_VERSION        EQU 1        ; 未发布的开发格式标记
STUB_FLAGS          EQU 0        ; 当前不定义可选标志
STUB_DIGEST_SHA256  EQU 1        ; 摘要算法编号：SHA-256，没有“禁用摘要”值
STUB_BUILDID_BYTES  EQU 32       ; BuildId 长度
STUB_BUILDID_OFFSET EQU 0x20     ; BuildId 相对头起点的偏移，由打包器填充
STUB_DIGEST_OFFSET  EQU 0x40     ; Digest 相对头起点的偏移，由打包器填充
STUB_RESERVE_BYTES  EQU 0x20     ; 头内保留字段长度
