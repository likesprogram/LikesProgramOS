# Common — 构建期工具的共享代码

放在这里的代码被 `Tools/Build` 下的构建期工具共用（主要是 MakeImage 与 TestInstaller），**不是**目标系统的容器或模块；跨分类共用的镜像头、摘要与宿主文件读写见 [Tools/Common](../../Common/README.md)：

| 模块 | 内容 |
| --- | --- |
| `Image` | 输出镜像：按绝对字节偏移写入、长度自跟踪、孔洞显式写零，保证同一输入产出同样的字节 |
| `Baleen` | 介质上的引导结构：描述符区（独占一个扇区，头部 + 段序列，位置字段以 512 字节为基准）、El Torito 引导目录与 MBR 分区项；段产物在 `Packages/Baleen/Common/Out/Descs/`，本层只负责回填起止位置；Stub 文件长度由 `CheckStubBytes` 按跨介质共用上限 `0x8000` 检查，MakeImage 与 TestInstaller 共用这一份实现 |
| `Fat` | FAT 引导扇区判定：按 Microsoft FAT 规范的簇数规则分出 FAT12/16/32，供 ESP 与 El Torito 引导镜像的"必须 FAT16"校验使用（双 ESP 布局要求它同时落在 1 MiB 与 8 MiB、体积不超过 7 MiB） |
| `BaleenLayout` | 布局描述 `BaleenLayout.bin` 的宿主侧编码：按[布局描述](../../../Docs/Specs/Baleen/布局描述.md)的字节表生成字节块、从 Ext4 系统卷读卷标识、回填系统卷位置与状态区域并算校验和 |
| `KernelImage` | 内核镜像数据 `LikesProgram.os` 文件起点那份数据的宿主侧填充与校验：按[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)的字节表填 BuildId 与 Digest、复核字段与整幅摘要，供 PackImage 使用 |

为什么集中在这里：引导结构必须只有一份实现——ISO 混合镜像与磁盘镜像若各写一份 MBR 分区表或描述符区的回填规则，就会在字段、对齐或校验上悄悄漂移，而这两条路径交付给一级引导的必须是同一处字节。MakeImage 与 TestInstaller 各自用 `vpath` 把这里的源码编译进自己的 `Out/Obj`，因此每个工具目录仍可单独构建。

约定：C++20，无外部依赖，头文件在 `Include/`、实现在 `Src/`；写出的字段值与 `Packages/Baleen/Ipl` 的汇编常量、`Packages/Baleen/Common/Include/Contract.inc` 的交权契约常量保持一致，改动时必须同步。
