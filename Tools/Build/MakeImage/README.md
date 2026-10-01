# MakeImage — 通用镜像制作器

MakeImage 是宿主侧的构建期工具，也是本项目的**通用镜像制作器**：把一级引导、实模式服务层（Stub）、核心阶段（BaleenCore）、EFI 引导镜像（光盘 El Torito 用的小 FAT 镜像与 ESP 分区内容各一份）与已装配好的 Ext4 系统卷镜像组装成**一份可引导镜像**，默认产出 `Out/LikesProgram.iso`。

这份镜像按"一份字节、多种介质"设计，覆盖下面的投递方式，不需要分别组装。"实测"指在本机 QEMU 上启动到 Baleen Core（BIOS 路径）或执行 EFI 引导镜像（UEFI 路径）：

| 目标介质 | 逻辑扇区单位 | BIOS 路径 | UEFI 路径 |
| --- | --- | --- | --- |
| CD / DVD / 虚拟机直接选择 ISO 文件 | 2048（固定） | 实测到 Core | 实测（El Torito） |
| U 盘 / 移动硬盘（第三方刻录器写入） | 512e | 实测到 Core | 实测（1 MiB 处的 ESP） |
| U 盘 / 移动硬盘（第三方刻录器写入） | 4Kn | 取决于固件：SeaBIOS 等不枚举 4Kn USB 存储，设备不可见；逻辑层由 `CheckIpl` 的 AH=48 夹具覆盖 | 实测（8 MiB 处的 ESP，**同一份镜像**） |

**双 ESP 让同一份镜像通吃两种设备单位**：0xEF 分区项的起点固定写 LBA 2048，512e 固件读到 1 MiB、4Kn 固件读到 8 MiB，两处各放一份同样的 FAT16 卷；BIOS 路径本来就不依赖分区表（位置字段与设备单位无关，装载方自行换算）。因此一份 `LikesProgram.iso` 刻到光盘、写入 512e 或 4Kn 设备都能启动，**不需要按目标设备重新组装**。

用 QEMU 复现：512e 用 `make run iso qemu usb bios`；4Kn 用 `make run iso qemu usb uefi RUN_USB_BLOCK_BYTES=4096`（`RUN_USB_BLOCK_BYTES` 让虚拟机把 U 盘挂成 4Kn 设备；SeaBIOS 不枚举 4Kn USB，该用例必须配 `uefi` 固件）。

