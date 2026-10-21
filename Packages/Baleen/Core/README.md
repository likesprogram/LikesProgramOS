# BaleenCore — 职责与当前实现

`BaleenCore` 是 BIOS 路径中继 IPL 与 Stub 之后的第二阶段。Stub 校验 `CoreDescriptor` 与镜像头、比对整幅摘要后，把 `BaleenCore.bin` 装到物理 1MiB，按 `CORE_LOAD + 头里的 EntryOffset` 跳入；入口时 `ESI` 指向交权块（定义见 [Stub 说明](../Stub/README.md)第二节）。BaleenCore 从交权块取得介质身份、内存图与固件服务入口，自己装配输出，此后不再依赖 Stub 的行为。

源码分工沿用 Stub 的约定：`Src/Core.cpp` 承担全部启动流程与全部诊断输出——步骤顺序、进度行、失败原因与停机都在这一段里；`Src/Console.cpp` 只做 BaleenCore 控制台的装配（VGA 文本、`0xE9` 与 COM1 三路组合，是独立于 Stub 的实例），不打印也不停机；`Src/Ext4.cpp` 是 Ext4 只读最小实现，`Src/BiosBlockDevice.cpp` 把交权块的读盘服务包成块设备（低 1MiB 中转窗口在此完成），`Src/LoadKernel.cpp` 定位、校验并装载内核镜像（读首块核对内核镜像数据与所选集 `BuildId`、流式比对整幅摘要、核对装载区、整幅读进装载地址），`Src/SelectSet.cpp` 做选版（集目录推导、状态决策与状态写回），状态记录的读写来自共用包的 `Packages/Common/Src/BootState.cpp`（引导期选择器与运行期写入方同一份实现），`Src/ManifestVerify.cpp` 验证集清单与全部产物，`Src/TrustAnchor.cpp` 按 `KeyId` 查内嵌信任锚（表由构建期生成的 `Out/TrustAnchors.cpp` 提供，生成输入见下），`Src/Libc.cpp` 提供无运行库环境引用的最小 C 运行库函数；`Src/Core.asm` 是入口、异常桩、交接入口与自己的栈，不做诊断排版；`Include/CoreHeader.inc` 与 `Include/Const.inc` 是汇编侧常量。

**当前实现到内核交接为止**：横幅、交权块校验、镜像事实交叉核对、CPU 快照、运行期段表与中断表、环境事实播报、布局描述读取与校验、系统卷偏移视图与 Ext4 只读挂载（含卷标识核对）、内核集选版与清单签名验证（含防回滚比较，见[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)与[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)）、按验证通过的清单 `.os` 条目定位内核镜像（缺省集的布局路径须与它逐字节一致）、读取并校验内核镜像数据（含 32 字节 `BuildId` 与所选集标识一致，`E-CORE-65`）与整幅摘要（含装载区落点检查、与仍在执行的引导器自身区间不重叠 `E-CORE-91`）、装载到装载地址并清零未落盘尾部、构造 BootInfo 与恒等映射页表、切长模式进入内核都已可用；引导状态区域的实际写入经 Stub 的写盘服务完成（`Src/BiosBlockDevice.cpp` 负责低地址窗口与分段，见[Stub 说明](../Stub/README.md)第二节），介质可写时选版的状态转换逐次落盘并读回校验（写盘服务本身的可用性与写后读回由 [CheckStub](../../../Tools/Test/CheckStub/README.md) 的探针在 QEMU 里覆盖，选版与状态记录由 [CheckSelect](../../../Tools/Test/CheckSelect/README.md) 覆盖）。内核镜像数据的字节表见[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)，内核侧入口见[内核侧 Core 说明](../../LikesProgramOS/Core/README.md)。Ext4 只读实现的边界与验证见 [CheckExt4](../../../Tools/Test/CheckExt4/README.md)。构建：`make -C Packages/Baleen/Core` 产出 `Out/Bin/BaleenCore.bin`，`make check` 复核长度上限、头字段与摘要，顶层组装按真实产物优先取用；信任锚表由 `Out/TrustAnchors.cpp` 提供——当前密钥从仓库外的 `.cache/Keys` 派生，轮换窗口里的旧公钥取进版本库的 `Anchors/Retained.txt`，两者由 `Tools/Bin/MakeKeys --anchors` 合并（[MakeKeys 说明](../../../Tools/Build/MakeKeys/README.md)）；内嵌的最低允许 `SecurityVersion` 由构建参数 `SECURITY_FLOOR` 写进生成头（默认 `0`，抬高即吊销更低世代，`make SECURITY_FLOOR=<n>` 后重建），值随镜像一起发布。

