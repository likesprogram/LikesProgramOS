# Baleen Stub — 约定与结构

`Stub` 是 BIOS 路径中承接 IPL 的实模式服务层：IPL 把它读到 `0000:7E00` 后直接跳过来，机器此时仍是 16 位实模式；Stub 的入口保存 IPL 交来的介质身份，由自己切到 32 位保护模式，随后自检自身完整性、打开 A20、建立运行期段表与中断表、取内存图、探测并读取启动设备，最后按第二节的契约定位 `BaleenCore`、校验其镜像头与整幅摘要，再把它装到高位、通过交权块交给它。源码分工：`Src/Stub.asm` 与 `Src/Stub.cpp` 是入口与主流程，`Src/SelfCheck.cpp` 是自身完整性自检，`Src/LoadCore.cpp` 是 Core 的描述符、镜像头、装载、摘要校验与交权机制，`Src/Bios.asm` 是 BIOS 服务。`make` 产出 `Out/Bin/BaleenStub.bin`：`objcopy` 之后由 `Tools/Bin/PackImage` 填头里的 BuildId 与 Digest，再由顶层组装进镜像。`Src/Bios.asm` 经实模式弹跳提供扇区探测、E820 内存图与低地址读盘，磁盘服务只读不写；读盘批量失败后从原始 LBA 与目标地址逐扇区重读，HDD 单扇区在 `LBA<63` 时保留 CHS 回退，服务只接受低 1MiB 的目的地，需要高位缓冲的调用方要自备中转。第三节的 `BaleenStubHeader` 随镜像发布，字段由链接器与打包器共同填出；它与 Core 的镜像头同构，布局的单点定义在 `Packages/Baleen/Common/Include/ImageHeader.hpp`。`Include/Const.inc` 只放本包私有常量，交权契约常量取自 `Packages/Baleen/Common/Include/Contract.inc`。

IPL 的当前行为以 [Baleen 一级引导契约](../../../Docs/Specs/Baleen/一级引导契约.md) 为准；整个引导器的职责见 [Baleen 引导器](../../../Docs/Specs/Baleen/README.md)。本文的 `BaleenStubHeader` 与介质上的 `BootDescriptor` 是两个独立结构；头里的 `Version=1` 与 `HeaderBytes=0x80` 标识当前格式。

**源码分工是一条硬约束**：`_Stub_Main` 承担全部启动流程与全部诊断输出——步骤顺序、进度行、失败原因与停机都在这一段里，步骤变多也不会散到别的文件；`Src/LoadCore.cpp` 只提供机制（读描述符与镜像头、查内存图、挑弹跳窗口、高位拷贝、摘要校验、填交权块），不打印也不停机，失败只回报原因文本。文件与类型名取「装载 Core」这个动作，避免与 Baleen 的 `Core` 阶段混同；`Src/Bios.asm` 同理，只做固件调用，不做诊断排版。

## 一、职责与进入条件

IPL 校验 `BootDescriptor` 并读入 Stub 文件，然后固定跳转到 `0000:7E00`。它不解释任何 Stub 头，也不按头中的入口字段跳转。进入时 `DS=07C0`、`ES=0`、`SS:SP=0000:7C00`，`DL` 为实际读取驱动器号，`DH=2` 表示 HDD / USB-HDD、`DH=4` 表示 CD，`IF=1`、`DF=0`；其他寄存器、其他标志及 A20 状态不承诺。

描述符位于介质绝对偏移 `0x300`。其读盘缓冲在装入 Stub 时被覆盖，不能假设收到持久有效的描述符指针。Stub 必须保存仍需使用的驱动器与介质身份，并自行建立数据段、内存占用和运行环境。

Stub 的职责与实现位置：

