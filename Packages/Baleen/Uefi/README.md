# Packages/Baleen/Uefi — UEFI 侧的固件应用

本包产出三件，都装在 ESP 的 `\EFI\BOOT\` 下：

| 产物 | 形态 | 职责 |
| --- | --- | --- |
| `Out/Bin/BOOTX64.EFI` | PE32+（x64）EFI 应用 | **本项目的 UEFI 引导路径**：读布局描述、挂 Ext4 系统卷、列出内核集目录 |
| `Out/Bin/BOOTIA32.EFI` | PE32（IA32）EFI 应用 | 32 位固件上的不支持提示件：打一行 `E-UEFI-11` 说明为什么起不来，不引导 |
| `Out/Bin/Efi.img` | 4 MiB FAT16 ESP 镜像 | 上面两件的容器：顶层组装器按 `EFI_IMAGE` 优先取它；只有本包产物缺失时，开发构建才退到 `MakePayloads` 生成的占位 ESP（提前停机、不引导，见 [MakePayloads 说明](../../../Tools/Build/MakePayloads/README.md)） |

[Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 3.1 把 UEFI x64 列为**必须支持**的环境（含无 CSM 的 UEFI Class 3）；本包就是这条路径，它绕过 BIOS IPL 与实模式 Stub，直接以固件应用形态跑完 BaleenCore 的同类职责。

```sh
make -C Packages/Baleen/Uefi          # 产出 BOOTX64.EFI、BOOTIA32.EFI 与 Efi.img
make -C Packages/Baleen/Uefi check    # 复核两件产物的 PE 形态、基址、重定位段与关键文本
```

## 当前进度

x64 侧与 BIOS 路径**同进度**：从固件加载起，一路做到把内核交出去。

| 步骤 | 状态 |
| --- | --- |
| 取介质块设备（块设备协议 → `Storage::BlockDevice`） | 已实现：固件块设备协议优先；被加载设备是分区时沿设备路径上溯到整盘 |
| 自建控制器路径（固件没有块协议时的回退） | 已实现：经 `EFI_PCI_IO_PROTOCOL` 定位启动控制器，按类码走 AHCI（ATA 盘，512 字节块）或 NVMe（512 / 4096 字节块）只读路径；读逻辑与内核侧模块共用同一份源码（`Packages/KernelModules/Ahci`、`Nvme`），失败编号 `E-UEFI-100..104`；只读、不申报刷新原语 |
| 读并校验布局描述（字节偏移 `0x20000`） | 已实现，判据与 BaleenCore 共用 `LayoutCheck`：固定字段与标志位、保留字段、校验和、尾部保留区 `[0x144, 0x1000)` 全 0、`KernelPath` 的统一路径语法，以及引导状态区域的六条判据（4096 对齐、最小 4096 字节、端点不回绕、不与布局描述块 / 描述符区 / 系统卷重叠；`E-UEFI-90`，与选版阶段共用同一编号与文本） |
| 挂 Ext4 系统卷并核对卷标识 | 已实现，复用 `Packages/Baleen/Core` 的只读实现 |
| 列出内核集目录 | 已实现，含目录项类型与长度 |
| 写缓存刷新能力 | 已实现：`HasFlushPrimitive` 只在介质可写、块 I/O 协议达到 `REVISION2`、且固件未对 `FlushBlocks` 报过 `EFI_UNSUPPORTED` 时为真；只读介质 `Flush()` 返回 `ReadOnly` 且不声称可刷新；报过 `EFI_UNSUPPORTED` 之后选择器每次提交报 `E-UEFI-71`（提交仍成立，只有设备缓存的保证） |
| 选版（引导状态记录读写、四态决策、写失败降级） | 已实现，复用 `SelectSet`（按 x64 编同一份源码） |
| 清单验签、防回滚、吊销判定 | 已实现，复用 `ManifestVerify` / `Revocation` / 内嵌信任锚 |
| 定位、校验并装载内核镜像 | 已实现，复用 `LoadKernel`：镜像先读进 `UefiAllocator` 经 `AllocatePages` 分配的中转缓冲（页数按镜像文件长度向上取整；分配避开低 1 MiB、引导器跨度与镜像声明的固定装载跨度，失败报 `E-UEFI-86`）；固定装载地址在退出启动服务之前不写，装载跨度与 `LoadedImage` 区间重叠报 `E-UEFI-91` |
| 预装引导闭包并生成交接包 | 已实现（M2 起，与 BIOS 侧共用 `BootClosure`）：解析 `Modules.list` 并按 `BootClosure.def` 筛出闭包成员，规划落点（UEFI 侧经 `UefiAllocator` 注入固件页分配，约束与 BIOS 侧一致：4 KiB 对齐、低 4GiB、不重叠低 1 MiB / 引导器跨度 / 内核固定装载跨度，失败报 `E-UEFI-97`），逐条核对最小读取面、字节数与清单摘要后读进内存（列表里的运行期条目跳过），失败不交接半套；成功行 `boot closure preloaded: count=13 bytes=<m> bad=0`。清单副本、`Modules.list` 副本与预装记录随 `BootInfo` 交给内核，内容与判据与 BIOS 路径一致 |
| 系统卷里没有内核镜像 | 已实现：报 `E-UEFI-32`（WARN）后报 `E-UEFI-58`（同 `E-CORE-58`，WARN），停在边界上、不交接 |
| 固件内存图 → `BootInfo` | 已实现：紧贴 `ExitBootServices` 的 MapKey 循环取最终快照（取图缓冲在退出前分配）；退出后按退出后视角转换（常规内存与启动服务 / 引导器内存都归系统，运行期服务 / ACPI NVS / MMIO 始终不可用），最终所有权核对与 `BootInfo` 都用这份视图 |
| 退出启动服务（MapKey 过期重取） | 已实现：退出前从 `LoadedImage` 协议取出并保存引导器装载跨度（无效报 `E-UEFI-87`）、取最终内存图；退出后按最终内存图核对固定装载跨度整段归系统且不与引导器映像重叠（不满足报 `E-UEFI-88`），再拷入固定地址并清零未落盘尾部，`BootInfo` 标记 `UefiExited` |
| 按内核入口契约交接 | 已实现：`EnterKernel.asm` 关中断、换栈、`RDI` 传 `BootInfo` 后跳入口 |

当前边界之后的事由内核接手：内核入口第一步核对 `BootInfo` 里的固件状态并确认启动服务已失效，随后自建段表、页表与页帧分配器，校验交接包并装载 13 个引导闭包模块。

## 与 BIOS 路径的差别

| 项 | BaleenCore（BIOS） | 本包（UEFI） |
| --- | --- | --- |
| 介质访问 | Stub 交来的读盘服务（实模式弹跳） | 固件块设备协议优先；被加载设备是分区时沿设备路径上溯到整盘。固件没有块协议时走 PCI 自建路径（AHCI ATA / NVMe 只读） |
| 内存图 | Stub 从 E820 取 | 固件 `GetMemoryMap`：紧贴 `ExitBootServices` 取最终快照（MapKey 过期会重取），退出后按退出后视角转换（常规内存与启动服务 / 引导器内存都归系统，运行期服务 / ACPI NVS / MMIO 始终不可用）。`Convert` 保留退出前 / 退出后两种视角（退出前只有常规内存可用），本路径只用退出后的那份 |
| 内核装载内存所有权 | 按 E820 可用区间核对后直接写入（没有分配接口） | 内核镜像先读进 `UefiAllocator` 分配的中转缓冲（`E-UEFI-86`，缓冲避开低 1 MiB、引导器跨度与内核固定跨度）；固定装载跨度在退出启动服务之前不写，退出后按最终内存图确认整段归系统且不与引导器映像重叠，才拷入（`E-UEFI-88`），不覆盖固件内存 |
| 控制台 | VGA 文本 + 0xE9 + COM1 | 固件控制台（退出启动服务前）+ COM1（`SerialTarget::Probe` 通过才挂）+ 0xE9（退出后） |
| 刷新原语 | 自建 AHCI / 传统 IDE 探测 | 固件路径用 `FlushBlocks`：只有可写介质、协议 `REVISION2` 以上且固件未报 `EFI_UNSUPPORTED` 时才声称可刷新，只读介质按 `ReadOnly` 处理。自建控制器路径只读，不申报刷新原语（`Writable=false`、`Flush` 返回 `ReadOnly`） |
| 机器状态准备 | 自建恒等映射页表、切长模式 | 已在长模式与固件页表上运行，直接按契约交接 |
| 引导耗时 | 报 `E-CORE-55`，判据来自 Stub 校准的 TSC | 报 `E-UEFI-55`，判据来自 TSC（先问 CPUID 0x15/0x16，问不出来就用 8254 实测一次）；两条路都失败时报 `Verdict=NoClock`，不报编号 |
| 异常诊断 | 自建 GDT/IDT，异常落进自己的诊断行 | **沿用固件的表**：固件处理程序给出完整寄存器现场且不依赖启动服务，自己重写一份只会更差；内核入口随后换上自己的表 |
| 启动设备线索 | IPL 交权时的 BIOS 驱动器号（只对固件环境有意义） | 分区句柄的 `EFI_DEVICE_PATH_PROTOCOL` 字节（抄进静态缓冲后才交接） |

## 形态与装配

| 项 | 约定 |
| --- | --- |
| 形态 | `BOOTX64.EFI` 是 PE32+（`ld -m i386pep`），`BOOTIA32.EFI` 是 PE32（`ld -m i386pe`）；两者都是子系统 `10`（EFI 应用） |
| 加载硬要求 | `--image-base 0x400000`（基址为 0 的映像固件判为不受支持）与 `--enable-reloc-section`（固件把映像装到别处时要靠 `.reloc` 重定位），缺一即加载失败 |
| 调用约定 | 固件接口一律走 EFIAPI（Microsoft x64 约定）：入口与每个协议函数指针都标了 `ms_abi`，少标一处就会让固件在错误的寄存器里取参数 |
| 编译约束 | `-mno-red-zone`：本阶段中断是开着的，红色区域会被固件的中断处理打断 |
| 内存可见范围 | 内核按低 4GiB 恒等映射读 `BootInfo` 引用的缓冲；本阶段把内核路径、设备线索等放在自己的映像里，映像被固件装到 4GiB 以上时内核读不到（会报 WARN 并按“未提供”处理） |
| 上游复用 | 存储抽象与 Print 前端、Ext4 只读实现、最小 C 运行库、`LayoutCheck` / `SelectSet` / `ManifestVerify` / `LoadKernel` / 信任锚与吊销记录，全部按 **x64 重编同一份源码**（`Packages/Common` 的默认构建面向 32 位阶段，不能混链）；`ImageHeader` 不在此列——镜像头字段校验不属于 UEFI 阶段。共享实现里的诊断编号序号不变，前缀由 `-DBALEEN_STAGE_PREFIX='"E-UEFI"'` 换成 E-UEFI |
| ESP 组装 | `Efi.img` 由 `Tools/Bin/MakePayloads --esp-image` 组装：FAT16 的布局只有那一份实现，本包不另写一份 |

## 介质视图为什么要上溯

布局描述固定在**介质**字节偏移 `0x20000`，而固件加载 `\EFI\BOOT\BOOTX64.EFI` 时给出的设备句柄是**它所在的分区**：分区上的块设备以分区起点为块 0。因此取到分区设备后要沿设备路径上溯一层，用 `LocateDevicePath` 拿到整盘的块设备协议，再读描述符——否则读到的是分区里的文件系统数据，布局描述校验会报 `E-UEFI-18`。

被加载的设备本来就是整个设备时（例如没有分区表的介质）直接用，不上溯。

## 启动来源：内核怎么知道“自己是从哪儿起来的”

交接时 `BootInfo` 带上启动来源（字段表见 [Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 6.1）：布局描述偏移、系统卷偏移与长度、状态区域偏移与长度、介质容量、卷标识、内核路径，以及启动设备线索与引导器自身的占用区间。**运行期更新据此把新内核集写进同一个卷、发布到同一条路径、把状态记录写回同一处区域**，不需要到设备里猜。

- **设备线索**：抄的是分区句柄的设备路径（`PciRoot(...)/Pci(...)/Sata(...)/HD(...)`），在退出启动服务之前**抄进本映像的静态缓冲**，之后不再依赖固件内存。
- **长度上限 512 字节**：放不下时线索按“未提供”交接并报 `E-UEFI-83`（WARN，不阻断），内核改用卷标识核对。
- **占用区间**：`LoadedImage.ImageBase/ImageSize` 在**退出启动服务之前**读出并存入静态变量，`BootInfo` 的 `LoaderBegin` / `LoaderBytes` 用它（协议内存退出后不保证有效）；协议报告的基址或长度为空 / 零时报 `E-UEFI-87`。固定装载跨度与这个区间重叠时报 `E-UEFI-88`，不交接。内核不得把本映像的残留当成可用内存。

## 输出通道

- **固件控制台**（简单文本输出协议）：退出启动服务之前的主通道。固件自己把它镜像到 QEMU 给的串口上（OVMF 在有 `-serial` 时就会），因此这个阶段**不直接写 COM1**——两个通道写同一个 UART 会让每行重复。
- **COM1**（16550 兼容串口）：退出启动服务之后由本阶段直接驱动，与 BIOS 侧共用 `SerialTarget`。**存在性先探测**：按 BDA 的 COM 端口表或 UART 回环自测判定，探测不通过就不初始化、不挂进输出扇出，`Write` 立即丢弃而不空转。
- **0xE9 调试口**：另一条独立通道，给 QEMU / Bochs 的 `-debugcon` 用；真实硬件上写入被忽略；本阶段恒初始化并挂进扇出。

`BootInfo.channels` 按同一份探测结果给出：`DebugPort` 恒置，`Serial` 只在 `SerialTarget::Probe(kDefaultBase)` 通过时置位；内核据此决定后续往哪些目标写。

标签行写法与 BIOS 路径共用一套词汇：`[ BALEENUEFI ] ...`、`[ FATAL ] E-UEFI-xx ...`。

## 验证

宿主侧形态检查：

```sh
make -C Packages/Baleen/Uefi check
```

真机路径的验证要用固件跑一遍（需要 OVMF）：

```sh
make && make run iso qemu built uefi        # 顶层入口：光盘 + QEMU + UEFI
```

也可以直接起一次并把输出落到文件：

```sh
qemu-system-x86_64 -machine q35 -m 512 -display none -monitor none -no-reboot \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=Out/OVMF_VARS.fd \
    -drive file=Out/LikesProgram.hdd,format=raw,if=ide -boot c \
    -serial file:/tmp/uefi-serial.log -debugcon file:/tmp/uefi-debug.log
