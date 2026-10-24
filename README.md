# LikesProgramOS

从引导与内核起步，理解操作系统怎样工作，并把这条链路自己实现出来。

LikesProgramOS 是一个基于 ASM 与 C++20 实现的 x86_64 操作系统项目。它自己定义从固件环境到用户空间的整条链路：引导器在 BIOS/UEFI 环境里识别介质、定位系统卷、装载内核与引导闭包并完成交接；内核由 `LikesProgram.os` 与发行配置选择的 `.osm` 模块构成；再往上是由 Init 编排的用户空间与常驻服务。每一层都有正式规格（见[文档索引](#文档索引)），规格是设计的正式定义，本 README 只做总览与索引。

项目由两个平级子项目、一套宿主侧构建工具与一份规格文档组成：

| 组成 | 目录 | 职责 |
| --- | --- | --- |
| **Baleen**（引导器） | `Packages/Baleen` | 从不同硬件找到指定 Ext4 文件系统的指定路径，装载系统内核与清单要求预装的引导闭包并完成交接 |
| **LikesProgramOS**（系统本体） | `Packages/LikesProgramOS` | 操作系统本体：`LikesProgram.os` 内核 + 发行配置选择的 `.osm` 模块；引导闭包模块在 `Packages/KernelModules` |
| 宿主侧工具 | `Tools/Build`、`Tools/Test`、`Tools/Maintain` | 构建期工具（MakeContract、PackModule、MakeImage、TestInstaller、MakePayloads、PackImage、MakeKeys、MakeManifest、SignManifest、MakeRevocation）、测试与回归工具（CheckIpl、CheckStub、CheckStorage、CheckExt4、CheckCrypto、CheckSelect、CheckMemory、CheckRevocation、CheckEfiBlockDevice、CheckOsm、CheckPreload、CheckRevision、CheckHandoff、CheckScreen、CheckObjects、CheckContainer、CheckRegistry、CheckDependency、CheckRelocation、CheckLoader、CheckDevModel、CheckVfs、CheckExt4Module、CheckNvme、CheckUsbCore、CheckXhci、CheckUsbHub、CheckUsbStorage、CheckIso9660、CheckFat32 与运行辅助 RunBootCase）与维护工具（FixBootState：引导状态区域的报告、受控恢复与人工接受），共享代码在 `Tools/Common` 与 `Tools/Build/Common`，可执行文件统一在 `Tools/Bin` |
| 规格文档 | `Docs/Specs` | 各领域的正式定义；与本 README 不一致时以规格为准 |

## 设计取向

1. **引导器只做引导器的事。** Baleen 负责固件环境里的介质识别、卷定位、内核镜像与引导闭包装载、机器状态准备，不解析模块容器内部、不重定位、不执行模块，也不建立运行期设备模型。引导模式覆盖 BIOS 与 UEFI（含无 CSM 的纯 UEFI），并把 coreboot、虚拟化直启等列为计划支持；介质覆盖内部硬盘、USB 移动存储与 ISO（写入 U 盘或移动硬盘、虚拟机挂载）。范围与边界见 [Baleen 引导器](Docs/Specs/Baleen/README.md)。
2. **进入 `KernelMain` 的第一步让固件失效。** 不是假定固件已经卸载：内核在第一步退出固件启动服务，此后固定运行模式——不降级、不依赖固件运行期服务，全部使用自身实现。见[内核启动与固件边界](Docs/Specs/内核启动与固件边界.md)。
3. **内核集整套构建；主版本整集切换，模块按修订更新。** `BuildId` 是 `.os` 发布线身份：由 `.os` 内容与集内路径派生，模块补丁不改变它。`.os` 变化（主版本升级）产生新 `.os` 与新的 `KernelSets/<BuildId>/` 目录，按整集选版、试启动、确认与回滚；同一发布线内的模块按修订更新，新模块文件、新 `Manifest.mft` 与新 `Modules.list` 落进同一集目录，重启生效、整修订回退，旧修订默认保留 1 个。模块主版本必须与 `.os` 的 `ModuleMajor` 硬相等，跨主版本一律拒绝；`Modules.list` 是内核侧书写的完整模块清单：Baleen 交权前只预装其中命中引导闭包期望集的成员，其余条目由内核挂卷后装载。见[内核集清单与签名](Docs/Specs/内核集清单与签名.md)、[模块修订与更新](Docs/Specs/模块修订与更新.md)与[内核集更新与回滚](Docs/Specs/内核集更新与回滚.md)。
4. **内核核心与引导闭包分层。** `.os` 只保留内核核心（平台初始化、内存、对象、调度、中断、输出与诊断、容器与模块装载、配置与清单解析、启动状态与契约层）；USB（xHCI、USB 2.0/3.x、BOT/SCSI，UAS 属阶段 3）、启动设备驱动、Ext4、ISO9660、FAT32 与统一设备模型组成引导闭包 `.osm`，由 Baleen 在交权前预装、内核入口校验后初始化，内核再挂载系统卷、读取清单并装载运行期模块。见[内核架构与自举闭包](Docs/Specs/内核架构与自举闭包.md)与[内核模块容器](Docs/Specs/内核模块容器.md)。
5. **系统卷是 Ext4 的指定路径。** 磁盘上是分区里的卷，ISO 上是卷内的镜像文件；引导器按路径取内核，内核自己再校验一次卷标识与布局。见[启动介质与文件系统](Docs/Specs/Common/启动介质与文件系统.md)。

## 当前状态

P5（引导闭包与模块装载，阶段 0–2）已于 2026-10-22 完成收尾：M0–M7 与后续 M0 重开项 M8 全部落地，构建链、引导器、内核装载器与存储栈均已接通，启动链路能从受支持介质进入内核并完成引导闭包装载、系统卷挂载、清单读取与运行期 `.osm` 装载。完成判定按验证门与证据，不按文档措辞；P5 范围之外的阶段 3/4 能力（调度 / IRQ / 异步、Init 确认点、Updater 事务、Ext4 写入等）尚未实现，不属于本阶段交付。

| 部分 | 状态 |
| --- | --- |
| Baleen 一级引导（`Packages/Baleen/Ipl`） | 已实现：硬盘 / USB-HDD 与 El Torito 光盘两种形态，NASM 汇编，构建含尺寸硬约束检查（NASM 3.01 下 `make check` 通过）；磁盘入口按 `AH=48` 接受 512 / 4096 逻辑扇区，软盘与 USB-FDD 明确拒绝；源码、产物与运行期契约见 [Ipl 说明](Packages/Baleen/Ipl/README.md) |
| 宿主构建工具（`Tools/Build`） | 已实现：契约生成器 `MakeContract`（`Interfaces.def` / `BootClosure.def` → 生成头与槽表）与模块打包器 `PackModule`；通用镜像制作器 `MakeImage` 产出可刻录、可写盘、可挂载的 `LikesProgram.iso`，测试安装器 `TestInstaller` 产出可写入内置硬盘 / 移动硬盘 / U 盘的整盘镜像，`MakePayloads` 生成开发占位载荷，以及签名链上的 `MakeKeys`（开发期 Ed25519 密钥与内嵌信任锚源）、`MakeManifest`（内核集清单）、`SignManifest`（清单签名）与 `MakeRevocation`（签名吊销记录）；`PackImage` 复核内核镜像数据结构与身份；构建不依赖测试目录 |
| 宿主测试与回归工具（`Tools/Test`） | 已实现：30 个检查工具（覆盖引导链、契约与模块、内存与页表、存储与文件系统、设备模型、USB 链、NVMe、ISO9660、FAT32、选版与引导状态）加运行辅助工具 `RunBootCase`；宿主用例合计 1000 例，**宿主段带固定门禁**（工具数与用例数不等于 30 / 1000 直接判失败，静默删例不会再全绿）；停机注入档用定长观察窗口跑完再断言“全程无内核标记”，不再以期望编号当停止条件。`make check-all` 把这一层、12 档启动矩阵（BIOS / UEFI × HDD / ISO / USB、UEFI NVMe 4Kn、uefi32 提示档与 Bochs 两档）与引导状态演练 19 档（含同集两修订切换 3 档）收成一条命令；`WITH_BOOT=1` 另跑运行期模块夹具 4 档，G7 总检查项 1035；`ACCEPT=1` 另跑 G7 三类失败注入载体 8 档、介质与控制器、4Kn 状态推进 5 档（状态演练共 24 档）、吊销与防回滚等扩展验证，G7 总检查项 1048。测试发现的缺陷与修复流水见[缺陷与修复记录](Docs/Test/缺陷与修复记录.md) |
| 宿主维护工具（`Tools/Maintain`） | 已实现：`FixBootState`（引导状态区域的报告、受控恢复、防回滚的人工接受，以及代执行两个运行期写入方 `--publish` / `--confirm`，见[引导状态记录](Docs/Specs/Baleen/引导状态记录.md)第四、八节）；手工执行，`make maintain-tools` 只负责编译 |
| Baleen Stub（`Packages/Baleen/Stub`） | 已实现：切保护模式、开 A20、建立运行期 GDT/IDT、异常诊断，BIOS 服务经实模式弹跳提供扇区探测、E820 内存图与低地址读盘；并按 [Stub 说明](Packages/Baleen/Stub/README.md)第二节的契约读取介质上的 `CoreDescriptor`、校验装载区、把 `BaleenCore` 装到 1MiB 并经交权块交给它（QEMU 已验证 HDD / ISO / USB 到 BaleenCore，Bochs 已验证 ATA HDD / ATAPI 光盘；两者均使用本机 SeaBIOS）；`CheckStub` 回归覆盖成功路径、交权块五个服务（含写盘服务的写后读回与只读介质返回 `0`）与失败注入 |
| BaleenCore（`Packages/Baleen/Core`） | 已实现：入口与诊断、交权块与镜像事实核对、CPU 快照、运行期段表与中断表、环境事实播报、布局描述读取与校验、系统卷挂载（Ext4 只读，含卷标识核对）、内核集选版（读引导状态记录、按状态机选目标集，含同集修订选择）与清单签名验证（Ed25519 与内嵌信任锚、`BuildId` 一致、防回滚比较、逐产物摘要）、按验证通过的清单 `.os` 条目定位内核镜像（缺省集的布局路径须与它逐字节一致）并读取校验[内核镜像数据](Docs/Specs/Baleen/内核镜像数据.md)与整幅摘要（含装载区落点检查）、装载到装载地址、构造 `BootInfo` 与恒等映射页表、切长模式进入内核（QEMU 已验证 HDD / CD / USB 512e / NVMe 4Kn 到内核阶段边界，Bochs 已验证 ATA HDD / ATAPI 光盘；见 [BaleenCore 说明](Packages/Baleen/Core/README.md)）；从 `Modules.list` 里筛出 `BootClosure.def` 期望集的 13 个引导闭包成员并预装，把清单副本、列表副本与预装记录经 `BootInfo` 交给内核（列表里的运行期条目不在引导期装载）；引导状态区域的写入经 Stub 的写盘服务完成（交权块），写缓存由 BaleenCore 经 AHCI 直接刷新（可刷新端口存在时下发 `FLUSH CACHE`，光盘 / USB / NVMe 按“无可用原语”通过），只读介质上写入失败时按降级路径继续启动（缺省集同样走清单验签），记录与清单的规格见 [引导状态记录](Docs/Specs/Baleen/引导状态记录.md) 与 [内核集清单与签名](Docs/Specs/内核集清单与签名.md) |
| Baleen UEFI 侧（`Packages/Baleen/Uefi`） | 已实现，与 BIOS 路径同进度：`BOOTX64.EFI` 从固件加载后取介质块设备（分区句柄沿设备路径上溯到整盘）、读并校验布局描述、挂 Ext4 系统卷、列内核集目录、选版（引导状态记录四态决策）、验清单签名与产物摘要（含防回滚与吊销）、定位并装载内核镜像与 `Modules.list` 里的引导闭包 `.osm`（先读进固件分配的中转缓冲，退出启动服务后按最终内存图确认固定跨度归系统再拷入，`E-UEFI-86` / `E-UEFI-88`）、取固件内存图（紧贴 `ExitBootServices` 取最终快照，交接用退出后视角）、调 `ExitBootServices` 退出启动服务，最后按内核入口契约把 `BootInfo`（含预装记录）交给内核；ESP 镜像 `Efi.img` 由本包产出，顶层组装器优先取它。与 BIOS 侧的差别只在载体（块设备协议 / 固件内存图 / 固件控制台与 `FlushBlocks`）：判据与编号两侧一致，但本包不链接 `ImageHeader`、不产生 `E-UEFI-40..51`；刷新原语只在介质可写、协议 `REVISION2` 以上且固件未报 `EFI_UNSUPPORTED` 时才声称（只读介质按 `ReadOnly`）；系统卷没有内核时报 `E-UEFI-58` 停在边界。`BOOTIA32.EFI` 是 32 位固件上的提示件。见 [UEFI 侧说明](Packages/Baleen/Uefi/README.md) |
| 内核侧 Core（`Packages/LikesProgramOS/Core`） | 已实现到 M8：产出内核映像 `LikesProgram.os`；入口、`BootInfo` 与内核镜像数据核对、固件启动服务失效、CPU 事实、描述符表（GDT/TSS/IDT，异常桩与 IST）、引导页表与直映射、MMIO 高地址窗口（4 GiB 跨度 + 按需 4 KiB PT）、页帧分配器、交接包 9 步校验（列表与清单 `Type=2` 全集对应、条目分引导闭包与运行期）与缓冲接管、内核核心导出、11 步模块装载编排、13 个引导闭包 `Init`、系统卷挂载与清单读取、光盘档 ISO9660 第二来源核对、运行期 `.osm` 装载；M8 起 `Modules.list` 是内核侧完整模块清单，运行期模块按“列表条目减引导闭包期望集”识别（必需 / 可选按集内路径前缀）；失败统一 `FATAL` + 屏幕 + 等待 + 复位。调度、IRQ 框架、Init 与用户空间尚未实现（见 [内核侧 Core 说明](Packages/LikesProgramOS/Core/README.md)） |
| 模块修订切换与 Updater 事务 | **选择器按修订选择与回退已实现**（M2 起，[引导状态记录](Docs/Specs/Baleen/引导状态记录.md) 2.2 的 `ModuleRevision` 与“当前修订 / 旧快照解析”算法；M6 起有同集两修订夹具：`Rev=2` 直启、`Rev=1` 直启、候选失败回退 `Rev=1`）；**Updater 的模块修订事务**（暂存、依赖求解、保留空间检查、发布 `Pending` 与清理）归后续用户空间阶段。`Modules.list` 的生成（内核侧完整模块清单）、清单登记、主版本门与 `E-CORE-95` / `E-CORE-96` 校验已落地；当前集目录内按 `BuildId` 读当前 `Manifest.mft`，旧修订快照作为回退材料保留 |
| 内核本体其余部分、驱动、用户空间、SDK | **引导闭包 13 个模块已是真实实现**（DevModel、DriverLoader、AtaPio、Ahci、Nvme、UsbCore、Xhci、UsbHub、UsbStorage、Vfs、Ext4、ISO9660、FAT32File）；调度、Init、用户空间、AppSDK 与 `Packages/LikesProgramSDK` 尚未实现。已知限制：USB hub 下游设备枚举与 UAS、FAT32 / ISO9660 的内核挂载消费者、布局描述重读与 `DeviceKind` 缩小候选范围尚未落地，属阶段 3 或后续阶段。Baleen 与内核共用的基础件在 `Packages/Common`：Print 前端、平台原语、[存储抽象](Packages/Common/README.md)、`BootInfo` 与内核镜像数据契约 |

顶层 `make` 现在就能产出可启动的 `Out/LikesProgram.iso` 与 `Out/LikesProgram.hdd`。Stub、BaleenCore、UEFI 侧与内核侧 Core 已由各自包真实构建，系统卷由 `Packages/LikesProgramOS` 从 `Static/` 卷根用 `mke2fs -d` 生成，卷内目录树见[系统卷布局](Docs/Specs/Common/系统卷布局.md)，内核集按 `.os` 发布线身份 `BuildId` 落在 `/LikesProgram/System/KernelSets/<BuildId>/`（`LikesProgram.os`、签名清单 `Manifest.mft`、`Modules.list` 与 `Modules/` 目录，卷内路径写进 `Out/KernelPath`；模块修订不改变集目录）；布局描述由组装器按[布局描述](Docs/Specs/Baleen/布局描述.md)生成并回填，`KernelPath` 指向缺省集目录里清单登记的 `.os` 条目（选择器装载时以验签通过的清单条目为准，布局路径须与它逐字节一致）；清单与签名由 `MakeManifest` / `SignManifest` 用仓库外的开发期密钥生成，验签用的信任锚由 `MakeKeys` 从同一密钥导出并编进 BaleenCore 与 UEFI 侧的 `BOOTX64.EFI`（两个固件环境各编同一份锚源，密钥与签名流程见[内核集清单与签名](Docs/Specs/内核集清单与签名.md)）；EFI 引导镜像是真实产物（`Packages/Baleen/Uefi` 产出 `Efi.img`，内含 `BOOTX64.EFI`），占位件只在它缺失时顶上；每个载荷的实际来源都在构建日志里逐项打印。BIOS 路径与 UEFI 路径都已经从受支持介质启动进入内核：BIOS 侧经 IPL → Stub → BaleenCore，BaleenCore 按引导状态记录选版、验证所选集的清单签名与产物摘要，再读取并校验内核镜像，把它装到装载地址、构造 `BootInfo` 与恒等映射页表，切长模式跳到内核入口；UEFI 侧由 `BOOTX64.EFI` 在固件环境里走完同样的选版、验签与装载，调 `ExitBootServices` 退出启动服务后按同一份 `BootInfo` 契约交给内核。`BootInfo` 同时带上**启动来源**（布局描述偏移、系统卷位置与卷标识、引导状态区域、介质容量、内核路径、启动设备线索与引导器占用区间，见 [Baleen 引导器](Docs/Specs/Baleen/README.md) 6.1）：内核由此知道这次是从哪个介质、哪个卷、哪条路径起来的，运行期更新才能把新内核集写进同一个卷、把状态记录写回同一处区域，而不到设备里猜。

## 快速开始

构建需要 GNU make、NASM 3.x、g++（C++20），以及生成占位件与系统卷的 `mkfs.vfat` / `mke2fs`（e2fsprogs，需要支持 `-d` 的版本）；启动与测试需要 QEMU（可选 Bochs，UEFI 用例需要 OVMF）。

构建全程把警告当错误：C++ 用 `-Wall -Wextra -Werror`（宿主工具另加 `-Wpedantic`），NASM 用 `-Werror`，链接用 `--fatal-warnings`。新增警告会直接让构建失败，不会被跳过。

```sh
make                # 构建 Out/LikesProgram.iso 与 Out/LikesProgram.hdd
make iso            # 只构建 ISO（make hdd 只构建磁盘镜像）
make check-all      # 一条命令跑完宿主侧 30 个检查工具（1000 例；工具数/用例数带固定门禁），输出“通过/失败 + 用例计数”表
make check-all WITH_BOOT=1   # 再跑十二档启动矩阵（BIOS 六档 + UEFI 五档 + IA32 提示档），每档九列：
                             # 进内核 / 刷新路径 / 诊断编号 / 预装 / 闭包Init / 挂卷 / 清单 / ISO源 / 运行期；
                             # 之后加运行期模块夹具 4 档与引导状态演练 19 档（空区域 / 槽位损坏 / 候选失败 /
                             # 写中途断电（撕裂）/ 降级策略位 / 设备级写缓存（刷新覆盖核验、无原语边界、丢缓存重来），
                             # 含同集两修订切换 3 档）；本档 G7 总检查项 1035
make check-all ACCEPT=1     # 扩展验证：另加 G7 三类失败注入载体 8 档、NVMe 与 4Kn 介质/控制器、
                             # 4Kn 状态推进 5 档（状态演练共 24 档）、吊销与防回滚三档；G7 总检查项 1048
make check-all EVIDENCE_DIR=/tmp/check-evidence   # 把可长期留存、可复核的验证证据写到 Out/ 之外
make release-guard  # 断言交付物载荷来自真实产物；STUB / CORE / EFI_IMAGE / SYSTEM_VOLUME 回退到占位件即失败（deliverables 与 check-all 的镜像批次已内置该门禁）
make reproducible-check     # 同一构建输入连续构建两次，全量交付物 sha256 一致（含镜像 JSON、运行期与两修订变体；需要已有 .cache/Keys）
make run            # 启动镜像：默认 hdd + qemu + 直接挂载 + BIOS，不开窗口
make run-win        # 同上，但开本地窗口（QEMU 用 gtk/sdl，Bochs 用 wx）
make run iso qemu built uefi    # 例：光盘 + QEMU + UEFI
make compile-db     # 扫描各包写出 compile_commands.json，供 clangd 与 C/C++ 扩展取用
make test-tools     # 只构建测试与回归工具（Tools/Test），手工执行，不进入默认构建
Tools/Bin/CheckIpl --ipl-dir Packages/Baleen/Ipl/Out/Bin # 手工验证 IPL，不依赖 Test 目录
Tools/Bin/CheckStub --stub-dir Packages/Baleen/Stub/Out/Bin --ipl-dir Packages/Baleen/Ipl/Out/Bin # 手工验证 Stub 与交权块五个服务（含写盘）
Tools/Bin/CheckStorage # 手工验证 S1 存储抽象：用例、32 位双编译与目标侧链接检查
Tools/Bin/CheckExt4 # 手工验证 Ext4 只读实现、块设备适配器与 AHCI 刷盘编码：真实卷夹具、故障注入与伪控制器
Tools/Bin/CheckCrypto # 手工验证密码学与信任锚轮换窗口：标准向量、两侧互验与锚生成
Tools/Bin/CheckSelect # 手工验证选版与引导状态记录：四态决策、八槽环形、写入门槛与维护动作
Tools/Bin/CheckMemory # 手工验证内存图判定与弹跳窗口搜索：合并规则、边界与找不到窗口
Tools/Bin/CheckRevocation # 手工验证签名吊销记录与引导状态区单调纪元：结构、验签、自吊销、并集与清单判定
Tools/Bin/CheckVfs # 手工验证 VFS 与块设备注册表：八槽、挂载点最长匹配、唯一候选与错误码
Tools/Bin/CheckNvme # 手工验证 NVMe 驱动：初始化、Identify、READ、PRP1 与 4Kn/512e
Tools/Bin/CheckIso9660 / Tools/Bin/CheckFat32 # 手工验证 ISO9660 / FAT32 只读解析与卷标识
Tools/Bin/MakeRevocation --help # 生成并签名吊销记录（LPRV）；用法与约束见工具 README
make maintain-tools # 只构建维护工具（Tools/Maintain）
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --report # 报告引导状态区域；另有 --repair、--clear、--accept-seen
make -C Packages/LikesProgramOS check   # 复核内核映像与系统卷（内核镜像数据、摘要、入口）
make help           # 全部目标与可用变量
```

`make run` 不开窗口，串口输出打在终端上（QEMU 用 `-display none`，Bochs 画面走 VNC，从 5900 起）；要在本地窗口里看画面就用 `make run-win`，它接受与 `run` 完全相同的四个位置参数。窗口路径要求机器上有图形会话：QEMU 需要装了 `qemu-system-gui` 的 gtk/sdl 后端，Bochs 用 wx 显示库（配置界面同为 wx，否则 Bochs 会退回 rfb）。四个位置参数、载荷变量与其余目标见顶层 [Makefile](Makefile) 头部注释与 `make help`。

`make check-all` 的产物默认落在 `Out/check/`（`summary.txt` 与逐例日志）。**那是构建产物，不是证据目录**：`make clean` 与 `make clean-all` 会连同 `Out/` 一起删掉它。需要长期留存、可复核的验证记录用 `make check-all EVIDENCE=<目录>`（变量也可写成 `EVIDENCE_DIR=`）：目录必须在项目 `Out/` 之外，运行结束时把 `summary.txt`、host / boot / state / accept 各组的逐例日志、状态报告与 `metadata.txt`（UTC 时间、git 版本、uname、QEMU / OVMF 版本与文件哈希、镜像批次与 ESP 里 `BOOTX64.EFI` / `BOOTIA32.EFI` / `Efi.img` 的 sha256 与字节数）复制过去，交付记录引用这个目录，而不是 `Out/check`。`CHECK_ALL_ARGS="--out <目录>"` 只移动可删输出的位置，不产生证据。

**可复现构建由 `make reproducible-check` 核对**：它要求工作区已有 `.cache/Keys` 与 `.cache/AltKeys`——开发期密钥是**外部构建输入**，不进仓库、不进镜像，没有就按 [MakeKeys 说明](Tools/Build/MakeKeys/README.md) 准备；脚本固定 `SOURCE_DATE_EPOCH` 与 `KEY_ID` 后做两次 `clean-all` + 全量构建，逐项比较交付物 sha256：主镜像与验证变体、各镜像的同名 `.json` 构建清单、ESP、`BOOTX64.EFI` / `BOOTIA32.EFI`、内核与引导器产物，以及运行期模块夹具与同集两修订夹具的卷/镜像/集目录产物（清单随交付集增长，**不写死份数**）。summary 与证据目录另留 `inputs.txt`：两把密钥的 sha256、`g++` / `ld` / `nasm` / `mke2fs` / `mkfs.vfat` 版本与 `uname`，供确认两次构建用的是同一组输入。**口径边界**：本检查证明“同机、同工具链、同密钥、同固定时间戳下两次全量构建逐字节一致”，不等于“同一构建输入跨主机/跨工具链也一致”——后者需要固定工具链与跨机构建，不在本脚本的结论范围内。构建时间戳统一取固定的 `SOURCE_DATE_EPOCH`（PE 头、ISO 与 Ext4 元数据），比较的是文件内容而不是 mtime。

BIOS 磁盘入口接受 512 / 4096 字节逻辑扇区，软盘与 USB-FDD/ZIP 不在范围。**引导区（描述符区与载荷）的位置字段以 512 字节为基准，装载方按运行期探测到的单位换算**，因此同一份 `LikesProgram.iso` 可直接刻录到光盘、写入 512e 或 4Kn 的 U 盘 / 移动硬盘；**ESP 双份、分区项起点固定写 LBA 2048**：512e 固件读到 1 MiB、4Kn 固件读到 8 MiB，两处各有一份同样的 FAT16 卷，所以同一份镜像的 UEFI 路径在两种设备上都通；Ext4 那一项（`0x83`）仍按 `DISK_SECTOR_BYTES` 生成，内核对系统卷用布局描述定位、不依赖它。各类启动模式的当前支持状态见[启动介质与文件系统](Docs/Specs/Common/启动介质与文件系统.md) 1.1。

编辑器配置在 `.vscode`：常用构建与运行操作做成了任务，clangd 与 C/C++ 扩展都读根目录的 `compile_commands.json`；该文件含本机绝对路径、不进 Git，Makefile 改过之后重跑 `make compile-db` 刷新。

## 文档索引

规格文档位于 `Docs/Specs`，按主题分组：`Baleen/` 收录引导器专属规格，`Common/` 收录跨模块通用规格（介质与文件系统、文件系统抽象与 VFS、ABI 兼容与外部契约），其余为各领域规格；面向实现者的手册在 `Docs/Manuals`：

| 文档 | 内容 |
| --- | --- |
| [Baleen 引导器](Docs/Specs/Baleen/README.md) | 引导器职责边界、引导模式支持梯队、硬件与介质范围、卷定位与内核路径、装载交接、引导选择器、产物清单 |
| [一级引导契约](Docs/Specs/Baleen/一级引导契约.md) | IPL 与 Stub 的介质访问、扇区单位、装载窗口与交权块约定 |
| [布局描述](Docs/Specs/Baleen/布局描述.md) | `BaleenLayout.bin` 的字节表、写入位置与校验，引导状态区域，以及模板与组装器的回填职责 |
| [内核镜像数据](Docs/Specs/Baleen/内核镜像数据.md) | 内核镜像文件起点那份数据的字节表、摘要覆盖、BaleenCore 与内核两侧的校验顺序，以及装载交接的当前边界 |
| [引导状态记录](Docs/Specs/Baleen/引导状态记录.md) | 引导状态区域的记录字节表、八槽环形布局、写入顺序与掉电语义，每候选的尝试次数与防回滚记录 |
| [失败诊断编号（引导器）](Docs/Specs/Baleen/失败诊断编号.md) | 引导器交权之前的 `E-IPL` / `E-STUB` / `E-CORE` / `E-UEFI` 编号分配与稳定规则 |
| [缺陷与修复记录](Docs/Test/缺陷与修复记录.md) | 测试发现的缺陷流水：现象、影响、根因、修法与回归网；刻意不修的与仍未覆盖的验证缺口也记在这里 |
| [系统卷布局](Docs/Specs/Common/系统卷布局.md) | 系统卷的目录树与职责边界：系统目录、程序目录与用户目录，命名与放置规则，以及 `Static/` 卷根的组装方式 |
| [内核启动与固件边界](Docs/Specs/内核启动与固件边界.md) | 内核入口契约、固件服务的失效边界、第三方引导方式的接入条件 |
| [内核架构与自举闭包](Docs/Specs/内核架构与自举闭包.md) | 内核组成、`LINK_MODE` 与源码边界、内核核心与引导闭包划分、外置能力归属、镜像体积口径 |
| [输出通道与Print](Docs/Specs/输出通道与Print.md) | 引导期与内核共用的文本输出：前端与目标契约、各阶段装配、重定向与缺省行为 |
| [内核诊断编号](Docs/Specs/内核诊断编号.md) | 内核侧 `E-KRN` 号段规划、现有失败路径的编号分配、编号稳定规则，以及屏幕输出与等待/重启要求 |
| [CPU 描述符表](Docs/Specs/CPU描述符表.md) | GDT/IDT 的装载机制、Baleen 与内核的分工、32 位与长模式的字节差异 |
| [内核模块与驱动清单](Docs/Specs/内核模块与驱动清单.md) | 内核模块、驱动、SDK 与契约层的名称、交付形态和职责 |
| [模块与驱动装载](Docs/Specs/模块与驱动装载.md) | 模块与驱动的装载流程、执行上下文、归属登记与热替换边界 |
| [内核模块容器](Docs/Specs/内核模块容器.md) | `.osm` 外置内核模块容器的字节格式、装载模型、接口版本与兼容窗口、装载流程与失败语义 |
| [模块契约层](Docs/Specs/模块契约层.md) | 内核与 `.osm` 模块之间唯一的调用面：接口标识与槽位、版本冻结与提供者唯一、`KernelAbiVersion` 导出、入口 ABI、模块返回码、驱动接入面、生成物与构建期检查 |
| [启动介质与文件系统](Docs/Specs/Common/启动介质与文件系统.md) | 启动介质与访问链、ISO 的三种投递方式、必备文件系统、阶段切换与写入启用 |
| [文件系统抽象与 VFS](Docs/Specs/Common/文件系统抽象与VFS.md) | Baleen 与内核共用的块设备、偏移视图、统一路径索引与文件系统接口；共用边界与阶段划分 |
| [内核集更新与回滚](Docs/Specs/内核集更新与回滚.md) | 内核集构成、试启动状态机、确认与失败处理、各类更新对象的生效边界 |
| [内核集清单与签名](Docs/Specs/内核集清单与签名.md) | 内核集构成与 `.os` 发布线身份 `BuildId`、`Modules.list`、卷内落点、清单字节表、签名与信任锚、密钥轮换与撤销、防回滚与试启动身份传递 |
| [模块修订与更新](Docs/Specs/模块修订与更新.md) | 同一 `.os` 发布线内的模块主版本门、`Modules.list`、目录布局与文件名、模块修订的版本保留与更新/回滚流程 |
| [用户空间与常驻服务](Docs/Specs/用户空间与常驻服务.md) | 用户态程序与常驻服务清单、安装包与安装事务、构建期配套工具 |
| [后缀设计](Docs/Specs/后缀设计.md) | 容器后缀的语义、总表与命名规则 |
| [ABI 兼容与外部契约](Docs/Specs/Common/ABI兼容与外部契约.md) | 内核集内部与对外边界的兼容规则：引导交接契约、DriveSDK、AppSDK |
| [镜像制作器与安装器实现手册](Docs/Manuals/镜像制作器与安装器实现手册.md) | 产出可引导介质的实现指南：描述符区格式、需要写回的信息与位置、建议流程，以及通用镜像制作器与测试安装器的定位与能力边界 |
| [体积与内存报告](Docs/Manuals/体积与内存报告.md) | D10 体积口径、启动峰值与运行期常驻的可观测统计、复现命令与边界 |
| [Baleen 验证报告](Docs/Test/Baleen-验证报告.md) | Baleen 子项目的全量验证范围、用例矩阵、失败注入、UEFI 自建控制器路径、可复现性、诊断编号一致性、实机排除项与证据入口 |

各子项目与工具目录的 README 描述各自的源码、构建与产物：

- [Baleen 子项目](Packages/Baleen/README.md) ・ [BaleenCore](Packages/Baleen/Core/README.md) ・ [Stub](Packages/Baleen/Stub/README.md) ・ [UEFI 侧](Packages/Baleen/Uefi/README.md)
- [一级引导 Ipl](Packages/Baleen/Ipl/README.md)
- [系统本体 LikesProgramOS](Packages/LikesProgramOS/README.md) ・ [内核侧 Core](Packages/LikesProgramOS/Core/README.md)
- [KernelModules：引导闭包模块包](Packages/KernelModules/README.md)（13 个闭包模块 + 运行期故障注入夹具）
- [Common：Baleen 与内核共用的基础件](Packages/Common/README.md)
- 构建期工具：[MakeContract：契约生成器](Tools/Build/MakeContract/README.md) ・ [PackModule：模块打包器](Tools/Build/PackModule/README.md) ・ [MakeImage：通用镜像制作器](Tools/Build/MakeImage/README.md) ・ [TestInstaller：测试安装器](Tools/Build/TestInstaller/README.md) ・ [MakePayloads：开发占位载荷](Tools/Build/MakePayloads/README.md) ・ [PackImage：内核镜像数据打包/校验](Tools/Build/PackImage/README.md) ・ [MakeKeys](Tools/Build/MakeKeys/README.md) ・ [MakeManifest](Tools/Build/MakeManifest/README.md) ・ [SignManifest](Tools/Build/SignManifest/README.md) ・ [MakeRevocation](Tools/Build/MakeRevocation/README.md)
- [Tools/Common：宿主工具的跨分类共享代码](Tools/Common/README.md) ・ [Tools/Build/Common：构建期工具共享代码](Tools/Build/Common/README.md)
- 测试与回归工具：[CheckIpl](Tools/Test/CheckIpl/README.md) ・ [CheckStub](Tools/Test/CheckStub/README.md) ・ [CheckStorage](Tools/Test/CheckStorage/README.md) ・ [CheckExt4](Tools/Test/CheckExt4/README.md) ・ [CheckCrypto](Tools/Test/CheckCrypto/README.md) ・ [CheckSelect](Tools/Test/CheckSelect/README.md) ・ [CheckMemory](Tools/Test/CheckMemory/README.md) ・ [CheckRevocation](Tools/Test/CheckRevocation/README.md) ・ [CheckEfiBlockDevice](Tools/Test/CheckEfiBlockDevice/README.md) ・ [CheckOsm](Tools/Test/CheckOsm/README.md) ・ [CheckPreload](Tools/Test/CheckPreload/README.md) ・ [CheckRevision](Tools/Test/CheckRevision/README.md) ・ [CheckHandoff](Tools/Test/CheckHandoff/README.md) ・ [CheckScreen](Tools/Test/CheckScreen/README.md) ・ [CheckObjects](Tools/Test/CheckObjects/README.md) ・ [CheckContainer](Tools/Test/CheckContainer/README.md) ・ [CheckRegistry](Tools/Test/CheckRegistry/README.md) ・ [CheckDependency](Tools/Test/CheckDependency/README.md) ・ [CheckRelocation](Tools/Test/CheckRelocation/README.md) ・ [CheckLoader](Tools/Test/CheckLoader/README.md) ・ [CheckDevModel](Tools/Test/CheckDevModel/README.md) ・ [CheckVfs](Tools/Test/CheckVfs/README.md) ・ [CheckExt4Module](Tools/Test/CheckExt4Module/README.md) ・ [CheckNvme](Tools/Test/CheckNvme/README.md) ・ [CheckIso9660](Tools/Test/CheckIso9660/README.md) ・ [CheckFat32](Tools/Test/CheckFat32/README.md) ・ USB 链检查（`CheckUsbCore` / `CheckXhci` / `CheckUsbHub` / `CheckUsbStorage`，无单独 README，用法见 `make test-tools`）・ [RunBootCase：启动用例运行工具](Tools/Test/RunBootCase/README.md)
- [FixBootState：引导状态维护工具](Tools/Maintain/FixBootState/README.md)

## 授权

Apache License 2.0，见 [LICENSE](LICENSE)。
