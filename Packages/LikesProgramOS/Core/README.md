# 内核侧 Core — 职责与当前实现

`Packages/LikesProgramOS/Core` 是内核核心实现，产出内核映像 `Out/Bin/LikesProgram.os`。它对应[内核架构与自举闭包](../../../Docs/Specs/内核架构与自举闭包.md) 2.1 的内核核心：入口、`BootInfo` 核对、固件启动服务失效、描述符表、引导页表与直映射、页帧分配器、交接包校验与缓冲接管、内核核心导出、模块装载编排、系统卷挂载、清单读取、ISO9660 第二来源核对与运行期模块装载。**当前停在调度与进程模型之前**：引导闭包 13 个模块全部装载并 `Init`、系统卷挂载与清单读取完成、运行期 `.osm` 按需装载；调度、IRQ 框架、Init 与用户空间尚未实现（第五节）。

移交路径已接通：BaleenCore 把映像装到 `LoadAddress`、构造 `BootInfo` 并切长模式跳入本入口，内核从自己的启动流程继续，不再返回引导器。

按与 BaleenCore 相同的分工：`Src/Core.cpp` 承担全部启动流程与全部诊断输出——步骤顺序、进度行、失败原因与停机都集中在这一段里；`Src/Console.cpp` 只按 `BootInfo` 声明的通道装配早期控制台，`Src/Tags.cpp` 写诊断标签与带编号的诊断行，`Include/Diagnostics.hpp` 定义 `E-KRN` 编号常量（制度见[内核诊断编号](../../../Docs/Specs/内核诊断编号.md)），其余机制文件各自只提供机制、不打印也不停机——`Src/Cpu.cpp`、`Src/Tables.cpp`、`Src/Paging.cpp`、`Src/Memory.cpp`、`Src/MmioMap.cpp`、`Src/ScreenTarget.cpp`、`Src/Reset.cpp`，以及装载器一侧的 `Src/ModuleContainer.cpp`、`Src/ModuleImage.cpp`、`Src/ModuleRelocation.cpp`、`Src/ModuleLoader.cpp`、`Src/ModuleRegistry.cpp`、`Src/InterfaceRegistry.cpp`、`Src/DependencyGraph.cpp`、`Src/Objects.cpp`、`Src/Handoff.cpp`、`Src/ManifestView.cpp` 与 `Src/Libc.cpp`；`Src/Core.asm` 是内核镜像数据、入口、异常桩与自己的栈。共享件按内核的编译选项重编一份：`Packages/Common` 的 Print 前端、串口与调试口目标、描述符表机制与[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)校验。

## 一、入口与镜像

| 项 | 约定 |
| --- | --- |
| 镜像形态 | 文件偏移 `0` 起是 128 字节[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)，`0x80` 起是长模式代码；不使用 ELF 头、压缩或重定位 |
| 装载地址 | 物理 `0x1000000`（16 MiB，2MiB 对齐）；数据里的 `LoadAddress` 与链接脚本一致，由 `make check` 与内核入口双重核对 |
| 入口状态 | 长模式、平坦段、分页已开启（引导映射按恒等映射覆盖低 4GiB）、`IF=0`、`DF=0`、`RDI` 指向 `BootInfo`；这些由交接契约保证，内核不重新推导 |
| 栈 | 自己的 32 KiB 栈在未初始化区（`Const.inc` 的 `KERNEL_STACK_BYTES = 0x8000`）；入口先 `MOV RSP` 换栈。装载期的容器视图、装载图与 256 KiB 运行期模块缓冲都放静态存储，不占栈 |
| 未初始化区 | 数据里 `MemoryBytes` 超出文件的部分（BSS、栈、页表、位图、装载器静态工作区）由引导器在交权前清零，内核侧不重复清零 |
| 数据字段填出方 | 长度字段由链接器按 `Core.ld` 的 `__image_end` / `__bss_end` 填入；`ModuleMajor` 由 `Const.inc` 的 `KERNEL_MODULE_MAJOR` 给出（发布线，非零）；本包产出的是 `BuildId` 与 `Digest` 全零的原型，发布线身份与摘要由 `Tools/Bin/MakeManifest` 在系统卷组装期写入，`PackImage` 只校验；规则见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md) 2.2、[模块修订与更新](../../../Docs/Specs/模块修订与更新.md)与[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)第三节 |

入口把 `RDI` 交给 `_Kernel_Main`，不返回。异常桩每个向量一个，把错误码与向量号整理成统一栈帧（向量号、错误码、返回偏移、CS、RFLAGS）后进公共入口，由 C++ 侧的 `_Kernel_Exception_Handler` 打印后停机；双故障与 NMI 的门使用 IST1 的应急栈，栈损坏时仍能留下诊断。失败路径统一为 `FATAL` + 屏幕可见 + 等待（默认 10 秒）+ 平台复位；复位不可用时报 `E-KRN-91` 后停机。

## 二、启动流程与诊断行