- 完成 BSS 初始化，在实模式下按链接脚本给出的边界清零；校验自身完整性头字段与整幅镜像的摘要，并核对头里的 `ImageBytes`、`MemoryBytes` 与实际链接布局一致（三、四节）。
- 建立并核验 A20 状态、取得内存图、维护 BIOS 磁盘会话及必要的调用状态；读盘是有界的两层策略：批量 EDD 请求各自至多 3 次尝试，失败时按会话一次的复位策略处理；某批耗尽后从原始 LBA 与目标地址把整段范围逐扇区重读，单扇区请求在 HDD 且 `LBA<63` 时保留 `AH=02h` 的 C0H0 回退，失败只返回 0。`Src/Bios.asm` 提供扇区探测、E820 内存图与读盘，`Platform::Cpu` 提供 A20 开关与模式识别。
- 按第二节的契约读取 `CoreDescriptor` 与 Core 镜像头、校验装载边界与整幅摘要，把 `BaleenCore` 装到高位、清零头声明的未落盘尾部并交权；Core 内部的代码、段与运行期内存仍由 Core 自己负责。
- 无法完成必要初始化或装载时报告本阶段错误并停止；诊断行按[输出通道与Print](../../../Docs/Specs/输出通道与Print.md)的标签约定输出，失败以 `FATAL` 标签给出原因文本后停机。

`BaleenLayout.bin` 的系统卷语义、Ext4、内核集选版、签名策略和引导状态持久化属于 Core / UEFI。Stub 不承担这些策略。它对自身与 Core 的完整性检查都不是签名认证，也不等于实现了内核集签名链。

## 二、Core 的介质定位、装载与交权

这一节是 Stub → Core 的当前 ABI，两侧由 `Packages/Baleen/Common/Include/Contract.inc` 的 `CORE_*` 常量、`CoreHandoff.hpp` 的交权块结构与 `ImageHeader.hpp` 的镜像头布局共同约束，改一处必须两边同时改。与 `BootDescriptor` 一样，本节格式的改动必须两侧同步。

### 2.1 介质上的 CoreDescriptor

`CoreDescriptor` 固定 32 字节，位于**整个引导介质的绝对字节偏移 `0x340`**，与 `BootDescriptor` 落在同一扇区：HDD 是 512 字节 LBA 1，CD 是 2048 字节 LBA 0。Stub 只读这一扇区就能同时拿到两个描述符，不额外读盘。

| 偏移 | 字节数 | 字段 | 当前要求 |
| --- | --- | --- | --- |
| `0x00` | 4 | Magic | `0x52444342`，介质字节为 `BCDR` |
| `0x04` | 2 | Version | 当前值 `1` |
| `0x06` | 2 | HeaderBytes | `32` |
| `0x08` | 8 | CoreFileOffset | 介质绝对字节偏移；高 32 位为 0；低 32 位非零且按本地扇区对齐 |
| `0x10` | 8 | CoreImageBytes | 文件字节数；高 32 位为 0；非 0 且不超过 `0x400000` |
| `0x18` | 8 | Reserved | 8 字节全部为 0 |

校验与一级引导对 `BootDescriptor` 的校验同构：格式标记、版本与头长、保留字段、高 32 位、扇区对齐、起点落在描述符扇区之后（HDD `>= 0x400`、CD `>= 0x800`），以及偏移加长度不进位。任一项不通过即打印原因并停机，不装载、不跳转。

**`CoreDescriptor` 只定位文件。** 装载地址固定，入口与内存跨度来自 Core 镜像头（2.3）；描述符不装这些字段，也不装摘要与签名：它们由镜像头承载、由 Stub 校验，把策略性字段塞进描述符会与后续阶段的设计冲突。

### 2.2 装载与交权

| 项 | 约定 |
| --- | --- |
| 镜像形态 | 直接装入的平坦镜像：文件偏移 0 起是入口前缀与完整性头，`0x90` 起是代码；不使用压缩、重定位或 ELF 头 |
| 装载地址 | 物理 `0x100000`（1MiB），即镜像基址；交权入口 = 基址 + 头里的 `EntryOffset`（当前 `0x90`） |
| 装载前检查 | 读入跨度与头里 `MemoryBytes` 的较大者必须整段落在 E820 可用区；不假定 1MiB 以上一定可写 |
| 装载方式 | 读盘只能落低 1MiB，高位因此分两步：先读进低地址弹跳窗口，再用 32 位平坦段整段拷上去。窗口从 `0x10000` 起按 64KiB 步进，取第一段整段可用者；不引入 unreal 模式，也不要求固件认识高位缓冲 |
| 尾部填充 | 按扇区上取整读入，最后一个扇区的填充也会写进内存；Core 不得把填充当成自己的内容 |
| 未落盘尾部 | 头里的 `MemoryBytes` 超出文件的部分由 Stub 在交权前清零；Core 不得依赖残留内容 |
| 进入状态 | 32 位保护模式、平坦段（`CS=0x08`、`DS/ES/SS=0x10`）、分页关闭、`IF=0`、`DF=0`；`ESI` 指向交权块，`ESP` 仍指向 Stub 的保护模式栈（栈顶 `0x7000`，Core 应在做重活前换成自己的栈） |
| 返回 | 不返回。Core 一旦进入即持有机器控制权；Stub 冻结常驻，其静态内存、交权块与固件服务在 Core 运行期间持续有效 |
| 失败 | 描述符或镜像头非法、摘要不符、装载区不可用、找不到弹跳窗口、读盘失败各自打印原因并停机；**绝不带着未检查的内容跳转** |

