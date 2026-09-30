# Baleen Core — 职责与当前实现

`Core` 是 BIOS 路径中继 IPL 与 Stub 之后的第二阶段。Stub 校验 `CoreDescriptor` 与镜像头、比对整幅摘要后，把 `BaleenCore.bin` 装到物理 1MiB，按 `CORE_LOAD + 头里的 EntryOffset` 跳入；入口时 `ESI` 指向交权块（定义见 [Stub 说明](../Stub/README.md)第二节）。Core 从交权块取得介质身份、内存图与固件服务入口，自己装配输出，此后不再依赖 Stub 的行为。

源码分工沿用 Stub 的约定：`Src/Core.cpp` 承担全部启动流程与全部诊断输出——步骤顺序、进度行、失败原因与停机都在这一段里；`Src/Console.cpp` 只做 Core 控制台的装配（VGA 文本、`0xE9` 与 COM1 三路组合，是独立于 Stub 的实例），不打印也不停机；`Src/Core.asm` 是入口、异常桩与自己的栈，不做诊断排版；`Include/CoreHeader.inc` 与 `Include/Const.inc` 是汇编侧常量。

**当前实现到交权核对与阶段边界为止**：横幅、交权块校验、镜像事实交叉核对、CPU 快照、运行期段表与中断表、环境事实播报都已可用；`BaleenLayout.bin`、Ext4、内核集校验与选版、内核交接尚未实现（第三节）。构建：`make -C Packages/Baleen/Core` 产出 `Out/Bin/BaleenCore.bin`，`make check` 复核长度上限、头字段与摘要，顶层组装按真实产物优先取用。

## 一、入口与镜像

| 项 | 约定 |
| --- | --- |
| 镜像形态 | 与 Stub 同构：文件偏移 `0` 起是 16 位近跳转前缀与保留区，`0x10` 起是 128 字节完整性头，`0x90` 起是 32 位保护模式代码；两种镜像只有 `Magic` 不同（Core 为 `BLNCORE`） |
| 装入与入口 | 物理 `0x100000`；Stub 直接跳到 `CORE_LOAD + EntryOffset`，**不经过文件起点的前缀** |
| 文件起点前缀 | 不被执行，只为与 Stub 镜像同构的格式要求而存在（只有 16 位跳转语义，宿主打包器核对它与 `EntryOffset` 一致）；从装入点按 32 位执行会跳飞 |
| 进入状态 | 32 位保护模式、平坦段（`CS=0x08`、`DS/ES/SS=0x10`）、分页关闭、`IF=0`、`DF=0`、`ESI` 指向交权块；这些由交权契约保证，Core 不重新推导 |
| 栈 | Core 自己的 16 KiB 栈在未初始化区（`Const.inc` 的 `CORE_STACK_BYTES`）；入口先 `MOV ESP` 换栈，不继续用 Stub 冻结的保护模式栈 |
| 未初始化区 | 头里 `MemoryBytes` 超出文件的部分由 Stub 在交权前清零，Core 侧不重复清零 |
| 头字段填出方 | 长度字段由链接器按 `Core.ld` 的 `__image_end` / `__bss_end` 填入；`BuildId` 与 `Digest` 由 `Tools/Bin/PackImage` 填充；`make check` 与顶层组装器都会复核 |

入口把 `ESI` 压栈后调用 `_Core_Main`，不返回。异常桩与 Stub 同构：每个向量一个桩，压向量号后进公共入口，由 C++ 侧的 `_Core_Exception_Handler` 打印向量、错误码与 `Eip` 后停机；Core 自己建立段表与中断表，换表之后异常不再落到 Stub 的门上。

## 二、启动流程与诊断行

`_Core_Main` 的步骤顺序固定，每步一行标签诊断（标签表见 [输出通道与Print](../../../Docs/Specs/输出通道与Print.md)）：

1. 装配 Core 自己的控制台并打印横幅 `[ CORE ] LikesProgramOS BaleenCore`；**不清屏**，Stub 的行留在屏上，引导链在串口与调试口里连续可读
2. 核对交权块：`Magic`、`Version`、`Bytes` 三项固定字段逐项检查，任一不符即以 `FATAL` 原因停机
3. 核对自身镜像事实：读自己的镜像头核对固定字段与入口前缀；再把头字段与链接布局（`__image_end` / `__bss_end`）、交权块的 `CoreLoad` / `CoreBytes` 交叉比对。**摘要不重复取**：Stub 已在交权前比对过整幅镜像，Core 只核对结构与配对关系
4. CPU 快照：记录模式与厂商串；模式不是 32 位保护模式即停机
5. 建立运行期段表与中断表；续行给出 `Cpu=`、`Mode=`、`Build=`
6. 播报环境事实：介质与扇区大小、Core 装载地址与字节数、内存图条数与截断标志、可用内存 MiB
7. 阶段边界：打印 `WARN` 行说明布局、卷与内核阶段尚未实现，停在这条边界上

`Build=` 的写法与 Stub 的约定相同：头里 BuildId 前 4 字节按字节顺序的十六进制，与构建日志里 `PackImage` / 组装器打印的 BuildId 是同一串字节，不引入小端整数解释，两阶段的诊断行因此可以直接对照（约定见 [Stub 说明](../Stub/README.md)第五节）。

## 三、尚未实现

| 项目 | 内容 |
| --- | --- |
| 布局描述 | `BaleenLayout.bin` 的读取与解释，系统卷与内核集路径的来源 |
| 卷访问 | 交权块 `readSectors` 之上的块设备与 Ext4 只读实现；接口见 [文件系统抽象与VFS](../../../Docs/Specs/Common/文件系统抽象与VFS.md) |
| 内核装载 | 内核集镜像头、摘要与签名校验、选版与持久状态 |
| 交接与模式 | 内核交接契约、长模式与分页的建立 |

本文只描述当前已实现的边界；后续阶段各自补各自的章节，不在这里提前承诺。
