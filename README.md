# LikesProgramOS

从引导与内核起步，理解操作系统怎样工作，并把这条链路自己实现出来。

LikesProgramOS 是一个基于 ASM 与 C++20 实现的 x86_64 操作系统项目。它自己定义从固件环境到用户空间的整条链路：引导器在 BIOS/UEFI 环境里识别介质、定位系统卷、装载内核并完成交接；内核由 `LikesProgram.os` 与发行配置选择的 `.osm` 模块构成；再往上是由 Init 编排的用户空间与常驻服务。每一层都有正式规格（见[文档索引](#文档索引)），规格是设计的正式定义，本 README 只做总览与索引。

项目由两个平级子项目、一套宿主侧构建工具与一份规格文档组成：

| 组成 | 目录 | 职责 |
| --- | --- | --- |
| **Baleen**（引导器） | `Packages/Baleen` | 从不同硬件找到指定 Ext4 文件系统的指定路径，装载系统内核并完成交接 |
| **LikesProgramOS**（系统本体） | `Packages/LikesProgramOS` | 操作系统本体：`LikesProgram.os` 内核 + 发行配置选择的 `.osm` 模块 |
| 宿主侧工具 | `Tools/Build`、`Tools/Test`、`Tools/Maintain` | 构建期工具（MakeImage、TestInstaller、MakePayloads、PackImage、MakeKeys、MakeManifest、SignManifest、MakeRevocation）、测试与回归工具（CheckIpl、CheckStub、CheckStorage、CheckExt4、CheckCrypto、CheckSelect、CheckMemory、CheckRevocation、RunBootCase）与维护工具（FixBootState：引导状态区域的报告、受控恢复与人工接受），共享代码在 `Tools/Common` 与 `Tools/Build/Common`，可执行文件统一在 `Tools/Bin` |
| 规格文档 | `Docs/Specs` | 各领域的正式定义；与本 README 不一致时以规格为准 |

## 设计取向

1. **引导器只做引导器的事。** Baleen 负责固件环境里的介质识别、卷定位、内核镜像装载与机器状态准备，不装载模块、不建立运行期设备模型。引导模式覆盖 BIOS 与 UEFI（含无 CSM 的纯 UEFI），并把 coreboot、虚拟化直启等列为计划支持；介质覆盖内部硬盘、USB 移动存储与 ISO（写入 U 盘或移动硬盘、虚拟机挂载）。范围与边界见 [Baleen 引导器](Docs/Specs/Baleen/README.md)。
2. **进入 `KernelMain` 的第一步让固件失效。** 不是假定固件已经卸载：内核在第一步退出固件启动服务，此后固定运行模式——不降级、不依赖固件运行期服务，全部使用自身实现。见[内核启动与固件边界](Docs/Specs/内核启动与固件边界.md)。
3. **内核集整套构建、整套更新。** 一次构建的 `.os`、所选 `.osm` 与构建清单共享 `BuildId`，不接受跨构建混装；选版、试启动、确认和回滚以整个内核集为单位。见[内核集更新与回滚](Docs/Specs/内核集更新与回滚.md)。
4. **必备能力内置。** USB（xHCI、USB 2.0/3.x、BOT/UAS）、Ext4、ISO9660、FAT32 与统一设备模型随 `.os` 提供，退出固件启动服务后即可读取系统卷、装载模块并报告错误。见[内核架构与自举闭包](Docs/Specs/内核架构与自举闭包.md)。
5. **系统卷是 Ext4 的指定路径。** 磁盘上是分区里的卷，ISO 上是卷内的镜像文件；引导器按路径取内核，内核自己再校验一次卷标识与布局。见[启动介质与文件系统](Docs/Specs/Common/启动介质与文件系统.md)。

## 当前状态

引导链与构建链已经能跑通，内核与用户空间尚未实现：

| 部分 | 状态 |
| --- | --- |
| Baleen 一级引导（`Packages/Baleen/Ipl`） | 已实现：硬盘 / USB-HDD 与 El Torito 光盘两种形态，NASM 汇编，构建含尺寸硬约束检查（NASM 3.01 下 `make check` 通过）；磁盘入口按 `AH=48` 接受 512 / 4096 逻辑扇区，软盘与 USB-FDD 明确拒绝；源码、产物与运行期契约见 [Ipl 说明](Packages/Baleen/Ipl/README.md) |
| 宿主构建工具（`Tools/Build`） | 已实现：通用镜像制作器 `MakeImage` 产出可刻录、可写盘、可挂载的 `LikesProgram.iso`，测试安装器 `TestInstaller` 产出可写入内置硬盘 / 移动硬盘 / U 盘的整盘镜像，`MakePayloads` 生成开发占位载荷，以及签名链上的 `MakeKeys`（开发期 Ed25519 密钥与内嵌信任锚源）、`MakeManifest`（内核集清单）、`SignManifest`（清单签名）与 `MakeRevocation`（签名吊销记录）；构建不依赖测试目录；用法见[工具 README](Tools/Build/MakeImage/README.md) |
| 宿主测试与回归工具（`Tools/Test`） | 已实现：`CheckIpl`（IPL 静态布局与 55 例启动回归）、`CheckStub`（Stub 阶段 28 例，含交权块五个服务与写盘往返；`--stack-probe` / `--window-probe` 两种探针模式）、`CheckStorage`（S1 存储抽象与 32 位双编译）、`CheckExt4`（Ext4 只读实现、目录遍历与块设备适配器）、`CheckCrypto`（SHA-512 与 Ed25519 的标准向量、两侧互验、信任锚轮换窗口与目标侧链接检查）、`CheckSelect`（选版与引导状态记录：四态决策、八槽环形与代次、写入门槛、逐字节截断与撕裂写演练、介质策略位与持久化边界、维护动作）、`CheckMemory`（内存图可用性判定与弹跳窗口搜索）、`CheckRevocation`（吊销记录的结构、签名、签名者自吊销、位图与并集、系统卷读取三态、真实清单验证的吊销与下限判定、单调性、记录版本只有 1 且拒绝其它版本）、`CheckEfiBlockDevice`（UEFI 块设备适配：绑定事实、参数与容量边界、介质更换、只读、刷新映射与老协议版本、对齐中转与异常边界）与运行辅助工具 `RunBootCase`；测试发现的缺陷与修复流水见[缺陷与修复记录](Docs/Test/缺陷与修复记录.md)。全部手工执行，不进入默认构建，`make test-tools` 只负责编译。`make check-all` 把这一层、十档启动矩阵与引导状态演练收成一条命令，见[快速开始](#快速开始) |
| 宿主维护工具（`Tools/Maintain`） | 已实现：`FixBootState`（引导状态区域的报告、受控恢复、防回滚的人工接受，以及代执行两个运行期写入方 `--publish` / `--confirm`，见[引导状态记录](Docs/Specs/Baleen/引导状态记录.md)第四、八节）；手工执行，`make maintain-tools` 只负责编译 |
| Baleen Stub（`Packages/Baleen/Stub`） | 阶段性实现：切保护模式、开 A20、建立运行期 GDT/IDT、异常诊断，BIOS 服务经实模式弹跳提供扇区探测、E820 内存图与低地址读盘；并按 [Stub 说明](Packages/Baleen/Stub/README.md)第二节的契约读取介质上的 `CoreDescriptor`、校验装载区、把 `BaleenCore` 装到 1MiB 并经交权块交给它（QEMU 已验证 HDD / ISO / USB 到 BaleenCore，Bochs 已验证 ATA HDD / ATAPI 光盘；两者均使用本机 SeaBIOS）；`CheckStub` 回归覆盖成功路径、交权块五个服务（含写盘服务的写后读回与只读介质返回 `0`）与失败注入 |
| BaleenCore（`Packages/Baleen/Core`） | 阶段性实现：入口与诊断、交权块与镜像事实核对、CPU 快照、运行期段表与中断表、环境事实播报、布局描述读取与校验、系统卷挂载（Ext4 只读，含卷标识核对）、内核集选版（读引导状态记录、按状态机选目标集）与清单签名验证（Ed25519 与内嵌信任锚、`BuildId` 一致、防回滚比较、逐产物摘要）、按选中集的路径定位内核镜像并读取校验[内核镜像数据](Docs/Specs/Baleen/内核镜像数据.md)与整幅摘要（含装载区落点检查）、装载到装载地址、构造 `BootInfo` 与恒等映射页表、切长模式进入内核（QEMU 已验证 HDD / CD / USB 512e / NVMe 4Kn 到内核阶段边界，Bochs 已验证 ATA HDD / ATAPI 光盘；见 [BaleenCore 说明](Packages/Baleen/Core/README.md)）；引导状态区域的写入经 Stub 的写盘服务完成（交权块），写缓存由 BaleenCore 经 AHCI 直接刷新（可刷新端口存在时下发 `FLUSH CACHE`，光盘 / USB / NVMe 按“无可用原语”通过），只读介质上写入失败并按降级路径直启，记录与清单的规格见 [引导状态记录](Docs/Specs/Baleen/引导状态记录.md) 与 [内核集清单与签名](Docs/Specs/内核集清单与签名.md) |
| Baleen UEFI 侧（`Packages/Baleen/Uefi`） | 已实现，与 BIOS 路径同进度：`BOOTX64.EFI` 从固件加载后取介质块设备（分区句柄沿设备路径上溯到整盘）、读并校验布局描述、挂 Ext4 系统卷、列内核集目录、选版（引导状态记录四态决策）、验清单签名与产物摘要（含防回滚与吊销）、定位并装载内核镜像、取固件内存图、调 `ExitBootServices` 退出启动服务，最后按内核入口契约把 `BootInfo` 交给内核；ESP 镜像 `Efi.img` 由本包产出，顶层组装器优先取它。与 BIOS 侧的差别只在载体（块设备协议 / 固件内存图 / 固件控制台与 `FlushBlocks`），判据与编号两侧一致；`BOOTIA32.EFI` 是 32 位固件上的提示件。见 [UEFI 侧说明](Packages/Baleen/Uefi/README.md) |
| 内核侧 Core（`Packages/LikesProgramOS/Core`） | 阶段性实现：产出内核映像 `LikesProgram.os`——文件起点是内核镜像数据，`0x80` 起是长模式入口；入口、`BootInfo` 尺寸与版本核对、固件启动服务失效、CPU 事实、描述符表（GDT/TSS/IDT，异常桩与 IST）、引导页表与直映射、页帧分配器已可用（见 [内核侧 Core 说明](Packages/LikesProgramOS/Core/README.md)）；入口已由 BaleenCore 的交接路径接通，设备模型、模块装载、调度与 Init 尚未实现 |
| 内核本体其余部分、驱动、用户空间、SDK | 尚未实现（`Packages/LikesProgramSDK` 等目录仍是空骨架）；Baleen 与内核共用的基础件已在 `Packages/Common`：Print 前端、平台原语、[存储抽象](Packages/Common/README.md)、`BootInfo` 与内核镜像数据契约 |

顶层 `make` 现在就能产出可启动的 `Out/LikesProgram.iso` 与 `Out/LikesProgram.hdd`。Stub、BaleenCore、UEFI 侧与内核侧 Core 已由各自包真实构建，系统卷由 `Packages/LikesProgramOS` 从 `Static/` 卷根用 `mke2fs -d` 生成，卷内目录树见[系统卷布局](Docs/Specs/Common/系统卷布局.md)，内核集落在 `/LikesProgram/System/KernelSets/<BuildId>/`（`LikesProgram.os` 与签名清单 `Manifest.mft`，卷内路径写进 `Out/KernelPath`）；布局描述由组装器按[布局描述](Docs/Specs/Baleen/布局描述.md)生成并回填，内核路径取该文件给出的卷内落点；清单与签名由 `MakeManifest` / `SignManifest` 用仓库外的开发期密钥生成，验签用的信任锚由 `MakeKeys` 从同一密钥导出并编进 BaleenCore 与 UEFI 侧的 `BOOTX64.EFI`（两个固件环境各编同一份锚源，密钥与签名流程见[内核集清单与签名](Docs/Specs/内核集清单与签名.md)）；EFI 引导镜像是真实产物（`Packages/Baleen/Uefi` 产出 `Efi.img`，内含 `BOOTX64.EFI`），占位件只在它缺失时顶上；每个载荷的实际来源都在构建日志里逐项打印。BIOS 路径与 UEFI 路径都已经从受支持介质启动进入内核：BIOS 侧经 IPL → Stub → BaleenCore，BaleenCore 按引导状态记录选版、验证所选集的清单签名与产物摘要，再读取并校验内核镜像，把它装到装载地址、构造 `BootInfo` 与恒等映射页表，切长模式跳到内核入口；UEFI 侧由 `BOOTX64.EFI` 在固件环境里走完同样的选版、验签与装载，调 `ExitBootServices` 退出启动服务后按同一份 `BootInfo` 契约交给内核。`BootInfo` 同时带上**启动来源**（布局描述偏移、系统卷位置与卷标识、引导状态区域、介质容量、内核路径、启动设备线索与引导器占用区间，见 [Baleen 引导器](Docs/Specs/Baleen/README.md) 6.1）：内核由此知道这次是从哪个介质、哪个卷、哪条路径起来的，运行期更新才能把新内核集写进同一个卷、把状态记录写回同一处区域，而不到设备里猜。

## 快速开始

构建需要 GNU make、NASM 3.x、g++（C++20），以及生成占位件与系统卷的 `mkfs.vfat` / `mke2fs`（e2fsprogs，需要支持 `-d` 的版本）；启动与测试需要 QEMU（可选 Bochs，UEFI 用例需要 OVMF）。

构建全程把警告当错误：C++ 用 `-Wall -Wextra -Werror`（宿主工具另加 `-Wpedantic`），NASM 用 `-Werror`，链接用 `--fatal-warnings`。新增警告会直接让构建失败，不会被跳过。

```sh
make                # 构建 Out/LikesProgram.iso 与 Out/LikesProgram.hdd
make iso            # 只构建 ISO（make hdd 只构建磁盘镜像）
make check-all      # 一条命令跑完宿主侧 8 个检查工具，输出“通过/失败 + 用例计数”表
make check-all WITH_BOOT=1   # 再跑十档启动矩阵（BIOS 六档 + UEFI 四档），每档三列：进内核 / 刷新路径 / 诊断编号
                            # 之后再跑引导状态演练：空区域 / 槽位损坏 / 候选失败 / 写中途断电（撕裂）/ 降级策略位 /
#   设备级写缓存（刷新覆盖核验、无原语边界、丢缓存重来），逐档核对期望
make check-all ACCEPT=1     # 工业验收：加失败注入（整链逐层弄坏）、NVMe 与 4Kn 介质/控制器、引导状态机多轮（约 1 分 40 秒）
make run            # 启动镜像：默认 hdd + qemu + 直接挂载 + BIOS，不开窗口
make run-win        # 同上，但开本地窗口（QEMU 用 gtk/sdl，Bochs 用 wx）
make run iso qemu built uefi    # 例：光盘 + QEMU + UEFI
make compile-db     # 扫描各包写出 compile_commands.json，供 clangd 与 C/C++ 扩展取用
make test-tools     # 只构建测试与回归工具（Tools/Test），手工执行，不进入默认构建
Tools/Bin/CheckIpl --ipl-dir Packages/Baleen/Ipl/Out/Bin # 手工验证 IPL，不依赖 Test 目录
Tools/Bin/CheckStub --stub-dir Packages/Baleen/Stub/Out/Bin --ipl-dir Packages/Baleen/Ipl/Out/Bin # 手工验证 Stub 与交权块五个服务（含写盘）
Tools/Bin/CheckStorage # 手工验证 S1 存储抽象：用例、32 位双编译与目标侧链接检查
Tools/Bin/CheckExt4 # 手工验证 Ext4 只读实现与块设备适配器：真实卷夹具与故障注入
Tools/Bin/CheckCrypto # 手工验证密码学与信任锚轮换窗口：标准向量、两侧互验与锚生成
Tools/Bin/CheckSelect # 手工验证选版与引导状态记录：四态决策、八槽环形、写入门槛与维护动作
Tools/Bin/CheckMemory # 手工验证内存图判定与弹跳窗口搜索：合并规则、边界与找不到窗口
Tools/Bin/CheckRevocation # 手工验证签名吊销记录与引导状态区单调纪元：结构、验签、自吊销、并集与清单判定
Tools/Bin/MakeRevocation --help # 生成并签名吊销记录（LPRV）；用法与约束见工具 README
make maintain-tools # 只构建维护工具（Tools/Maintain）
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --report # 报告引导状态区域；另有 --repair、--clear、--accept-seen
make -C Packages/LikesProgramOS check   # 复核内核映像与系统卷（内核镜像数据、摘要、入口）
make help           # 全部目标与可用变量
```

`make run` 不开窗口，串口输出打在终端上（QEMU 用 `-display none`，Bochs 画面走 VNC，从 5900 起）；要在本地窗口里看画面就用 `make run-win`，它接受与 `run` 完全相同的四个位置参数。窗口路径要求机器上有图形会话：QEMU 需要装了 `qemu-system-gui` 的 gtk/sdl 后端，Bochs 用 wx 显示库（配置界面同为 wx，否则 Bochs 会退回 rfb）。四个位置参数、载荷变量与其余目标见顶层 [Makefile](Makefile) 头部注释与 `make help`。

`make check-all` 的产物落在 `Out/check/`（`summary.txt` 与逐例日志）。**那是构建产物，不是证据目录**：`make clean` 与 `make clean-all` 会连同 `Out/` 一起删掉它。需要长期留存、可复核的验证记录，用 `CHECK_ALL_ARGS="--out <目录>"` 指到 `Out/` 之外，或及时把 `summary.txt` 与日志拷走。

BIOS 磁盘入口接受 512 / 4096 字节逻辑扇区，软盘与 USB-FDD/ZIP 不在范围。**引导区（描述符区与载荷）的位置字段以 512 字节为基准，装载方按运行期探测到的单位换算**，因此同一份 `LikesProgram.iso` 可直接刻录到光盘、写入 512e 或 4Kn 的 U 盘 / 移动硬盘；**ESP 双份、分区项起点固定写 LBA 2048**：512e 固件读到 1 MiB、4Kn 固件读到 8 MiB，两处各有一份同样的 FAT16 卷，所以同一份镜像的 UEFI 路径在两种设备上都通；Ext4 那一项（`0x83`）仍按 `DISK_SECTOR_BYTES` 生成，内核对系统卷用布局描述定位、不依赖它。各类启动模式的当前支持状态见[启动介质与文件系统](Docs/Specs/Common/启动介质与文件系统.md) 1.1。

编辑器配置在 `.vscode`：常用构建与运行操作做成了任务，clangd 与 C/C++ 扩展都读根目录的 `compile_commands.json`；该文件含本机绝对路径、不进 Git，Makefile 改过之后重跑 `make compile-db` 刷新。

## 文档索引

规格文档位于 `Docs/Specs`，按主题分组：`Baleen/` 收录引导器专属规格，`Common/` 收录跨模块通用规格（介质与文件系统、文件系统抽象与 VFS、ABI 兼容与外部契约），其余为各领域规格；面向实现者的手册在 `Docs/Manuals`：

| 文档 | 内容 |
| --- | --- |
| [Baleen 引导器](Docs/Specs/Baleen/README.md) | 引导器职责边界、引导模式支持梯队、硬件与介质范围、卷定位与内核路径、装载交接、引导选择器、产物清单 |
| [布局描述](Docs/Specs/Baleen/布局描述.md) | `BaleenLayout.bin` 的字节表、写入位置与校验，引导状态区域，以及模板与组装器的回填职责 |
| [内核镜像数据](Docs/Specs/Baleen/内核镜像数据.md) | 内核镜像文件起点那份数据的字节表、摘要覆盖、BaleenCore 与内核两侧的校验顺序，以及装载交接的当前边界 |
| [引导状态记录](Docs/Specs/Baleen/引导状态记录.md) | 引导状态区域的记录字节表、八槽环形布局、写入顺序与掉电语义，每候选的尝试次数与防回滚记录 |
| [缺陷与修复记录](Docs/缺陷与修复记录.md) | 测试发现的缺陷流水：现象、影响、根因、修法与回归网；刻意不修的与仍未覆盖的验证缺口也记在这里 |
| [系统卷布局](Docs/Specs/Common/系统卷布局.md) | 系统卷的目录树与职责边界：系统目录、程序目录与用户目录，命名与放置规则，以及 `Static/` 卷根的组装方式 |
| [内核启动与固件边界](Docs/Specs/内核启动与固件边界.md) | 内核入口契约、固件服务的失效边界、第三方引导方式的接入条件 |
| [内核架构与自举闭包](Docs/Specs/内核架构与自举闭包.md) | 内核组成、`LINK_MODE` 与源码边界、自举闭包与内置必备基线、外置能力归属、镜像体积口径 |
| [输出通道与Print](Docs/Specs/输出通道与Print.md) | 引导期与内核共用的文本输出：前端与目标契约、各阶段装配、重定向与缺省行为 |
| [CPU 描述符表](Docs/Specs/CPU描述符表.md) | GDT/IDT 的装载机制、Baleen 与内核的分工、32 位与长模式的字节差异 |
| [内核模块与驱动清单](Docs/Specs/内核模块与驱动清单.md) | 内核模块、驱动、SDK 与契约层的名称、交付形态和职责 |
| [模块与驱动装载](Docs/Specs/模块与驱动装载.md) | 模块与驱动的装载流程、执行上下文、归属登记与热替换边界 |
| [启动介质与文件系统](Docs/Specs/Common/启动介质与文件系统.md) | 启动介质与访问链、ISO 的三种投递方式、必备文件系统、阶段切换与写入启用 |
| [文件系统抽象与 VFS](Docs/Specs/Common/文件系统抽象与VFS.md) | Baleen 与内核共用的块设备、偏移视图、统一路径索引与文件系统接口；共用边界与阶段划分 |
| [内核集更新与回滚](Docs/Specs/内核集更新与回滚.md) | 内核集构成、试启动状态机、确认与失败处理、各类更新对象的生效边界 |
| [内核集清单与签名](Docs/Specs/内核集清单与签名.md) | 内核集的卷内落点、清单字节表、签名与信任锚、密钥轮换与撤销、防回滚与试启动身份传递 |
| [用户空间与常驻服务](Docs/Specs/用户空间与常驻服务.md) | 用户态程序与常驻服务清单、安装包与安装事务、构建期配套工具 |
| [后缀设计](Docs/Specs/后缀设计.md) | 容器后缀的语义、总表与命名规则 |
| [ABI 兼容与外部契约](Docs/Specs/Common/ABI兼容与外部契约.md) | 内核集内部与对外边界的兼容规则：引导交接契约、DriveSDK、AppSDK |
| [镜像制作器与安装器实现手册](Docs/Manuals/镜像制作器与安装器实现手册.md) | 产出可引导介质的实现指南：描述符区格式、需要写回的信息与位置、建议流程，以及通用镜像制作器与测试安装器的定位与能力边界 |

各子项目与工具目录的 README 描述各自的源码、构建与产物：

- [Baleen 子项目](Packages/Baleen/README.md)
- [一级引导 Ipl](Packages/Baleen/Ipl/README.md)
- [系统本体 LikesProgramOS](Packages/LikesProgramOS/README.md) ・ [内核侧 Core](Packages/LikesProgramOS/Core/README.md)
- [MakeImage：通用镜像制作器](Tools/Build/MakeImage/README.md) 、[TestInstaller：测试安装器](Tools/Build/TestInstaller/README.md)
- [Tools/Common：宿主工具的跨分类共享代码](Tools/Common/README.md) ・ [Tools/Build/Common：构建期工具共享代码](Tools/Build/Common/README.md)
- [CheckIpl：IPL 回归工具](Tools/Test/CheckIpl/README.md) 、[CheckStub：Stub 回归工具](Tools/Test/CheckStub/README.md) 、[CheckStorage：S1 存储抽象检查工具](Tools/Test/CheckStorage/README.md) 、[CheckExt4：Ext4 只读实现检查工具](Tools/Test/CheckExt4/README.md) 、[CheckCrypto：密码学与信任锚检查工具](Tools/Test/CheckCrypto/README.md) 、[CheckSelect：选版与引导状态记录检查工具](Tools/Test/CheckSelect/README.md) 、[CheckMemory：内存图与弹跳窗口检查工具](Tools/Test/CheckMemory/README.md) 、[CheckRevocation：吊销记录与单调纪元检查工具](Tools/Test/CheckRevocation/README.md) 、[RunBootCase：启动用例运行工具](Tools/Test/RunBootCase/README.md)
- [FixBootState：引导状态维护工具](Tools/Maintain/FixBootState/README.md)

## 授权

Apache License 2.0，见 [LICENSE](LICENSE)。