### 2.3 Core 镜像头

`BaleenCore.bin` 与 Stub 同构：文件偏移 `0` 起是 16 位近跳转前缀与保留区，`0x10` 起是 128 字节完整性头，`0x90` 起是代码；两种镜像只有格式标记不同（Core 为 `BLNCORE`），字段偏移、固定取值与摘要覆盖区间完全一致，字节表见第三节。布局的单点定义在 `Packages/Baleen/Common/Include/ImageHeader.hpp`，目标侧的字段校验与摘要比对在 `Packages/Baleen/Common/Src/ImageHeader.cpp`。

Stub 分两步核对，任一项不通过都停机，不跳转：

1. **装载前**读文件首扇区核对头字段：格式标记、版本、头长、标志、摘要算法、`BuildId` 长度、保留区、入口前缀与 `EntryOffset` 一致；头里的 `ImageBytes` 必须与 `CoreDescriptor` 的文件长度相等，`MemoryBytes` 必须覆盖文件且不超过 `0x400000`。
2. **读入高位后**比对整幅摘要：按第四节的覆盖区间把 `Digest` 字段视作 32 个零，与头里的摘要逐字节比较；随后按 `MemoryBytes` 清零未落盘尾部，入口取 `CoreLoad + EntryOffset`。

头里的 `ImageBytes`、`MemoryBytes`、`EntryOffset` 与 `Digest` 都由打包器填出：正式 Core 走 `Tools/Bin/PackImage`，占位 Core 由 `MakePayloads` 组装同样的头并用同一套规则填充。头之后的代码、段与 Core 内部结构不由 Stub 解释。

一条边界要写清：**头没有被认证**。改内容并同时重算摘要的文件能通过这套校验，它检测的是非预期字节变化，不是执行前认证；信任锚仍未定义，见第六节。

### 2.4 交权块

只跳转不构成交权：Core 需要介质身份、内存图与可用的固件服务入口，而 `BootDescriptor` 缓冲区早已被 Stub 覆盖。这些事实由交权块传递，定义在 `Packages/Baleen/Common/Include/CoreHandoff.hpp`，长度固定 64 字节；入口时 `ESI` 指向它，指针是物理地址（分页关闭，虚拟地址等于物理地址）。

| 块内偏移 | 字段 | 含义 |
| --- | --- | --- |
| `0x00` | Magic / Version / Bytes | `0x484E4C42`（`BLNH`）/ `1` / `64`；Core 先核对这三项再读取其余字段 |
| `0x0C` | Drive | 启动驱动器的 BIOS 号，即 IPL 交权时的 `DL` |
| `0x10` | Media / SectorBytes | 介质编号（同 `Boot::Media`）与本地扇区大小，读盘的块数以后者为单位 |
| `0x18` | CoreBytes / CoreSectors | Core 的文件字节数与按扇区上取整后的读入跨度 |
| `0x20` | CoreLoad | Core 的装入物理地址，即镜像基址；本次入口 = 它 + 镜像头里的 `EntryOffset` |
| `0x24` | MemoryMapCount / MemoryMapTruncated | 内存图条数与截断标志 |
| `0x2C` | MemoryMap | 指向 Stub 静态缓冲里的 E820 条目数组，条目布局见 `Boot::MemoryMapEntry` |
| `0x30` | Write | 写入 Stub 的控制台（VGA 文本、`0xE9` 与 COM1），Core 不必自己初始化输出 |
| `0x34` | ReadSectors | 读盘 `(lba, count, dest, sectorBytes)`：成功返回 1，失败返回 0；`dest` 须 16 字节对齐且落低 1MiB |
| `0x38` | SectorSize | 探测启动驱动器的扇区大小 |
| `0x3C` | MemoryMapQuery | 重新取内存图，签名与语义同 `_Bios_E820` |

