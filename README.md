# LikesProgramOS

从引导与内核起步，理解操作系统怎样工作，并把这条链路自己实现出来。

LikesProgramOS 是一个基于 ASM 与 C++20 实现的 x86_64 操作系统项目。它自己定义从固件环境到用户空间的整条链路：引导器在 BIOS/UEFI 环境里识别介质、定位系统卷、装载内核并完成交接；内核由 `LikesProgram.os` 与发行配置选择的 `.osm` 模块构成；再往上是由 Init 编排的用户空间与常驻服务。每一层都有正式规格（见[文档索引](#文档索引)），规格是设计的正式定义，本 README 只做总览与索引。

项目由两个平级子项目、一套宿主侧构建工具与一份规格文档组成：

| 组成 | 目录 | 职责 |
| --- | --- | --- |
| **Baleen**（引导器） | `Packages/Baleen` | 从不同硬件找到指定 Ext4 文件系统的指定路径，装载系统内核并完成交接 |
| **LikesProgramOS**（系统本体） | `Packages/LikesProgramOS` | 操作系统本体：`LikesProgram.os` 内核 + 发行配置选择的 `.osm` 模块 |
| 宿主侧构建工具 | `Tools/Build` | 介质镜像组装器 MakeIso / MakeHdd 与共享代码，可执行文件在 `Tools/Bin` |
| 规格文档 | `Docs/Specs` | 各领域的正式定义；与本 README 不一致时以规格为准 |

## 设计取向

1. **引导器只做引导器的事。** Baleen 负责固件环境里的介质识别、卷定位、内核镜像装载与机器状态准备，不装载模块、不建立运行期设备模型。引导模式覆盖 BIOS 与 UEFI（含无 CSM 的纯 UEFI），并把 coreboot、虚拟化直启等列为计划支持；介质覆盖内部硬盘、USB 移动存储与 ISO（写入 U 盘或移动硬盘、虚拟机挂载）。范围与边界见 [Baleen 引导器](Docs/Specs/Baleen/README.md)。
2. **进入 `KernelMain` 的第一步让固件失效。** 不是假定固件已经卸载：内核在第一步退出固件启动服务，此后固定运行模式——不降级、不依赖固件运行期服务，全部使用自身实现。见[内核启动与固件边界](Docs/Specs/内核启动与固件边界.md)。
3. **内核集整套构建、整套更新。** 一次构建的 `.os`、所选 `.osm` 与构建清单共享 `BuildId`，不接受跨构建混装；选版、试启动、确认和回滚以整个内核集为单位。见[内核集更新与回滚](Docs/Specs/内核集更新与回滚.md)。
4. **必备能力内置。** USB（xHCI、USB 2.0/3.x、BOT/UAS）、Ext4、ISO9660、FAT32 与统一设备模型随 `.os` 提供，退出固件启动服务后即可读取系统卷、装载模块并报告错误。见[内核架构与自举闭包](Docs/Specs/内核架构与自举闭包.md)。
5. **系统卷是 Ext4 的指定路径。** 磁盘上是分区里的卷，ISO 上是卷内的镜像文件；引导器按路径取内核，内核自己再校验一次卷标识与布局。见[启动介质与文件系统](Docs/Specs/启动介质与文件系统.md)。

## 当前状态

引导链与构建链已经能跑通，内核与用户空间尚未实现：

| 部分 | 状态 |
| --- | --- |
| Baleen 一级引导（`Packages/Baleen/Ipl`） | 已实现：硬盘 / USB-HDD 与 El Torito 光盘两种形态，NASM 汇编，构建含尺寸硬约束检查（NASM 3.01 下 `make check` 通过）；源码、产物与运行期契约见 [Ipl 说明](Packages/Baleen/Ipl/README.md) |
| 宿主构建工具（`Tools/Build`） | 已实现：MakeIso 与 MakeHdd 组装可引导的 ISO 与磁盘镜像，MakePayloads 生成开发占位载荷；构建不依赖测试目录；用法见[工具 README](Tools/Build/MakeIso/README.md) |
| Baleen Stub（`Packages/Baleen/Stub`） | 阶段性实现：切保护模式、开 A20、建立运行期 GDT/IDT、异常诊断，BIOS 服务经实模式弹跳提供扇区探测、E820 内存图与低地址读盘；并按 [Stub 说明](Packages/Baleen/Stub/README.md)第二节的契约读取介质上的 `CoreDescriptor`、校验装载区、把 `BaleenCore` 装到 1MiB 并经交权块交给它（QEMU 与 Bochs 上 HDD / ISO / USB 形态实测均已走到占位 Core）；Core 本身尚未实现 |
| Baleen 其余阶段（BootCore、UEFI 侧） | 骨架，尚未实现 |
| 内核本体、模块、驱动、用户空间、SDK | 尚未实现（`Packages/LikesProgramOS`、`Packages/Common`、`Packages/LikesProgramSDK`、`Install/` 下的 SDK 目录仍是空骨架） |

顶层 `make` 现在就能产出可启动的 `Out/LikesProgram.iso` 与 `Out/LikesProgram.hdd`。Stub 已由 `Packages/Baleen/Stub` 真实构建；BootCore、EFI 引导镜像、Ext4 系统卷与布局描述尚未实现，构建时由 `Tools/Bin/MakePayloads` 生成的占位件顶上，只为把引导链跑通；每个载荷的实际来源都在构建日志里逐项打印。

## 快速开始

构建需要 GNU make、NASM 3.x、g++（C++20），以及生成占位件的 `mkfs.vfat` / `mkfs.ext4`；启动与测试需要 QEMU（可选 Bochs，UEFI 用例需要 OVMF）。

```sh
make                # 构建 Out/LikesProgram.iso 与 Out/LikesProgram.hdd
make iso            # 只构建 ISO（make hdd 只构建磁盘镜像）
make run            # 启动镜像：默认 hdd + qemu + 直接挂载 + BIOS，不开窗口
make run-win        # 同上，但开本地窗口（QEMU 用 gtk/sdl，Bochs 用 wx）
make run iso qemu built uefi    # 例：光盘 + QEMU + UEFI
make compile-db     # 扫描各包写出 compile_commands.json，供 clangd 与 C/C++ 扩展取用
Tools/Bin/CheckIpl --ipl-dir Packages/Baleen/Ipl/Out/Bin # 手工验证 IPL，不依赖 Test 目录
make help           # 全部目标与可用变量
```

`make run` 不开窗口，串口输出打在终端上（QEMU 用 `-display none`，Bochs 画面走 VNC，从 5900 起）；要在本地窗口里看画面就用 `make run-win`，它接受与 `run` 完全相同的四个位置参数。窗口路径要求机器上有图形会话：QEMU 需要装了 `qemu-system-gui` 的 gtk/sdl 后端，Bochs 用 wx 显示库（配置界面同为 wx，否则 Bochs 会退回 rfb）。四个位置参数、载荷变量与其余目标见顶层 [Makefile](Makefile) 头部注释与 `make help`。

编辑器配置在 `.vscode`：常用构建与运行操作做成了任务，clangd 与 C/C++ 扩展都读根目录的 `compile_commands.json`；该文件含本机绝对路径、不进 Git，Makefile 改过之后重跑 `make compile-db` 刷新。

## 文档索引

规格文档位于 `Docs/Specs`，是各领域的正式定义：

| 文档 | 内容 |
| --- | --- |
| [Baleen 引导器](Docs/Specs/Baleen/README.md) | 引导器职责边界、引导模式支持梯队、硬件与介质范围、卷定位与内核路径、装载交接、引导选择器、产物清单 |
| [内核启动与固件边界](Docs/Specs/内核启动与固件边界.md) | 内核入口契约、固件服务的失效边界、第三方引导方式的接入条件 |
| [内核架构与自举闭包](Docs/Specs/内核架构与自举闭包.md) | 内核组成、`LINK_MODE` 与源码边界、自举闭包与内置必备基线、外置能力归属、镜像体积口径 |
| [输出通道与Print](Docs/Specs/输出通道与Print.md) | 引导期与内核共用的文本输出：前端与目标契约、各阶段装配、重定向与缺省行为 |
| [CPU 描述符表](Docs/Specs/CPU描述符表.md) | GDT/IDT 的装载机制、Baleen 与内核的分工、32 位与长模式的字节差异 |
| [内核模块与驱动清单](Docs/Specs/内核模块与驱动清单.md) | 内核模块、驱动、SDK 与契约层的名称、交付形态和职责 |
| [模块与驱动装载](Docs/Specs/模块与驱动装载.md) | 模块与驱动的装载流程、执行上下文、归属登记与热替换边界 |
| [启动介质与文件系统](Docs/Specs/启动介质与文件系统.md) | 启动介质与访问链、ISO 的三种投递方式、必备文件系统、阶段切换与写入启用 |
| [内核集更新与回滚](Docs/Specs/内核集更新与回滚.md) | 内核集构成、试启动状态机、确认与失败处理、各类更新对象的生效边界 |
| [用户空间与常驻服务](Docs/Specs/用户空间与常驻服务.md) | 用户态程序与常驻服务清单、安装包与安装事务、构建期配套工具 |
| [后缀设计](Docs/Specs/后缀设计.md) | 容器后缀的语义、总表与命名规则 |
| [ABI 兼容与外部契约](Docs/Specs/ABI兼容与外部契约.md) | 内核集内部与对外边界的兼容规则：引导交接契约、DriveSDK、AppSDK |

各子项目与工具目录的 README 描述各自的源码、构建与产物：

- [Baleen 子项目](Packages/Baleen/README.md)
- [一级引导 Ipl](Packages/Baleen/Ipl/README.md)
- [MakeIso：ISO / 混合镜像组装器](Tools/Build/MakeIso/README.md) 、[MakeHdd：磁盘镜像组装器](Tools/Build/MakeHdd/README.md)
- [Tools/Build/Common：宿主工具共享代码](Tools/Build/Common/README.md)

## 授权

Apache License 2.0，见 [LICENSE](LICENSE)。