```

当前在串口日志里的样子（已在 OVMF 下实测，硬盘与光盘档都走到内核）：

```
[ BALEENUEFI ] LikesProgramOS Baleen UEFI (x64)
[ INFO ] Binding the medium block device
[ INFO ] The loaded image sits on a partition; binding the whole medium instead
[ OK ] The layout descriptor is valid
[ OK ] The system volume matches the layout descriptor
[ OK ] The kernel set is verified                      ← 清单验签与防回滚在 UEFI 侧通过
[ ... ] Set=d7575969... Attempt=0x0000000000000000
[ INFO ] Locating and checking the kernel image
[ OK ] The kernel image is valid
[ OK ] The kernel image buffer is allocated
[ INFO ] Loading the kernel image
[ OK ] The kernel image is loaded into the firmware buffer
[ INFO ] Capturing the final firmware memory map
[ INFO ] Exiting the firmware boot services
[ OK ] The firmware boot services are exited
[ OK ] The kernel image is placed at its load address
[ INFO ] Building the handover state
[ ... ] BootInfo=0x000000000041E000 Mmap=0x00000077 Firmware=0x00000001 Channels=0x00000003
[ ... ] Boot=Volume=0x0000000002800000 Bytes=0x0000000001000000 State=0x0000000000021000 Kind=0x00000001 Clue=0x0000009C Medium=0x0000000003800000
[ ... ] Path=/LikesProgram/System/KernelSets/d7575969.../LikesProgram.os
[ ... ] BootMs=0x0000010C BudgetMs=0x00000BB8 Verdict=Ok
[ OK ] Entering the kernel
[ KERNEL ] LikesProgramOS kernel                       ← 内核接过去了
[ OK ] The kernel image data matches the linked image
[ OK ] The BootInfo matches the kernel
[ OK ] The UEFI boot services are already exited
[ OK ] The boot page tables are installed
```

`make check-all WITH_BOOT=1` 会把这五档（HDD / ISO / U 盘 / 4Kn U 盘 / NVMe 4Kn）与 BIOS 六档一起跑，每档抽九列（进内核 / 刷新路径 / 诊断编号 / 预装 / 闭包Init / 挂卷 / 清单 / ISO源 / 运行期）；五档的“进内核”都应为“是”——判定不止内核横幅，还要求“清单已验证 / 已退出启动服务 / 内核已放到装载地址”三条步骤标记都出现。32 位固件与 `qemu-system-i386` 可用时，矩阵再加 IA32 提示档（OVMF32 加载 `BOOTIA32.EFI`，只验证 `E-UEFI-11`、不进内核）。

## 验证

QEMU + OVMF（HDD / ISO / U 盘 / 4Kn U 盘 / NVMe 4Kn 五档，IA32 提示档）与 BIOS 侧一起纳入全量验证；`--accept` 另跑强制自建控制器路径三档（AHCI HDD、NVMe 4Kn 两档正例 + USB 类码负例 `E-UEFI-102`，强制路径变体 `CONTROLLER_PATH=1`），证明同一份 UEFI 代码在真实 AHCI/NVMe 设备上完成卷标识核对、清单验证、预装 13 与交权。正例启动矩阵、失败注入、状态演练、可复现构建的结论与证据入口见 [Baleen 验证报告](../../../Docs/Test/Baleen-验证报告.md)。实机边界见报告排除项。

## 边界

- **不是 Secure Boot 路径**：固件验证引导程序镜像；本产物未签名，开启 Secure Boot 的机器上固件不会执行它，这与本项目无关（见规格 3.4）。
- **只支持线性目录**：Ext4 只读实现拒绝 htree 索引目录、符号链接与内联数据，遇到即按编号报错，不做兼容性猜测。
- **不做镜像头字段校验**：本包不链接 `ImageHeader`，`E-UEFI-40..51` 不由本阶段产生——镜像头自校验只属于 Stub 与 BaleenCore 自身（`E-STUB-40..51` / `E-CORE-40..51`）；把 `ImageHeader` 链进来只会让不产生的编号留在二进制里。**`E-UEFI-52` 同样不产生**：固定装载跨度的归属要等 `ExitBootServices` 之后按最终内存图快照判定（报 `E-UEFI-88`），退出启动服务之前的内存图把启动服务与引导器内存都算在固件手里，不能用来判定可用区间；装载前只核对与引导器映像的重叠（`E-UEFI-91`），`E-UEFI-53` / `E-UEFI-54` 照常产生。
- **只在 x64 固件上被加载**：32 位固件走 `BOOTIA32.EFI`，那一件只提示、不引导。
- **介质类型按块大小判定**：`BootInfo` 的 `media` 字段取“光盘为 2048 字节块、其余为硬盘”，这是交接事实里唯一靠推断的一项。
- **自建控制器路径只覆盖只读的两类盘**：AHCI 侧只支持 ATA 盘（512 字节逻辑块），ATAPI（光盘）与 4Kn SATA 不在当前阶段；NVMe 支持 512 / 4096 字节逻辑块。自建路径不写介质、不申报刷新原语，引导状态写入按只读介质降级（`E-UEFI-70` WARN），不做 `Pending` / `Trying` 转换。
- **自建路径的测试载体是强制变体**：QEMU/OVMF 无法构造“固件故意不给块协议”的现场，因此用 `CONTROLLER_PATH=1` 强制忽略固件块协议，在同一台模拟 AHCI/NVMe 设备上验证同一份代码；真实固件不提供块协议的现场仍属实机验证门。
- **DMA 与 MMIO 假设**：自建路径在启动服务阶段按 x86_64 固件的恒等映射直接访问 BAR，并把 `AllocatePages` 返回的物理地址交给设备做 DMA；不处理 IOMMU。这是最小启动路径的明确边界，QEMU/OVMF 与 PC 固件常态如此。
- **不报引导耗时的情形**：CPUID 给不出频率、且 8254 实测的 TSC 频率落在 100 MHz..100 GHz 之外时，报 `Verdict=NoClock`，不报 `E-UEFI-55`——宁可说没有校准源，也不报一个假数字。
- **校准时机**：8254 的实测在校验退出启动服务之后做，那时定时器归本阶段使用，不与固件抢。