`_Kernel_Main` 的步骤顺序固定，每步一行标签诊断（内核侧标签按[输出通道与Print](../../../Docs/Specs/输出通道与Print.md)的同一文本写法与同一张属性表，屏幕目标存在时着色）；失败行带 `E-KRN` 稳定编号，行格式见[内核诊断编号](../../../Docs/Specs/内核诊断编号.md)：

1. 按 `BootInfo` 声明的通道装配早期控制台并打印横幅 `[ KERNEL ] LikesProgramOS kernel`
2. 核对自身内核镜像数据：固定字段与内核事实，再把 `ImageBytes`、`MemoryBytes`、`LoadAddress`、`EntryOffset` 与链接布局（`__image_end`、`__bss_end`、入口符号）交叉比对；摘要不重复取，引导器已在交接前比对（失败落 `E-KRN-10`–`14`）
3. 核对 `BootInfo`：尺寸与版本先行，通过之后才是其余字段；`BootInfo` 版本与镜像数据要求的版本必须一致（失败落 `E-KRN-01`–`05`）
4. 让固件启动服务失效：BIOS 路径无服务可退，只按状态标记确认；UEFI 路径由引导器在交权前退出，内核只核对标记——固件状态枚举未知落 `E-KRN-06`，UEFI 启动服务仍生效落 `E-KRN-07`
5. CPU 事实：厂商串与长模式、TSC、APIC、NX 能力位（CPUID 不可用落 `E-KRN-08`）
6. 建立运行期段表、任务寄存器与中断表（失败落 `E-KRN-40`）；之后 CPU 异常停在诊断行上，而不是三重故障
7. 建立引导页表并切换 CR3：恒等映射 + 直映射窗口（失败落 `E-KRN-30`）；MMIO 高地址窗口按需建立 4 KiB 映射
8. 核对启动来源并播报：`BootInfo` 的设备 / 卷 / 路径 / 状态区线索一致时打 `[ OK ] The boot source is consistent`；字段缺失或不一致只按 `WARN E-KRN-20`–`23` 播报，不停机
9. 交接包校验（9 步）：清单副本、`Modules.list` 副本、预装记录与内核镜像事实的一致性；列表条目集合与清单 `Type=2` 相等、路径主名派生 `ModuleId` 把条目分为引导闭包成员与运行期模块、预装记录只覆盖闭包成员；任何一项不成立都按 `E-KRN-50`–`59` 停机，不交接、不降级（`Handoff.cpp` / `ManifestView.cpp`）
10. 初始化页帧分配器：按内存图放行可用页、保留低地址与内核自身（`ManagedMiB=` / `FreeMiB=` 行）
11. 标占交接区间：三类缓冲与每个模块字节在首次分配前逐段 `Reserve`（失败归 `E-KRN-50`）
12. 接管交接缓冲：把三类缓冲与模块字节复制到内核自有 4 KiB 帧，此后内核只读自有副本（失败归 `E-KRN-50`）
13. 登记内核核心导出：`Core.*` 十张槽表（失败落 `E-KRN-59`）
14. 装载引导闭包：按预装顺序与依赖拓扑解析 13 个容器并逐个 `LoadModule`（`closureModule=true`），每个 `Init` 成功打印 `[ OK ] module init ok: <逻辑名>`；模块 `Init` 之间由内核编排 `Drv.Loader.Start`（失败落 `E-KRN-56`–`59`、驱动启动失败落 `E-KRN-62`）
15. 挂载系统卷并读取清单：`Boot.Volume.MountSystemVolume` 按 `BootInfo` 区段唯一候选核对卷标识（失败 `E-KRN-70`–`72`）→ 由 `kernelPath` 推出集根并经 VFS 读 `Manifest.mft` 前 32 字节（失败 `E-KRN-73`–`76`）；成功行 `[ OK ] system volume mounted: uuid=…` 与 `[ OK ] system manifest read: bytes=32 digest=…`
16. ISO9660 第二来源核对（仅光盘介质档）：内核核心从 ISO9660 主卷描述符与根目录项推出 `SystemVolume.img` 的区段（LBA × 2048 字节），与 `BootInfo.volumeOffset` / `volumeBytes` 比对；一致打印 `[ OK ] iso9660: system volume file=SystemVolume.img bytes=… offset=…`，不一致按 `E-KRN-77` 停机，ISO 卷不可读或根目录没有该文件只 `WARN E-KRN-78`（不改变已完成的挂载判定）
17. 运行期模块装载：`Modules.list` 里不属于引导闭包期望集的条目逐条按集根拼路径、`Stat`/`Open`/`Read` 到 256 KiB 静态缓冲后交同一套装载编排（`closureModule=false`）；成功打印 `runtime module init ok: <逻辑名>`，全部处理完打印 `[ OK ] runtime modules: loaded=<n> failed=<n> optional=<n>`；必需失败 `E-KRN-80`–`82` 停机，可选失败只 `WARN E-KRN-83`
18. 阶段边界：打印 `WARN` 行说明调度、进程与 IPC 模型尚未实现，停在可观测状态上；阶段边界不是失败路径，不带编号

