# Common — 宿主侧构建工具共享代码

放在这里的代码被 `Tools/Build` 下的多个宿主工具共用，**不是**目标系统的容器或模块：

| 模块 | 内容 |
| --- | --- |
| `Image` | 输出镜像：按绝对字节偏移写入、长度自跟踪、孔洞显式写零，保证同一输入产出同样的字节 |
| `Baleen` | 介质上的引导结构：描述符区（独占一个扇区，头部 + 段序列，位置字段以 512 字节为基准）、El Torito 引导目录与 MBR 分区项；段产物在 `Packages/Baleen/Common/Out/Descs/`，本层只负责回填起止位置 |
| `HostIo` | 宿主文件读取、数字解析、布局记账（区间不重叠）与清单 JSON 片段 |
| `Fat` | FAT 引导扇区判定：按 Microsoft FAT 规范的簇数规则分出 FAT12/16/32，供 ESP 与 El Torito 引导镜像的"必须 FAT16"校验使用（双 ESP 布局要求它同时落在 1 MiB 与 8 MiB、体积不超过 7 MiB） |
| `Sha256` | SHA-256 摘要算法的宿主侧实现，与目标侧 `Packages/Baleen/Common` 同算法、用测试向量与真实镜像互验 |
| `BaleenImage` | Baleen 镜像完整性头：Stub 与 Core 同构的字段常量、BuildId 与 Digest 填充、头字段与摘要校验，供 PackImage、通用镜像制作器与测试安装器的载荷门禁使用 |

为什么集中在这里：引导结构必须只有一份实现——ISO 混合镜像与磁盘镜像若各写一份 MBR 分区表或描述符区的回填规则，就会在字段、对齐或校验上悄悄漂移，而这两条路径交付给一级引导的必须是同一处字节。MakeImage 与 TestInstaller 各自用 `vpath` 把这里的源码编译进自己的 `Out/Obj`，因此每个工具目录仍可单独构建。

约定：C++20，无外部依赖，头文件在 `Include/`、实现在 `Src/`；写出的字段值与 `Packages/Baleen/Ipl` 的汇编常量、`Packages/Baleen/Common/Include/Contract.inc` 的交权契约常量、`Packages/Baleen/Common/Include/ImageHeader.hpp` 的镜像头布局、`Packages/Baleen/Stub/Include/StubHeader.inc` 保持一致，改动时必须同步。