服务入口是 Stub 里的函数地址：Stub 常驻不动，Core 在保护模式下直接调用，由 Stub 负责弹回实模式、还原现场再返回。**这些服务不改变所有权**：Core 不能要求 Stub 在交权后做别的工作，Stub 也不再观察 Core 的状态。

块的指针宽度与字段偏移由 `CoreHandoff.hpp` 的 `static_assert` 在 32 位目标上固定；占位 Core 的汇编常量由 `Tools/Build/MakePayloads` 从同一头文件取值拼出，两侧不会各写一份。

## 三、镜像完整性头（BaleenStubHeader / BaleenCoreHeader）

### 3.1 生成责任

整数的字节序与填充由字节表固定：均以小端序逐字节编码，不允许编译器插入填充；解析时须核对格式标记、头长、摘要算法与保留字段，不能对不认识的结构猜测读取。各字段的填出方：`Src/Stub.asm` 组装 Magic、版本、头长、标志、入口、算法与保留字段；链接器按 `__image_end` 与 `__bss_end` 填 `ImageBytes` 与 `MemoryBytes`；`Tools/Bin/PackImage` 填 `BuildId` 与 `Digest`。运行期由 `Src/SelfCheck.cpp` 校验，宿主侧由 `Tools/Build/Common` 的 `BaleenImage` 供 PackImage 与两个组装器共用；Core 的镜像头同构，见 2.3。

`Version=1` 与 `HeaderBytes=0x80` 标识当前格式；格式变动须同步上述各处。

### 3.2 固定入口与文件布局

所有范围以 Stub 文件起点为 0，右端不含。

| 文件范围 | 内容 | 约束 |
| --- | --- | --- |
| `[0x00, 0x03)` | 16 位 `JMP rel16` 入口前缀 | 从 IPL 固定进入的文件偏移 0 跳到 `EntryOffset`；位移相对于该指令末端，汇编期由本行位置直接算出，加载后由自检、打包期由 PackImage 分别核对它与头字段一致 |
| `[0x03, 0x10)` | 前缀保留字节 | 全 0 |
| `[0x10, 0x90)` | `BaleenStubHeader` | 固定为 128 字节，字段偏移在汇编期与 C++ 侧各有断言 |
| `[0x90, ImageBytes)` | 早期初始化代码、其余代码与落盘数据 | 包含 `EntryOffset` 指向的入口；直接装入的平坦镜像，不使用压缩或重定位；落盘部分保持只读，见 3.4 |
| `[ImageBytes, MemoryBytes)` | 未落盘的静态内存尾部 | 包含 BSS 与所需静态对齐空间，由 Stub 验证范围并在实模式下清零 |

Core 的文件布局同构，只有格式标记与入口之后的代码不同，见 2.3。前缀是 Stub 的可执行内容，解决“固定 `0x7E00` 入口与数据头共存”的布局问题。IPL 不识别前缀、不检查其位移、不查找头。`EntryOffset` 是 Stub 自己的早期入口元数据，不是新的 IPL 交权地址，也不是 Core 的入口。

链接脚本把代码与只读数据、可写数据分别声明为两个权限不同的 `LOAD` 段（`R E` 与 `RW`），链接以 `--fatal-warnings` 执行；产物是 `objcopy` 出的扁平镜像，段边界不落盘，装载后的内存跨度仍由头里的长度字段与低 64KiB 窗口约束决定。

### 3.3 头字段字节表

下表偏移相对于头起点（文件偏移 `0x10`）；“填出方”一列说明该字段的来源。同一套字节表适用于 Stub 与 Core 两种镜像，差异只有 `Magic`：Core 为 `BLNCORE` 加终止零，其余字段的语义、填出方与约束相同。