## 三、平台初始化与内存层

| 部分 | 当前内容 |
| --- | --- |
| 描述符表 | GDT 五项：内核 64 位代码段 `0x08`、内核数据段 `0x10`、用户代码段 `0x18`（RPL3，选择子 `0x1B`）、用户数据段 `0x20`（RPL3，选择子 `0x23`）、TSS `0x28`（占两个槽位）；IDT 覆盖 0 至 31 号异常，双故障与 NMI 用 IST1 |
| 页表与直映射 | 4 级页表，2MiB 大页覆盖低 4GiB：恒等映射保持映像与栈按物理地址可用，直映射窗口基址 `0xFFFF800000000000` 供内核访问物理内存；不按内存图精细映射，也不区分缓存策略 |
| MMIO 高地址窗口 | `kMmioWindowBase = 0xFFFF900000000000`，4 GiB 跨度、独立 PML4 槽位；`MmioMap` 支持 64 条映射 × 64 MiB、按需 4 KiB PT，映射属性 `present + rw + PWT + PCD`；`Core.Mmio` / `Core.PhysMap` 接线后 > 4 GiB BAR 可用（NVMe、xHCI 需要）。**已知限制**：模块映像仍恒等映射，没有 NX / W^X |
| 内存层 | 页帧分配器：位图放在未初始化区，管理低 4GiB；分配返回物理地址，访问经直映射换算；保留内核映像起点以下的全部地址、内核跨度自身、`BootInfo` 与其内存图数组；交接区间按段 `Reserve`，内核只读接管后的自有副本 |
| 模块装载平台 | 按 `ImageBytes` 取连续页帧作基址（4 KiB 对齐、恒等映射）、逐段拷贝与 BSS 清零；重定位只接受 `RELATIVE64` / `RELATIVE32`；容器视图（约 9.6 KiB）、装载图与运行期模块缓冲放静态存储；装载步骤 10 之前不执行模块代码 |

## 四、构建与校验

```sh
make -C Packages/LikesProgramOS/Core          # 产出 Out/Bin/LikesProgram.os
make -C Packages/LikesProgramOS/Core check    # 复核大小、入口偏移、ELF 依赖与内核镜像数据
make -C Packages/LikesProgramOS               # 产出系统卷 Out/LikesProgram.img（内核映像拷入 Static 的卷内落点）
```

`make check` 复核四项：镜像非空且不超过 `0x800000`、数据里的入口偏移为 `0x80`、ELF 不含 `.init_array` 与动态依赖、`PackImage --verify` 复核数据字段与原型状态（`BuildId` 与 `Digest` 全零是合法原型；集身份由系统卷包写入后再由 `PackImage` 复核 `Digest` 与 `--expect-build-id`）。系统卷由 `Packages/LikesProgramOS/Makefile` 从 `Static/` 卷根用 `mke2fs -d` 就地生成，不挂载、不需要 root；内核映像的卷内落点与布局描述的默认内核路径一致，见[系统卷布局](../../../Docs/Specs/Common/系统卷布局.md)。

顶层 `make` 会把本包与 13 个引导闭包模块一起打包、签名并组装成 `Out/LikesProgram.iso` / `Out/LikesProgram.hdd`；`make check-all` 用宿主检查（30 个工具、990 例）、启动矩阵（12 档，含 UEFI NVMe 4Kn 与 xHCI USB 档）与引导状态演练（19 档，含同集两修订切换 3 档）验证装载链路；`make check-all WITH_BOOT=1` 另跑运行期模块夹具 4 档（G7 总检查项 1025），`ACCEPT=1` 再加失败注入 8 档与 4Kn 状态推进 5 档（状态演练共 24 档，G7 总检查项 1038）。

## 五、尚未实现

| 项目 | 内容 |
| --- | --- |
| 调度与中断框架 | 工作线程、抢占、IRQ 登记与分发；当前 `IF=0`，中断保持关闭，异常处理只做诊断 |
| Init 与用户空间 | 进程模型与 IPC、Init 与常驻服务、AppSDK；内核停在阶段边界，用户态尚未实现 |
| 写入与回放 | Ext4 写路径与日志回放、`Activate` 生命周期、写缓存刷新在持久化路径上的使用；当前文件系统与驱动全部只读 |
| 设备与介质扩展 | USB hub 下游设备枚举、UAS 传输、FAT32 / ISO9660 的内核挂载消费者；布局描述重读与 `DeviceKind` 缩小候选范围 |
| 更高半区与更细映射 | `LoadAddress` 当前是物理常量 `0x1000000`，模块映像恒等映射、无 NX / W^X；虚拟地址布局、每 CPU 表与更细的页粒度随相应阶段定义 |
| 早期计时与日志 | 时间源、内存日志 ring、栈回溯（`Core.Time.TicksPerMs` 已有 PIT/TSC 标定，其余未实现） |

本文只描述当前已实现的边界；后续阶段各自补各自的章节，不在这里提前承诺。
