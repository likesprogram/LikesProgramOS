# Packages/Baleen/Uefi — UEFI 侧的固件应用

本包产出三件，都装在 ESP 的 `\EFI\BOOT\` 下：

| 产物 | 形态 | 职责 |
| --- | --- | --- |
| `Out/Bin/BOOTX64.EFI` | PE32+（x64）EFI 应用 | **本项目的 UEFI 引导路径**：读布局描述、挂 Ext4 系统卷、列出内核集目录 |
| `Out/Bin/BOOTIA32.EFI` | PE32（IA32）EFI 应用 | 32 位固件上的不支持提示件：打一行 `E-UEFI-11` 说明为什么起不来，不引导 |
| `Out/Bin/Efi.img` | 4 MiB FAT16 ESP 镜像 | 上面两件的容器：顶层组装器按 `EFI_IMAGE` 优先取它，取不到才退到占位件 |

[Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 3.1 把 UEFI x64 列为**必须支持**的环境（含无 CSM 的 UEFI Class 3）；本包就是这条路径，它绕过 BIOS IPL 与实模式 Stub，直接以固件应用形态跑完 BaleenCore 的同类职责。

```sh
make -C Packages/Baleen/Uefi          # 产出 BOOTX64.EFI、BOOTIA32.EFI 与 Efi.img
make -C Packages/Baleen/Uefi check    # 复核两件产物的 PE 形态、基址、重定位段与关键文本
```

## 当前进度

x64 侧与 BIOS 路径**同进度**：从固件加载起，一路做到把内核交出去。

| 步骤 | 状态 |
| --- | --- |
| 取介质块设备（块设备协议 → `Storage::BlockDevice`） | 已实现 |
| 读并校验布局描述（字节偏移 `0x20000`） | 已实现，判据与 BaleenCore 逐条一致 |
| 挂 Ext4 系统卷并核对卷标识 | 已实现，复用 `Packages/Baleen/Core` 的只读实现 |
| 列出内核集目录 | 已实现，含目录项类型与长度 |
| 写缓存刷新能力 | 已实现：固件的 `FlushBlocks` 就是刷新原语 |
| 选版（引导状态记录读写、四态决策、写失败降级） | 已实现，复用 `SelectSet`（按 x64 编同一份源码） |
| 清单验签、防回滚、吊销判定 | 已实现，复用 `ManifestVerify` / `Revocation` / 内嵌信任锚 |
| 定位、校验并装载内核镜像 | 已实现，复用 `LoadKernel` |
| 固件内存图 → `BootInfo` | 已实现，只把退出启动服务后归系统的类型上报为可用 |
| 退出启动服务（MapKey 过期重取） | 已实现，`BootInfo` 标记 `UefiExited` |
| 按内核入口契约交接 | 已实现：`EnterKernel.asm` 关中断、换栈、`RDI` 传 `BootInfo` 后跳入口 |

当前边界之后的事由内核接手：内核入口第一步核对 `BootInfo` 里的固件状态并确认启动服务已失效，随后自建段表、页表与页帧分配器。

## 与 BIOS 路径的差别

| 项 | BaleenCore（BIOS） | 本包（UEFI） |
| --- | --- | --- |
| 介质访问 | Stub 交来的读盘服务（实模式弹跳） | 固件块设备协议；被加载设备是分区时沿设备路径上溯到整盘 |
| 内存图 | Stub 从 E820 取 | 固件 `GetMemoryMap`；只把常规内存与启动服务 / 引导器内存在退出后归为可用 |
| 控制台 | VGA 文本 + 0xE9 + COM1 | 固件控制台（退出启动服务前）+ COM1 + 0xE9（退出后） |
| 刷新原语 | 自建 AHCI / 传统 IDE 探测 | 固件的 `FlushBlocks`，不自建控制器路径 |
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
| 上游复用 | 存储抽象与 Print 前端、Ext4 只读实现、最小 C 运行库、`SelectSet` / `ManifestVerify` / `LoadKernel` / 信任锚与吊销记录，全部按 **x64 重编同一份源码**（`Packages/Common` 的默认构建面向 32 位阶段，不能混链）；共享实现里的诊断编号序号不变，前缀由 `-DBALEEN_STAGE_PREFIX='"E-UEFI"'` 换成 E-UEFI |
| ESP 组装 | `Efi.img` 由 `Tools/Bin/MakePayloads --esp-image` 组装：FAT16 的布局只有那一份实现，本包不另写一份 |

## 介质视图为什么要上溯

布局描述固定在**介质**字节偏移 `0x20000`，而固件加载 `\EFI\BOOT\BOOTX64.EFI` 时给出的设备句柄是**它所在的分区**：分区上的块设备以分区起点为块 0。因此取到分区设备后要沿设备路径上溯一层，用 `LocateDevicePath` 拿到整盘的块设备协议，再读描述符——否则读到的是分区里的文件系统数据，布局描述校验会报 `E-UEFI-18`。

被加载的设备本来就是整个设备时（例如没有分区表的介质）直接用，不上溯。

## 启动来源：内核怎么知道“自己是从哪儿起来的”

交接时 `BootInfo` 带上启动来源（字段表见 [Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 6.1）：布局描述偏移、系统卷偏移与长度、状态区域偏移与长度、介质容量、卷标识、内核路径，以及启动设备线索与引导器自身的占用区间。**运行期更新据此把新内核集写进同一个卷、发布到同一条路径、把状态记录写回同一处区域**，不需要到设备里猜。

- **设备线索**：抄的是分区句柄的设备路径（`PciRoot(...)/Pci(...)/Sata(...)/HD(...)`），在退出启动服务之前**抄进本映像的静态缓冲**，之后不再依赖固件内存。
- **长度上限 512 字节**：放不下时线索按“未提供”交接并报 `E-UEFI-83`（WARN，不阻断），内核改用卷标识核对。
- **占用区间**：取 `LoadedImage.ImageBase/ImageSize`，内核不得把本映像的残留当成可用内存。

## 输出通道

- **固件控制台**（简单文本输出协议）：给人看的主通道。固件自己把它镜像到 QEMU 给的串口上（OVMF 在有 `-serial` 时就会），所以本阶段**不再直接写 COM1**——两个通道写同一个 UART 会让每行重复。
- **0xE9 调试口**：另一条独立通道，给 QEMU / Bochs 的 `-debugcon` 用；真实硬件上写入被忽略。

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
[ OK ] The kernel image is valid
[ OK ] The kernel image is loaded
[ OK ] The firmware boot services are exited
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

`make check-all WITH_BOOT=1` 会把这四档（HDD / ISO / U 盘 / 4Kn U 盘）与 BIOS 六档一起跑，每档抽三列；四档的“进内核”都应为“是”。

## 边界

- **不是 Secure Boot 路径**：固件验证引导程序镜像；本产物未签名，开启 Secure Boot 的机器上固件不会执行它，这与本项目无关（见规格 3.4）。
- **只支持线性目录**：Ext4 只读实现拒绝 htree 索引目录、符号链接与内联数据，遇到即按编号报错，不做兼容性猜测。
- **只在 x64 固件上被加载**：32 位固件走 `BOOTIA32.EFI`，那一件只提示、不引导。
- **介质类型按块大小判定**：`BootInfo` 的 `media` 字段取“光盘为 2048 字节块、其余为硬盘”，这是交接事实里唯一靠推断的一项。
- **没有自建控制器路径**：规格 4 允许“固件覆盖不到该控制器时走 Baleen 的最小控制器路径”，本包未实现——UEFI 固件普遍为 NVMe / AHCI 提供块协议，而本机造不出“固件故意不提供块协议”的用例，写了也无法验证。真遇到这种固件时，失败点是 `E-UEFI-16`，现场明确。
- **不报引导耗时的情形**：CPUID 给不出频率、且 8254 实测的 TSC 频率落在 100 MHz..100 GHz 之外时，报 `Verdict=NoClock`，不报 `E-UEFI-55`——宁可说没有校准源，也不报一个假数字。
- **校准时机**：8254 的实测在校验退出启动服务之后做，那时定时器归本阶段使用，不与固件抢。