## 一、入口与镜像

| 项 | 约定 |
| --- | --- |
| 镜像形态 | 与 Stub 同构：文件偏移 `0` 起是 16 位近跳转前缀与保留区，`0x10` 起是 128 字节完整性头，`0x90` 起是 32 位保护模式代码；两种镜像只有 `Magic` 不同（BaleenCore 为 `BLNCORE`） |
| 装入与入口 | 物理 `0x100000`；Stub 直接跳到 `CORE_LOAD + EntryOffset`，**不经过文件起点的前缀** |
| 文件起点前缀 | 不被执行，只为与 Stub 镜像同构的格式要求而存在（只有 16 位跳转语义，宿主打包器核对它与 `EntryOffset` 一致）；从装入点按 32 位执行会跳飞 |
| 进入状态 | 32 位保护模式、平坦段（`CS=0x08`、`DS/ES/SS=0x10`）、分页关闭、`IF=0`、`DF=0`、`ESI` 指向交权块；这些由交权契约保证，BaleenCore 不重新推导 |
| 栈 | BaleenCore 自己的 16 KiB 栈在未初始化区（`Const.inc` 的 `CORE_STACK_BYTES`）；入口先 `MOV ESP` 换栈，不继续用 Stub 冻结的保护模式栈 |
| 未初始化区 | 头里 `MemoryBytes` 超出文件的部分由 Stub 在交权前清零，BaleenCore 侧不重复清零 |
| 头字段填出方 | 长度字段由链接器按 `Core.ld` 的 `__image_end` / `__bss_end` 填入；`BuildId` 与 `Digest` 由 `Tools/Bin/PackImage` 填充；`make check` 与顶层组装器都会复核 |

入口把 `ESI` 压栈后调用 `_Core_Main`，不返回。异常桩与 Stub 同构：每个向量一个桩，压向量号后进公共入口，由 C++ 侧的 `_Core_Exception_Handler` 打印向量、错误码与 `Eip` 后停机；BaleenCore 自己建立段表与中断表，换表之后异常不再落到 Stub 的门上。

## 二、启动流程与诊断行

`_Core_Main` 的步骤顺序固定，每步一行标签诊断（标签表见 [输出通道与Print](../../../Docs/Specs/输出通道与Print.md)）：

