# Packages/Common — 引导器与内核共用的基础件

本包放 Baleen 各阶段与内核共用的内容。口径是：**共用接口声明与无设备依赖的纯逻辑，设备实现与策略各侧自己持有**——引导器与内核的运行环境（位宽、内存模型、运行库有无）不同，把设备相关实现放进共用包会把两侧绑死。

| 目录 | 内容 | 主要使用方 |
| --- | --- | --- |
| `Include/Print/`、`Src/Print.cpp` | 文本输出前端、多路复用目标、串口与 `0xE9` 目标 | Baleen 各阶段、内核 |
| `Include/Platform/`、`Src/Platform/` | CPU 事实探测与 A20、GDT/IDT 的编码与装载机制 | 同上 |
| `Include/Storage/`、`Src/Storage/` | 块设备、偏移视图、统一路径与文件系统抽象 | 同上 |

存储抽象的口径见 [文件系统抽象与 VFS](../../Docs/Specs/Common/文件系统抽象与VFS.md)：接口只有一份定义，块设备实现（BIOS 读盘、AHCI、NVMe、固件块协议）与卷解析（Ext4 只读最小实现 / 完整实现、ISO9660、FAT32）由各阶段自己写。`Storage/Path.hpp` 定义**统一的路径形式与分量切分**：Ext4 区分大小写、ISO9660 与 FAT32 不区分，比较规则由各文件系统实现决定，并经 `FileSystemCaps::nameCase` 报告给调用方，共用层不替任何一侧做大小写折叠。

接口的硬约束：不依赖 C++ 运行库（无异常、无 RTTI、不动态分配、不用虚析构），接口方法都有默认实现而不是纯虚函数（纯虚会让引导镜像链接 `__cxa_pure_virtual`），实例由提供方就地构造与销毁。

构建是 32 位保护模式（`-m32`，`-ffreestanding`），对象产出在 `Out/Obj`；内核侧将按自己的编译选项引用同一份源码。`make` 与 `make clean` 只覆盖本包目录，子包通过 `upstream` 调用本包的规则。
