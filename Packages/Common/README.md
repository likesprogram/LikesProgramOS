# Packages/Common — 引导器与内核共用的基础件

本包放 Baleen 各阶段与内核共用的内容。口径是：**共用接口声明与无设备依赖的纯逻辑，设备实现与策略各侧自己持有**——引导器与内核的运行环境（位宽、内存模型、运行库有无）不同，把设备相关实现放进共用包会把两侧绑死。

| 目录 | 内容 | 主要使用方 |
| --- | --- | --- |
| `Include/Print/`、`Src/Print.cpp` | 文本输出前端、多路复用目标、串口与 `0xE9` 目标 | Baleen 各阶段、内核 |
| `Include/Platform/`、`Src/Platform/` | CPU 事实探测与 A20、GDT/IDT 的编码与装载机制 | 同上 |
| `Include/Storage/`、`Src/Storage/` | 块设备、偏移视图、统一路径与文件系统抽象 | 同上 |
| `Include/BootInfo.hpp` | 引导交接契约：尺寸、版本、固件状态、介质与内存图 | Baleen 各阶段、内核 |
| `Include/KernelImage.hpp`、`Src/KernelImage.cpp` | 内核镜像数据的字段布局与校验，字节表见 [内核镜像数据](../../Docs/Specs/Baleen/内核镜像数据.md) | BaleenCore、内核侧 Core |
| `Include/BootState.hpp`、`Src/BootState.cpp` | 引导状态记录：双副本校验与选择、写入顺序（写 → 刷盘 → 读回）、副本级检查与受控恢复（清零 / 按有效副本修复）与人工接受，字节表见 [引导状态记录](../../Docs/Specs/Baleen/引导状态记录.md) | BaleenCore 的选版、运行期 Updater / Init、维护工具 [FixBootState](../../Tools/Maintain/FixBootState/README.md) |
| `Include/Revocation.hpp`、`Src/Revocation.cpp` | 签名吊销记录（`LPRV`）：字节表、结构检查、签名覆盖准备、签名者自吊销拒绝、位图判定与并集，以及从系统卷读取的三态结论，见[内核集清单与签名](../../Docs/Specs/内核集清单与签名.md) 5.1、5.2 | BaleenCore 的选版、宿主工具 [MakeRevocation](../../Tools/Build/MakeRevocation/README.md) 与 [CheckRevocation](../../Tools/Test/CheckRevocation/README.md)；运行期读取方待内核存储阶段 |

`BootState` 放在共用包是**规格要求的一部分**：运行期确认写入与引导期选择必须使用同一状态存储后端和记录格式（[内核集更新与回滚](../../Docs/Specs/内核集更新与回滚.md)第三节），因此两侧引用同一份实现，而不是各写一份“约定”。记录里的 `SeenSecurityVersion` 只在人工接受时下调（同样在本文件里实现），抬高由选择器随清单更新完成；记录还带吊销事实（纪元、下限、已吊销密钥位图），运行期写入方必须读—改—写、原样保留这三个字段与已试次数 `TriesDone`。**记录格式尚未发布，只有版本 1**：字段调整直接改字节表，不留兼容分支。

`Revocation` 同样只在共用包放**协议与判定**：结构检查、签名覆盖的准备（签名值取出、字段归零）、签名者自吊销拒绝、位图与并集都在这份实现里；**验签实现由调用方链入**（`Load` 收一个取公钥的回调与一个验签回调），因此本包不依赖任何加密头，引导器与将来的内核读取方各自链入自己的 Ed25519。

`Storage/Ext4Features.hpp` 定义 Ext4 卷格式的接受与拒绝清单（`INCOMPAT` 与 `RO_COMPAT` 位），Baleen 只读实现与内核完整实现引用同一份，避免出现一侧能读、另一侧读不了的布局。接受集合里另分**解析前提位**：目录项的 `FILETYPE` 与 extent 树的 `EXTENTS` 是只读解析路径的硬前提，缺失位的卷在挂载阶段就被拒绝，判定不散到读目录项或 extent 的最后一步。

存储抽象的口径见 [文件系统抽象与 VFS](../../Docs/Specs/Common/文件系统抽象与VFS.md)：接口只有一份定义，块设备实现（BIOS 读盘、AHCI、NVMe、固件块协议）与卷解析（Ext4 只读最小实现 / 完整实现、ISO9660、FAT32）由各阶段自己写。`Storage/Path.hpp` 定义**统一的路径形式与分量切分**：Ext4 区分大小写、ISO9660 与 FAT32 不区分，比较规则由各文件系统实现决定，并经 `FileSystemCaps::nameCase` 报告给调用方，共用层不替任何一侧做大小写折叠。

接口的硬约束：不依赖 C++ 运行库（无异常、无 RTTI、不动态分配、不用虚析构），接口方法都有默认实现而不是纯虚函数（纯虚会让引导镜像链接 `__cxa_pure_virtual`），实例由提供方就地构造与销毁。

构建是 32 位保护模式（`-m32`，`-ffreestanding`），对象产出在 `Out/Obj`；内核侧将按自己的编译选项引用同一份源码。`make` 与 `make clean` 只覆盖本包目录，子包通过 `upstream` 调用本包的规则。