1. 装配 BaleenCore 自己的控制台并打印横幅 `[ BALEENCORE ] LikesProgramOS BaleenCore`；**不清屏**，Stub 的行留在屏上，引导链在串口与调试口里连续可读
2. 核对交权块：`Magic`、`Version`、`Bytes` 三项固定字段逐项检查，任一不符即以 `FATAL` 原因停机
3. 核对自身镜像事实：读自己的镜像头核对固定字段与入口前缀；再把头字段与链接布局（`__image_end` / `__bss_end`）、交权块的 `CoreLoad` / `CoreBytes` 交叉比对。**摘要不重复取**：Stub 已在交权前比对过整幅镜像，BaleenCore 只核对结构与配对关系
4. CPU 快照：记录模式与厂商串；模式不是 32 位保护模式即停机。交接阶段的前置能力同时在这里把关：**PAE、长模式与 NX 缺一即停机**（`E-CORE-10`..`E-CORE-12`），而不是等到切模式或进入内核时才崩；能力事实与厂商串同一行输出（`Pae=` / `Long=` / `Nx=`）
5. 建立运行期段表与中断表；续行给出 `Cpu=`、`Mode=`、`Build=`
6. 播报环境事实：介质与扇区大小、BaleenCore 装载地址与字节数、内存图条数与截断标志、可用内存 MiB
7. 读布局描述并校验：从固定字节偏移 `0x20000` 读一个 4096 字节块，核对固定字段、保留字段、内核路径长度与校验和（字节表见[布局描述](../../../Docs/Specs/Baleen/布局描述.md)）；续行给出 `Volume=`、`Bytes=`、`State=` 与 `Kernel=`
8. 挂载系统卷：按布局描述给的偏移与长度在介质设备上开偏移视图，挂 Ext4（只读，特性清单取 `Storage/Ext4Features.hpp`；解析前提位缺失、卷未干净卸载、64 位卷的高位块数或描述符大小非法都在这一步拒绝），并把 superblock 的卷标识与布局描述逐字节比较；不一致即停机，不猜测
9. 选版与签名（选版先于装载内核镜像）：选择器只接受集目录形态的布局路径——缺省集的内核路径必须是 `KernelSets/<BuildId>/<相对产物路径>`，否则以 `E-CORE-84` 拒绝（含旧扁平落点与过长的路径），不存在未验证的直启回退；先读系统卷里的签名吊销记录（`/LikesProgram/System/Revocation.rvk`），只有文件不存在（`NotFound`）按“没有记录”处理、打不开或读不了与其它不可用一样报 `WARN`（`E-CORE-75`）后继续，合法记录与状态记录里的持久事实取并集得到有效吊销事实（[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md) 5.1、5.2）；读布局描述给出的状态区域（`StateOffset` / `StateBytes`），按[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)的槽位规则取代次最高且可验证的一个——空区域与没有状态区域按“无记录”处理、启动布局描述指定的缺省集（缺省集同样要过下面的清单验证），读失败（`IoError`）与区域参数非法（`InvalidArea`）不是空区域、分别以 `E-CORE-89` / `E-CORE-90` 停机，空区域在可写介质上会把验证过的缺省集写成 `Confirmed` 记录并持久化已见最高 `SecurityVersion` 与吊销并集（回退缺省集时保持原状态、抬升 seen 与并入吊销，写不成只报 `WARN E-CORE-70`），有写入痕迹但全部槽位都不可验证时默认不猜测目标集、以 `FATAL` 停机（`E-CORE-69`，文本里给出维护动作），介质在布局描述里置了降级策略位时改为按缺省集启动并报 `WARN`（`E-CORE-56`）；按记录选出的集与缺省集都在装载前验证清单结构与保留字段、Ed25519 签名与信任锚、`BuildId` 一致性、防回滚的 `SecurityVersion` 比较与逐产物 SHA-256（`E-CORE-61`..`E-CORE-68`），其中签名密钥在有效吊销位图里时以 `E-CORE-74` 拒绝，规则见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)；候选验证失败（`E-CORE-72`）或可用尝试次数为零（`E-CORE-73`）时写 `Failed` 并改选已知良好集，状态写入不成立时不做状态转换、报 `WARN`（`E-CORE-70`）后按降级路径继续；回退到缺省集时把设备已见最高 `SecurityVersion` 一并交给清单验证（没有记录时取 `0`），缺省集因此同样受防回滚比较约束，低于已见的缺省集以 `E-CORE-66` 拒绝；缺省集还要求布局路径里的相对产物路径与清单登记的 `.os` 条目逐字节一致，否则以 `E-CORE-85` 拒绝，装载路径一律取验证通过的清单 `.os` 条目、不按固定文件名拼接；吊销事实前进时随同状态写回持久化（只增不减），状态写入按**写 → 刷介质缓存 → 读回校验**的次序执行，刷新失败按写失败处理（写缓存经 AHCI 的 `FLUSH CACHE` 刷新；找到端口还要核验它是不是启动介质所在的设备（把启动介质第 0 号块经固件与经该端口各读一次比对内容），核验不成立或没有可刷新端口时按“无可用原语”通过，并在每次真正提交后报 `WARN E-CORE-71`，边界见[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)第五节）；续行给出 `Set=`（集标识的 64 位小写十六进制；选版成功时集一定经过清单验签）与 `Attempt=`（试启动身份，非试启动为 0）
10. 定位并校验内核镜像：按选版给出的装载路径打开——装载路径由集目录与验证通过的清单 `.os` 条目拼成，不按固定文件名另行拼接；路径不存在时以 `WARN` 说明内核尚未放入系统卷。校验分三步：读首块核对[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)的固定字段与内核事实（含 32 字节 `BuildId` 必须等于所选集标识，否则以 `E-CORE-65` 拒绝），核对 `BootInfoVersion` 是本侧能构造的版本，核对镜像与静态内存的较大者整段落在交权块内存图的可用区间内（相邻可用条目按同一段连续内存合并，判定与 Stub 共用 `Baleen::SpanUsable`），再从文件起点流式重读整幅计算 SHA-256（`Digest` 字段按 32 个零代入）与数据里的摘要比对；任一不符即以 `FATAL` 原因停机
11. 装载：整幅读进内核镜像数据里的 `LoadAddress`，按 `MemoryBytes` 清零未落盘尾部（覆盖最后一个扇区的填充）；续行给出 `Load=`、`Bytes=`、`Span=`
12. 交接状态：构造 `BootInfo`（尺寸、版本、固件状态、介质、扇区大小、输出通道、内存图、内核集身份与**启动来源**；内存图复制到本阶段的静态存储，内核路径与驱动器号放在静态存储里而不是栈上），建恒等映射页表（4 级表、2MiB 大页覆盖低 4GiB）；续行给出 `BootInfo=`、`Mmap=`、`Channels=` 与 `Boot=Volume=… State=… Kind=… Drive=…`、`Path=…`。启动来源（布局描述偏移、系统卷位置与卷标识、引导状态区域、内核路径、设备线索、引导器占用区间）让内核与运行期更新知道这次是从哪个卷、哪条路径起来的，字段表见 [Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 6.1；BIOS 侧交权块没有介质容量这个事实，`MediumBytes` 如实填 0（未提供）
13. 引导耗时：用交权块里 Stub 的时间戳与校准值报出 `BootMs=`（从 Stub 进入起）、`StageMs=`（本阶段）与 `BudgetMs=`，预算 3000 ms，`Verdict=` 给出 `Ok` / `Over` / `NoClock`；超出预算以 `WARN` 与 `E-CORE-55` 报出，不阻断启动。报在装载与交接状态之后、进入内核之前，装载读盘计入预算
14. 进入内核：切长模式（CR4 的 PAE、OSFXSR 与 OSXMMEXCPT，EFER 的 LME 与 NXE，CR0.PG），按恒等映射跳 `LoadAddress + EntryOffset`，`RDI` 指向 `BootInfo`；不返回

失败原因都带稳定编号（`E-CORE-nn`，登记表见[失败诊断编号](../../../Docs/Specs/Baleen/失败诊断编号.md)），诊断行格式为 `[ FATAL ] E-CORE-07 <文本>`；存储层错误名（如 `not supported`）作为补充现场附在同一行末尾的括号里。读盘失败时，`FATAL` 行之后另补一行失败现场（`Lba=`、`Count=`、`Ah=`、`Phase=`）：记录由 Stub 的读盘服务维护，经交权块的 `LastReadFailure` 取用；读盘成功的调用会清空它，非读盘失败因此不会多出这一行。

`Build=` 的写法与 Stub 的约定相同：头里 BuildId 前 4 字节按字节顺序的十六进制，与构建日志里 `PackImage` / 组装器打印的 BuildId 是同一串字节，不引入小端整数解释，两阶段的诊断行因此可以直接对照（约定见 [Stub 说明](../Stub/README.md)第五节）。

## 三、尚未实现

| 项目 | 内容 |
| --- | --- |
| 更新与确认的写入方 | 引导侧（选择、`Trying`/`Failed` 写入、已见版本抬升与回退）已实现。两个**运行期写入方本体**也已实现并落在共用层：`BootState::PublishCandidate`（写 `Pending`）与 `BootState::ConfirmAttempt`（按 `AttemptId` 比较再写 `Confirmed`），见[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)第四节。缺的是**发起方**：`Pending` 的发布方 Updater 与 `Confirmed` 的提交方 Init 都是运行期组件（[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)第四节），内核当前停在核心阶段边界、用户态尚未实现，因此两侧都还没有自动调用者。代执行入口是维护工具 `Tools/Bin/FixBootState --publish / --confirm`，调用的是同一份实现；选版决策、八槽环形、写入门槛与这两个写入方都由 [CheckSelect](../../../Tools/Test/CheckSelect/README.md) 覆盖 |
| 只读介质与写失败 | 介质不提供写服务、写盘失败或刷盘失败时，按降级路径处理：本次不做状态转换、报 `WARN E-CORE-70`，选已知良好集、没有则启动缺省集；写缓存由本阶段直接刷新（`Src/DiskFlush.cpp`：AHCI 优先，传统 IDE 兜底，对在用 ATA 盘下发 `FLUSH CACHE`；命令头按 AHCI 1.3.1 的 CFL/PRDTL/PRDBC 字段编码，身份核验的 LBA 与扇区计数按设备逻辑扇区换算（4Kn=4096 字节），命令出错或超时时 `Flush` 返回 `Io` 并按写失败处理），光盘、USB、NVMe 等两条路径都够不到的介质按“无可用原语”通过，持久化保证止于写成功加读回校验（规格见[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)第五节；命令头与换算由 [CheckExt4](../../../Tools/Test/CheckExt4/README.md) 的伪 AHCI 控制器覆盖） |

本文只描述当前已实现的边界；后续阶段各自补各自的章节，不在这里提前承诺。