| 头内偏移 | 字节数 | 字段 | 当前语义 | 填出方 |
| --- | --- | --- | --- | --- |
| `0x00` | 8 | Magic | 字节 `42 4C 4E 53 54 55 42 00`，即 `BLNSTUB` 加终止零 | 汇编常量 |
| `0x08` | 2 | Version | 当前值 `1` | 汇编常量 |
| `0x0A` | 2 | HeaderBytes | `0x80` | 汇编常量 |
| `0x0C` | 4 | Flags | `0`，当前不定义可选标志 | 汇编常量 |
| `0x10` | 4 | ImageBytes | Stub 整个文件的实际字节数，含入口前缀、完整头、代码与落盘数据；不含扇区填充和 BSS | 链接器按 `__image_end` |
| `0x14` | 4 | MemoryBytes | 从物理 `0x7E00` 起的静态内存跨度，包含文件和未落盘尾部；不包含外部栈、内存图缓冲、磁盘缓冲或 Core 的单独分配 | 链接器按 `__bss_end` |
| `0x18` | 4 | EntryOffset | 从文件起点算起的早期初始化入口偏移；须在 `[0x90, ImageBytes)`，且与入口前缀一致 | 汇编常量 |
| `0x1C` | 2 | DigestAlgorithm | `1` = SHA-256；其他值拒绝，无“禁用摘要”值 | 汇编常量 |
| `0x1E` | 2 | BuildIdBytes | `32` | 汇编常量 |
| `0x20` | 32 | BuildId | 本次构建的标识，不透明字节串；全零保留为“未分配”，正式产物不得使用 | PackImage 按第五节规则填入 |
| `0x40` | 32 | Digest | 按第四节计算的 SHA-256 原始 32 字节摘要，不是十六进制文本 | PackImage 最后填入 |
| `0x60` | 32 | Reserved | 全 0 | 汇编常量 |

约束至少包括 `0x90 < ImageBytes <= MemoryBytes`、`MemoryBytes <= 0x8200`，以及入口落在文件已初始化区域。`ImageBytes` 的介质限制仍为 HDD `<=0x8200`、CD `<=0x8000`；用于两种入口的同一文件按 CD 上限检查。Core 镜像的同一组约束把上限换成 `0x400000`（`MemoryBytes` 上限同为 `0x400000`），并去掉低 64KiB 窗口项，见 2.3。所有加法和上取整均先检查溢出。

实际占用必须按 `max(MemoryBytes, ceil(ImageBytes / SectorBytes) * SectorBytes)` 预留，并保持 `0x7E00 + 实际占用 <= 0x10000`。完整扇区读取可能已写入 BSS 开头，Stub 仍须按定义清零，不能依赖介质填充碰巧为零。不得覆盖 `[0x7C00, 0x7E00)` 的 IPL；额外栈与其他缓冲必须另行依据内存图和所有权规则检查。

`PackImage` 在构建期核对 `ImageBytes` 与实际文件长度、长度上限、入口前缀与 `EntryOffset` 一致、`MemoryBytes` 的落点约束以及整幅摘要；`Src/SelfCheck.cpp` 在运行期把 `ImageBytes`、`MemoryBytes` 与链接脚本符号 `__image_end`、`__bss_end` 比对，头字段与实际布局不符时在取摘要之前就拒绝。

### 3.4 装入后镜像只读

摘要自检读的是刚装入的镜像字节，因此落盘区必须保持只读：把运行期状态写进落盘区会让自检看到的内容与打包时不同，不能靠忽略变化的字节回避。为此：

- IPL 交权的 `DL`/`DH` 写进未初始化区的 `_Boot_Drive` / `_Boot_Media`；
- 控制台与 VGA 设备带虚表、且 VGA 有非零初值，必须放在未初始化区的存储上、由 `InitConsole` 就地构造，不能作为普通全局对象落进数据区；
- 两处段表都只在镜像里留只读模板：入口的最小段表与 BIOS 弹跳段表都在使用前把副本拷进未初始化区再加载。CPU 加载段选择子时会置位描述符的 Accessed 位并写回内存，模板若直接加载就会改写镜像；
- 实模式清零未初始化区的动作排在写段表副本之前，否则副本会被清零覆盖。

## 四、摘要覆盖与自引用处理

设文件的原始字节串为 `F`、实际长度为 `ImageBytes`。摘要字段位于文件 `[0x50, 0x70)`，计算时只将这 32 字节视为零：

```text
Digest = SHA-256(F[0x00:0x50] || 32 个零字节 || F[0x70:ImageBytes])
```

