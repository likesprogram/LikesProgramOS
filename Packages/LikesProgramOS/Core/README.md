# 内核侧 Core — 职责与当前实现

`Packages/LikesProgramOS/Core` 是内核侧的第一阶段，产出内核映像 `Out/Bin/LikesProgram.os`。它对应[内核架构与自举闭包](../../../Docs/Specs/内核架构与自举闭包.md)第二节里“平台初始化”“物理内存与虚拟内存”“输出与诊断”三块的自举部分：入口、`BootInfo` 核对、固件启动服务失效、描述符表、引导页表与直映射、页帧分配器。**当前停在阶段边界上**：设备模型、模块装载、调度、IRQ 与 Init 都还没有实现（第五节）。移交路径已接通：BaleenCore 把映像装到 `LoadAddress`、构造 `BootInfo` 并切长模式跳入本入口，内核从自己的阶段边界继续。

按与 BaleenCore 相同的分工：`Src/Core.cpp` 承担全部启动流程与全部诊断输出——步骤顺序、进度行、失败原因与停机都集中在这一段里；`Src/Console.cpp` 只按 `BootInfo` 声明的通道装配早期控制台，`Src/Tags.cpp` 写诊断标签，`Src/Cpu.cpp`、`Src/Tables.cpp`、`Src/Paging.cpp`、`Src/Memory.cpp` 各自只提供机制，都不打印也不停机；`Src/Core.asm` 是内核镜像数据、入口、异常桩与自己的栈；`Src/Libc.cpp` 提供 64 位无运行库环境引用的最小 C 运行库函数。共享件按内核的编译选项重编一份：`Packages/Common` 的 Print 前端、串口与调试口目标、描述符表机制与[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)校验。

## 一、入口与镜像

| 项 | 约定 |
| --- | --- |
| 镜像形态 | 文件偏移 `0` 起是 128 字节[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)，`0x80` 起是长模式代码；不使用 ELF 头、压缩或重定位 |
| 装载地址 | 物理 `0x1000000`（16 MiB，2MiB 对齐）；数据里的 `LoadAddress` 与链接脚本一致，由 `make check` 与内核入口双重核对 |
| 入口状态 | 长模式、平坦段、分页已开启（引导映射按恒等映射覆盖低 4GiB）、`IF=0`、`DF=0`、`RDI` 指向 `BootInfo`；这些由交接契约保证，内核不重新推导 |
| 栈 | 自己的 16 KiB 栈在未初始化区（`Const.inc` 的 `KERNEL_STACK_BYTES`）；入口先 `MOV RSP` 换栈 |
| 未初始化区 | 数据里 `MemoryBytes` 超出文件的部分（BSS、栈、页表、位图）由引导器在交权前清零，内核侧不重复清零 |
| 数据字段填出方 | 长度字段由链接器按 `Core.ld` 的 `__image_end` / `__bss_end` 填入；`BuildId` 与 `Digest` 由 `Tools/Bin/PackImage` 填充；`make check` 与组装器都会复核 |

入口把 `RDI` 交给 `_Kernel_Main`，不返回。异常桩每个向量一个，把错误码与向量号整理成统一栈帧（向量号、错误码、返回偏移、CS、RFLAGS）后进公共入口，由 C++ 侧的 `_Kernel_Exception_Handler` 打印后停机；双故障与 NMI 的门使用 IST1 的应急栈，栈损坏时仍能留下诊断。

## 二、启动流程与诊断行

`_Kernel_Main` 的步骤顺序固定，每步一行标签诊断（内核侧标签按[输出通道与Print](../../../Docs/Specs/输出通道与Print.md)的同一文本写法，没有显示属性）：

1. 按 `BootInfo` 声明的通道装配早期控制台并打印横幅 `[ KERNEL ] LikesProgramOS kernel`
2. 核对自身内核镜像数据：固定字段与内核事实，再把 `ImageBytes`、`MemoryBytes`、`LoadAddress`、`EntryOffset` 与链接布局（`__image_end`、`__bss_end`、入口符号）交叉比对；摘要不重复取，引导器已在交接前比对
3. 核对 `BootInfo`：尺寸与版本先行，通过之后才是其余字段；`BootInfo` 版本与镜像数据要求的版本必须一致
4. 让固件启动服务失效：BIOS 路径无服务可退，只按状态标记确认；UEFI 路径若标记仍有效，报告“退出启动服务尚未实现”并停机，不假装已经接管
5. CPU 事实：厂商串与长模式、TSC、APIC、NX 能力位
6. 建立运行期段表、任务寄存器与中断表
7. 建立引导页表并切换 CR3：恒等映射与直映射窗口
8. 初始化页帧分配器：按内存图放行可用页，保留低地址与内核自身
9. 阶段边界：打印 `WARN` 行说明设备模型、模块装载、调度与 Init 尚未实现，停在这条边界上

## 三、平台初始化与内存层

| 部分 | 当前内容 |
| --- | --- |
| 描述符表 | GDT 五项：内核 64 位代码段 `0x08`、内核数据段 `0x10`、用户代码段 `0x18|3`、用户数据段 `0x20|3`、TSS `0x28`（占两个槽位）；IDT 覆盖 0 至 31 号异常，双故障与 NMI 用 IST1 |
| 页表与直映射 | 4 级页表，2MiB 大页覆盖低 4GiB：恒等映射保持映像与栈按物理地址可用，直映射窗口基址 `0xFFFF800000000000` 供内核访问物理内存；不按内存图精细映射，也不区分缓存策略，MMIO 策略随设备阶段引入 |
| 内存层 | 页帧分配器：位图放在未初始化区，管理低 4GiB；分配返回物理地址，访问经直映射换算；保留内核映像起点以下的全部地址、内核跨度自身、`BootInfo` 与其内存图数组 |

## 四、构建与校验

```sh
make -C Packages/LikesProgramOS/Core          # 产出 Out/Bin/LikesProgram.os
make -C Packages/LikesProgramOS/Core check    # 复核大小、入口偏移、ELF 依赖与内核镜像数据
make -C Packages/LikesProgramOS               # 产出系统卷 Out/LikesProgram.img（内核映像拷入 Static 的卷内落点）
```

`make check` 复核四项：镜像非空且不超过 `0x800000`、数据里的入口偏移为 `0x80`、ELF 不含 `.init_array` 与动态依赖、`PackImage --verify` 复核数据字段与整幅摘要。系统卷由 `Packages/LikesProgramOS/Makefile` 从 `Static/` 卷根用 `mke2fs -d` 就地生成，不挂载、不需要 root；内核映像的卷内落点与布局描述的默认内核路径一致，见[系统卷布局](../../../Docs/Specs/Common/系统卷布局.md)。

## 五、尚未实现

| 项目 | 内容 |
| --- | --- |
| 设备模型与驱动 | PCI/USB 与平台枚举、资源分配、启动设备驱动（ATA/AHCI/NVMe、USB 存储） |
| 模块与文件系统 | `.osm` 装载、VFS 与 Ext4 完整实现、ISO9660、FAT32 |
| 调度与中断框架 | 工作线程、抢占、IRQ 登记与分发；当前中断保持关闭，异常处理只做诊断 |
| 更高半区与更细映射 | `LoadAddress` 当前是物理常量 `0x1000000`；虚拟地址布局、每 CPU 表与更细的页粒度随相应阶段定义 |
| 早期计时与日志 | 时间源、内存日志 ring、栈回溯 |

本文只描述当前已实现的边界；后续阶段各自补各自的章节，不在这里提前承诺。