它属于 [Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 第八节的镜像组装环节，产物形态见[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md)第二节。

工具不解析 Ext4、不装配系统卷内容，也不生成 `.os` / `.osm`：它只负责介质上的位置安排与引导结构。构建期工具用宿主格式，不属于目标系统的容器体系。整盘磁盘镜像（MBR + ESP + Ext4）由[测试安装器 TestInstaller](../TestInstaller/README.md) 产出，两者共用 `../Common` 里的描述符区、El Torito 与 MBR 代码。

## 一、构建

```
make            # 产出 ../../Bin/MakeImage（中间产物在 Obj/；g++，C++20，共享代码在 ../Common）
make clean      # 删除 Obj/ 与本工具的可执行文件（Bin 与 TestInstaller 共用，不删别人的）
```

正常构建只依赖正式 C++ 源码，不依赖未交付的本地回归夹具。本地回归覆盖：MBR 分区项与 0xAA55、描述符区的 Stub 段、El Torito 校验和与两个引导项、ISO9660 主卷描述符与目录记录（Rock Ridge NM/PX/TF）、清单与实际字节一致性、两次构建逐字节相同、纯光盘形态，以及坏 MBR/超长 Stub、非 FAT16 的 ESP 与 El Torito 引导镜像这些拒绝路径，并核对超限镜像的扇区计数按上限写、清单标记为截断；装有 xorriso 时用 libisofs 独立读取并提取比对，有权限时再用内核 isofs 挂载核对。

## 二、用法

```
MakeImage --out LikesProgram.iso \
    --boot-image Packages/Baleen/Ipl/Out/Bin/BaleenIPLCd.bin \
    --mbr        Packages/Baleen/Ipl/Out/Bin/BaleenIPL.bin \
    --stub       <BaleenStub.bin> \
    --core       <BaleenCore.bin> \
    --efi        <El Torito 的 EFI 引导镜像：小 FAT 镜像> \
    --esp        <FAT16 的 ESP 镜像> \
    --system-volume <Ext4 系统卷镜像> [--iso-name <ISO 内的文件名>] \
    --file BaleenLayout.bin=<布局描述文件> \
    --manifest Out/LikesProgram.iso.json
```

**系统卷镜像从哪来。** 它是构建期「布局计算与镜像组装」产出的 Ext4 卷，内容与磁盘形态共用同一份卷内布局（见[用户空间与常驻服务](../../../Docs/Specs/用户空间与常驻服务.md)第五节、[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md)第二节）；本工具只把它连续放进 ISO9660 卷并记录字节偏移。该组装环节尚未实现，所以现在要由调用方提供这个镜像。规格固定的是连续性与位置描述（引导器看布局描述，内核对光盘形态用块设备偏移视图），**没有固定它在 ISO 目录里的名字**，`--iso-name` 因此只是构建期选择；若内核最终按 ISO9660 路径去找这个镜像（见[内核架构与自举闭包](../../../Docs/Specs/内核架构与自举闭包.md)的文件系统表），名字需要在规格里定下来。

| 选项 | 说明 |
| --- | --- |
| `--out PATH` | 输出镜像（必需） |
| `--boot-image PATH` | El Torito 默认（BIOS）引导镜像，即 `BaleenIPLCd.bin`（必需） |
| `--load-segment N` | 引导镜像装入段，默认 `0x7C0`，与一级引导的寻址一致 |
| `--mbr PATH` | 混合镜像的 MBR（`BaleenIPL.bin`，512 字节且带 `0xAA55`），写到镜像偏移 0 |
| `--stub PATH` | 实模式服务层，原始扇区放置，位置写进描述符区的 Stub 段 |
| `--core PATH` | 核心阶段，原始扇区放置，位置写进描述符区的 Core 段 |
| `--descs DIR` | 描述符段产物目录（默认 `Packages/Baleen/Common/Out/Descs`）；头部与各段拆开，本工具只回填起止位置 |
| `--efi PATH` | El Torito 平台 0xEF 的引导镜像（供光盘 UEFI 引导）：**必须是 FAT16** |
| `--esp PATH` | 混合镜像的 ESP 分区内容：**必须是 FAT16**，同一份写到 1 MiB 与 8 MiB 两处 |
| `--system-volume PATH` | 构建期装配好的 Ext4 系统卷镜像，作为连续 ISO 文件存放，并写 Ext4 分区项 |
| `--iso-name NAME` | 系统卷在 ISO 内的名字，默认取源文件名；规格未固定该名字，见上 |
| `--file ISO路径=宿主路径` | 附加文件，可重复；中间目录自动建立 |
| `--raw 宿主路径[@偏移]` | 原始载荷，可重复；偏移须 4096 对齐，省略则自动分配 |
| `--sector-bytes N` | Ext4 分区项（`0x83`）的 LBA 单位：`512`（默认）或 `4096`；0xEF 项固定按 512 单位写（双 ESP 布局需要），ISO 结构仍为 2048 字节块 |
| `--volume-id ID` | 卷标识，默认 `LIKESPROGRAM` |
| `--timestamp N` | 卷与目录时间戳（UTC 秒）；默认取 `SOURCE_DATE_EPOCH`，未设则写全零 |
| `--stub-unchecked` | 免除 Stub 头与摘要校验：只给测试夹具与特殊用途，正式构建不要用 |
| `--manifest PATH` | 输出构建清单（JSON） |

不给 `--mbr` 时产出纯光盘形态：引导镜像放在 LBA 0。给 `--mbr` 时产出混合镜像：引导镜像移到 LBA 1，偏移 0 是磁盘一级引导，描述符扇区不变——两种引导路径读的正是同一处字节。

## 三、介质布局

| 位置 | 内容 |
| --- | --- |
| 偏移 0（LBA 0） | 混合镜像：MBR（磁盘一级引导 + 分区表）；纯光盘：CD 引导镜像 |
| 描述符区（字节偏移 `0x22000`） | 描述符区：头部与段序列（当前为 Stub 段与 Core 段），由 `--descs` 的段产物回填后写入 |
| LBA 1 | 混合镜像的 CD 引导镜像（`BaleenIPLCd.bin`，2048 字节） |
| LBA 16/17/18 | 主卷描述符 / El Torito 引导记录 / 卷描述符集结束符 |
| LBA 19 | El Torito 引导目录 |
| LBA 20 起 | 路径表（小端、大端各一份），随后是目录区 |
| 元数据之后 | 原始载荷：Stub、Core 与 `--raw` 条目，各自 4096 对齐连续存放 |
| 1 MiB 与 8 MiB | ESP 的 FAT16 镜像，同一份写两处：0xEF 分区项起点固定 LBA 2048，512e 固件按 512 解释读到 1 MiB、4Kn 固件按 4096 解释读到 8 MiB |
| ESP 在 4Kn 视角下的分区范围之后 | El Torito EFI 引导镜像（位置与分区表无关，由引导目录按 LBA 引用） |
| 其后 | ISO 文件数据：系统卷与 `--file` 条目，按 ISO 路径排序依次连续分配 |

镜像总长覆盖 ISO 结构、描述符区与全部引导载荷，并按 4096 收尾：载荷尾部不足整扇区的部分补零，装载方按整扇区读取不会越过文件末尾。

所有写到清单里的位置都是字节偏移，块设备偏移视图可直接使用。

## 四、写出的引导结构

**描述符区**（写入字节偏移 `0x22000`）：头部与各段来自 `--descs` 的段产物，本工具只回填起止位置，不生成结构。位置字段按 **512 字节基准**写（值 = 字节偏移 / 512），与设备单位无关，装载方按运行期探测到的单位换算——同一份镜像写入光盘、512e 与 4Kn 介质都指向同一处字节位置。

头部：

| 偏移 | 宽度 | 字段 | 值 |
| --- | --- | --- | --- |
| 0 | 4 | magic | `0x43534442`（`BDSC`） |
| 4 | 2 | version | 版本 `1` |
| 6 | 2 | headerBytes | `16` |
| 8 | 4 | areaLba | 本区起始位置，512 字节基准：`0x22000 / 512 = 272` |
| 12 | 4 | areaEndLba | 本区结束位置，512 字节基准，含；由本工具按段序列总长算出 |

段序列（头部之后，按文件名排序）：每段以段头开头——标记 4 字节、版本 2 字节、段长 2 字节（含段头）。Stub 段标记 `0x52445342`（`BSDR`）、Core 段标记 `0x52444342`（`BCDR`），其后各是载荷的起始与结束位置（各 4 字节，512 字节基准，结束含在范围内）。起始位置非 0，换算成本地扇区号后必须大于描述符区扇区号，结束不早于起始，扇区数不超过各自上限。**MBR 里的描述符区位置槽同样是 512 字节基准、保持默认 `272`**，两种引导路径（El Torito 与混合 MBR）读的是同一处描述符区，换算由各自的装载方完成。

**载荷门禁**：与 TestInstaller 同一套判定，实现在 `../Common` 的 `BaleenImage`。`--stub` 与 `--core` 的带头载荷校验头字段、长度、入口前缀、内存跨度与整幅 SHA-256 摘要，通过后把 BuildId 前 4 字节打进构建输出；`MakePayloads` 生成的占位内容同样要过校验，完全没有头的 `PLACEHOLDER-STUB` 占位件放行并提示；两种标识都没有的载荷默认拒绝，`--stub-unchecked` / `--core-unchecked` 显式免除对应载荷的校验（只给测试夹具与特殊用途）。

**El Torito**：校验项（16 字之和为 0，含 `0x55AA`）；默认项平台 BIOS、无仿真、装入段 `0x7C0`、LoadSize = `ceil(引导镜像/512)`；给 `--efi` 时追加平台 0xEF 段首部与 EFI 项，指向该小 FAT 镜像。

**两份 FAT16 镜像、双 ESP 布局。** `--esp`（0xEF 分区内容）与 `--efi`（El Torito EFI 项）是同一份 4 MiB FAT16。选 FAT16 有两个原因：**体积**——0xEF 分区项的起点固定写 LBA 2048，512e 固件读到 1 MiB、4Kn 固件读到 8 MiB，两处各放一份，1 MiB 处那份不得盖住 8 MiB，所以必须小于 7 MiB，而 FAT32 的最小卷是 32 MiB（65525 簇 × 512 字节）；**规范**——UEFI 要求固件支持 FAT12/16/32，FAT16 满足，且 4 MiB = 8192 个 512 字节扇区，El Torito 的 16 位扇区计数按规范就能表达（清单里的 `sector_count_exact` 与 `sector_count_clamped` 因此一致，只有调用方传入更大镜像时才会出现截断提示）。

**混合 MBR 分区表**（仅在给 `--mbr` 时写入，按顺序）：0 号 = ESP（类型 `0xEF`），1 号 = Ext4 系统卷（类型 `0x83`）；起点与长度取实际字节偏移换算的 `--sector-bytes` 指定单位 LBA，CHS 字段写 `FE FF FF`，强制读取方按 LBA 访问。

**Rock Ridge**：主目录 `.` 项带 SP、RR、PX、TF、ER（`RRIP_1991A`），文件与子目录项带 RR、NM、PX、TF；RR 项的标志位按记录里实际存在的字段置位（`PX|NM|TF`）。ISO 标识符统一为 8.3 大写加 `;1`，真实名字由 NM 承载，因此**读取方必须实现 Rock Ridge**，与[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md)第三节"主目录与 Rock Ridge"的要求一致。本写入器不生成 CE 续接项，单条目录记录超过 255 字节会直接报错。

## 五、构建清单

`--manifest` 输出的 JSON 记录所有分配结果，供后续步骤（布局描述生成、写盘工具、验证脚本）使用：

- `total_bytes` / `sector_bytes` / `total_sectors_2048`
- `boot_image`：LBA、偏移、长度、LoadSize（512 字节单位）、装入段
- `boot_catalog`：引导目录的位置
- `descriptor_area`：描述符区偏移、位置字段的基准单位（512）与段产物目录
- `stub` / `core` / `efi_image` / `raw[]`：原始载荷的偏移与长度
- `system_volume`：ISO 路径、偏移、长度
- `files[]`：每个 ISO 文件的名字、偏移、长度

## 六、可复现性

同一组输入产出逐字节相同的镜像：文件按 ISO 路径排序分配、目录记录按标识符排序、日期取 `--timestamp`（或 `SOURCE_DATE_EPOCH`，未设则写全零），不读取宿主文件的时间戳。挂载所需的 PVD 时间字段同样来自该时间戳。

## 七、边界与待定项

- **不生成**描述符区之外的引导期结构：`BaleenLayout.bin` 的字段、落点仍待后续阶段设计。工具把 Core 等载荷的偏移写进清单，等规格确定后由生成器落盘，或直接用 `--raw` 指定位置；IPL 不解析这些内容。
- **不装配系统卷**：Ext4 系统卷镜像（内核槽位、发行内容）与磁盘形态镜像都属「布局计算与镜像组装」的产出，尚未加入；本工具只消费调用方给的镜像并记录位置。
- **混合镜像固定使用 MBR 分区表**：GPT 的主头与标准项区（LBA 1–33）会覆盖 El Torito 引导镜像（字节 `0x800`）与 ISO9660 元数据（主卷描述符固定在 LBA 16），备份表又要占盘尾的文件数据区；需要 GPT 的是安装器装配的磁盘布局，见[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md) 4.2。
- **不做** ISO 之外的分发打包、签名与信任锚、Joliet/UDF 卷、压缩与引导镜像校验。
- **不写** 任何持久写回：镜像是一次性生成的文件，刻录或写入 U 盘、移动硬盘由用户与写盘工具（如 `dd`）负责；面向目标设备的整盘装配由[测试安装器 TestInstaller](../TestInstaller/README.md) 承担。