这里的切片右端均不含，`||` 表示字节串拼接。`Magic`、版本、长度、入口、BuildId、所有保留字节、入口前缀、代码和落盘数据均被覆盖；只有摘要本身按规则归零，避免自引用。SHA-256 的标准依据为 NIST [Hash Functions](https://csrc.nist.gov/projects/hash-functions) 与其列出的 [FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final)；目标侧实现在 `Packages/Baleen/Common/Src/Sha256.cpp`，宿主侧实现在 `Tools/Build/Common/Src/Sha256.cpp`。

覆盖范围不包含介质上的 `BootDescriptor`、最后一个本地扇区的文件外填充、BSS、其他运行时缓冲、Core、`BaleenLayout.bin` 或内核集。这些对象不能因 Stub 摘要通过而被声称已验证。

打包顺序：链接器填好长度字段、`objcopy` 产出平坦镜像、`PackImage` 先按第五节规则填 `BuildId`，再把 `Digest` 置零并按上式填入摘要，最后复核整幅镜像；占位 Core 由 `MakePayloads` 组装同样的头并调用同一套填充与校验。摘要写入后修改任何被覆盖字节都必须重跑打包器；校验器流式代入零字节，不改写被校验的代码或头。

运行期自检顺序：先核对格式标记、版本、头长、标志、摘要算法、BuildId 长度与保留字段，再核对 `ImageBytes`、`MemoryBytes` 与链接布局、静态内存落在低 64KiB 窗口内、入口前缀与 `EntryOffset` 一致，最后流式代入 32 个零字节计算 SHA-256 并与头里的 `Digest` 比对；实现在 `Src/SelfCheck.cpp`。任一项不通过都打印 `FATAL` 原因并停机，不带着未通过校验的镜像继续。Core 镜像的同类检查在 `Src/LoadCore.cpp`，由 Stub 在交权前执行（2.3）。

自校验针对**刚装入、尚未改写的文件字节**进行，3.4 节的只读约束保证这一点。自修改、重定位或把临时状态写进被覆盖数据区都会改变输入，实现不允许靠忽略这类字节来维持校验。

## 五、Baleen BuildId 的归属

这里的 BuildId 标识 **Baleen 引导器构建**。它用于追踪 Stub 及未来与之配套的 Baleen 组件、检查打包时的构建配对，不能与 `.os`、`.osm` 和内核集构建清单的 BuildId 混用；后者的定义见 [内核集更新与回滚](../../../Docs/Specs/内核集更新与回滚.md)。

BuildId 由 Baleen 构建阶段分配，32 字节按原样比较；应绑定确定的源码、配置与构建输入，并支持相同输入下的可复现构建。BuildId 必须先于文件摘要确定，不能从包含自身字段的最终文件摘要反推；不为它引入第二个自引用问题。

`PackImage` 把 `BuildId` 与 `Digest` 两字段代入 32 个零后计算镜像的 SHA-256，取其 32 字节作为 `BuildId`，再用已填入的 `BuildId` 计算最终 `Digest`。输入是打包器看到的整个镜像文件 `[0, ImageBytes)`，规范化编码就是镜像本身；派生规则、可复现要求与配对规则见 [PackImage 说明](../../../Tools/Build/PackImage/README.md) 的填充规则一节。这条规则随源码与配置变化、同一输入可复现，但不构成来源证明；调整规则时改 `PackImage` 与本节，不动目标侧。

Stub 与 Core 的 BuildId 各自独立，且都覆盖在各自摘要的范围内；两者相同不证明接口兼容、内容完整或来源可信。内核集的选择与签名仍由 Core / UEFI 处理。

## 六、检查责任与信任边界

| 执行方 | 检查责任 | 限制 |
| --- | --- | --- |
| IPL（当前） | `BootDescriptor` 结构、文件偏移与长度、全部必要扇区的读盘结果 | 不读取 Stub 头、不检查 BSS、BuildId 或摘要，不改变交权 ABI，也不读 `CoreDescriptor` |
| 打包与组装器（当前） | `PackImage` 填 Stub 与 Core 的 BuildId 与 Digest 并校验头字段、长度、入口前缀与摘要；`MakeHdd` / `MakeIso` 对带头 Stub 与 Core 复核同样内容并把 BuildId 打进构建输出；`MakePayloads` 组装的占位 Core 带头并经同一套校验 | 只做构建期检查；无头的开发占位件与 `--stub-unchecked` / `--core-unchecked` 免除校验的载荷不构成对正式产物的检查 |
| Stub（当前） | 先自检头字段、布局一致性与摘要，再有界检查 `CoreDescriptor` 与 Core 镜像头、校验装载区落在 E820 可用区与 Core 摘要，随后装载、清零未落盘尾部并交权 | 描述符缓冲已被覆盖，不能假设能从传入指针独立复核 IPL 实际读取的文件字节数；自检只覆盖 Stub 自己的镜像字节 |
| Core（未来） | 核对交权块的格式标记与长度；镜像头与摘要已由 Stub 在交权前核对，不重复取摘要；交权块里没有的项自行探测或拒绝，并核对自身运行期内存需求 | 不能从文件名或寄存器残留推导事实；不把 Stub 已做的核对当成对运行期环境的保证 |

组装器必须保证 `BootDescriptor.StubImageBytes` 与实际 Stub 文件长度一致；`CoreDescriptor` 的字段与 Stub 读取时执行的校验一致（`EncodeCoreDescriptor` 与 `Src/LoadCore.cpp` 各有一份，取值同源）。Stub 的本地检查可使用 `DH` 选择文件上限，但它没有来自 IPL 的持久描述符副本；这不构成对原描述符和镜像匹配关系的独立证明。若未来需要重新读取并核对描述符，其过程、缓冲和失败语义另行设计，不能暗增 IPL 参数。

**Stub 自校验发生在 IPL 已经跳入并执行 Stub 之后，不能构成执行前认证，也不是信任根。** Core 的摘要由 Stub 在跳转前比对，检测的同样是非预期字节变化：攻击者若能同时改写 Core 与头里的摘要就能绕过。无密钥摘要最多用于检测非预期字节变化；真实的执行前认证需要可信的更早阶段或外部认证链，该方案尚未定义，不以扩大 IPL 职责冒充已经解决。

IPL 没有验头不等于缺陷，这是现有边界。Stub 因自身头、BSS、A20、内存图，或 Core 的头、摘要与装载失败，应报告为 Stub 阶段错误；不能把错误归到 IPL，也不能失败后继续使用未检查的 Core。

## 七、尚待后续设计与实现

| 项目 | 当前尚未确定或实现的内容 |
| --- | --- |
| Core 的镜像内结构 | 完整性头与入口前缀已定（2.3，与 Stub 同构）；头之后的代码、段、重定位与对齐仍由 Core 后续自行定义 |
| Stub 实模式服务 | 磁盘服务只读不写，写盘需求由 Core 侧自行实现；读盘服务只接受低 1MiB 的目的地，需要高位缓冲的调用方要自备中转；两层读盘回退与 HDD 的 `AH=02h` 回退是项目的有界重试策略，不承诺无 EDD 或任意几何的机器都能读完整段。A20 与模式识别由 `Packages/Common` 的 `Platform::Cpu` 提供 |
| Baleen 构建元数据 | BuildId 的派生规则、输入与规范化编码已定（第五节与 [PackImage 说明](../../../Tools/Build/PackImage/README.md)）；统一清单与 UEFI 携带方式仍未定 |
| 验证与认证 | 执行前认证及其信任锚未定义，无密钥摘要不是信任根；头字段与摘要的构建期、运行期检查见第三、四节 |
| Core / UEFI 的上层职责 | `BaleenLayout.bin` 详细格式与位置、Ext4 实现、内核集验证和选版需要各自的后续实现 |

不得仅为满足头格式而在 `Stub/Out/Bin/` 生成同名空壳或停机件，让顶层“真实产物优先”逻辑误把它当成正式实现。`Tools/Build/MakePayloads` 生成的占位 Core 是一段真正的 32 位映像：它带完整性与头入口，经 Stub 校验头与摘要后被装入，再经交权块调用 Stub 的控制台打印标识后停机，用来证明定位、装载、校验与交权真的走通；它不是 Core 实现，不代表 Baleen 已能引导内核。

正常构建只能依赖正式源码与工具，不依赖不上传的 `Test` 目录或本地测试脚本；格式与打包检查由 NASM 汇编期断言和正式 C++ 工具承担，额外的本地测试不构成本目录交付所必需的文件。完整测试记录与硬件验证应单独提供，本文不宣称通过。
