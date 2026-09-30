# MakeIso — ISO 组装器

MakeIso 是宿主侧的构建期工具：把一级引导、实模式服务层（Stub）、核心阶段（BaleenCore）、EFI 引导镜像（光盘 El Torito 用的小 FAT 镜像与 ESP 分区内容各一份）与已装配好的 Ext4 系统卷镜像组装成一份可引导的 ISO9660 镜像，并让同一份字节在写入 U 盘后仍带磁盘引导记录（混合镜像）。它属于 [Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 第八节的镜像组装环节，产物形态见[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md)第二节。

工具不解析 Ext4、不装配系统卷内容，也不生成 `.os` / `.osm`：它只负责介质上的位置安排与引导结构。构建期工具用宿主格式，不属于目标系统的容器体系。磁盘镜像（MBR + ESP + Ext4）由 [MakeHdd](../MakeHdd/README.md) 产出，两者共用 `../Common` 里的 BootDescriptor、El Torito 与 MBR 代码。

## 一、构建

```
make            # 产出 ../../Bin/MakeIso（中间产物在 Obj/；g++，C++20，共享代码在 ../Common）
make clean      # 删除 Obj/ 与本工具的可执行文件（Bin 与 MakeHdd 共用，不删别人的）
```

正常构建只依赖正式 C++ 源码，不依赖未交付的本地回归夹具。本地回归覆盖：MBR 分区项与 0xAA55、BootDescriptor 字段、El Torito 校验和与两个引导项、ISO9660 主卷描述符与目录记录（Rock Ridge NM/PX/TF）、清单与实际字节一致性、两次构建逐字节相同、纯光盘形态，以及坏 MBR/超长 Stub、非 FAT32 的 ESP 与 El Torito 引导镜像这些拒绝路径，并核对超限镜像的扇区计数按上限写、清单标记为截断；装有 xorriso 时用 libisofs 独立读取并提取比对，有权限时再用内核 isofs 挂载核对。

## 二、用法

```
MakeIso --out Baleen.iso \
    --boot-image Packages/Baleen/Ipl/Out/Bin/BaleenIPLCd.bin \
    --mbr        Packages/Baleen/Ipl/Out/Bin/BaleenIPL.bin \
    --stub       <BaleenStub.bin> \
    --core       <BaleenCore.bin> \
    --efi        <El Torito 的 EFI 引导镜像：小 FAT 镜像> \
    --esp        <FAT32 的 ESP 镜像> \
    --system-volume <Ext4 系统卷镜像> [--iso-name <ISO 内的文件名>] \
    --file BaleenLayout.bin=<布局描述文件> \
    --manifest Out/Baleen.iso.json
```

**系统卷镜像从哪来。** 它是构建期「布局计算与镜像组装」产出的 Ext4 卷，内容与磁盘形态共用同一份卷内布局（见[用户空间与常驻服务](../../../Docs/Specs/用户空间与常驻服务.md)第五节、[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md)第二节）；本工具只把它连续放进 ISO9660 卷并记录字节偏移。该组装环节尚未实现，所以现在要由调用方提供这个镜像。规格固定的是连续性与位置描述（引导器看布局描述，内核对光盘形态用块设备偏移视图），**没有固定它在 ISO 目录里的名字**，`--iso-name` 因此只是构建期选择；若内核最终按 ISO9660 路径去找这个镜像（见[内核架构与自举闭包](../../../Docs/Specs/内核架构与自举闭包.md)的文件系统表），名字需要在规格里定下来。

| 选项 | 说明 |
| --- | --- |
| `--out PATH` | 输出镜像（必需） |
| `--boot-image PATH` | El Torito 默认（BIOS）引导镜像，即 `BaleenIPLCd.bin`（必需） |
| `--load-segment N` | 引导镜像装入段，默认 `0x7C0`，与一级引导的寻址一致 |
| `--mbr PATH` | 混合镜像的 MBR（`BaleenIPL.bin`，512 字节且带 `0xAA55`），写到镜像偏移 0 |
| `--stub PATH` | 实模式服务层，原始扇区放置，并把偏移/长度写进 BootDescriptor |
| `--core PATH` | 核心阶段，原始扇区放置并写 CoreDescriptor，位置进清单 |
| `--efi PATH` | El Torito 平台 0xEF 的引导镜像（供光盘 UEFI 引导）：**必须是 FAT32** |
| `--esp PATH` | 混合镜像的 ESP 分区内容：**必须是 FAT32**，落到 0xEF 分区项 |
| `--system-volume PATH` | 构建期装配好的 Ext4 系统卷镜像，作为连续 ISO 文件存放，并写 Ext4 分区项 |
| `--iso-name NAME` | 系统卷在 ISO 内的名字，默认取源文件名；规格未固定该名字，见上 |
| `--file ISO路径=宿主路径` | 附加文件，可重复；中间目录自动建立 |
| `--raw 宿主路径[@偏移]` | 原始载荷，可重复；偏移须 4096 对齐，省略则自动分配 |
| `--sector-bytes N` | 混合 MBR 的 LBA 单位：`512`（默认）或 `4096`；ISO 结构仍为 2048 字节块 |
| `--volume-id ID` | 卷标识，默认 `LIKESPROGRAM` |
| `--timestamp N` | 卷与目录时间戳（UTC 秒）；默认取 `SOURCE_DATE_EPOCH`，未设则写全零 |
| `--stub-unchecked` | 免除 Stub 头与摘要校验：只给测试夹具与特殊用途，正式构建不要用 |
| `--manifest PATH` | 输出构建清单（JSON） |

不给 `--mbr` 时产出纯光盘形态：引导镜像放在 LBA 0，引导镜像放在 LBA 0。给 `--mbr` 时产出混合镜像：引导镜像移到 LBA 1，偏移 0 是磁盘一级引导，描述符仍落在绝对偏移 `0x22000`——两种引导路径读的正是同一处字节。

## 三、介质布局

| 位置 | 内容 |
| --- | --- |
| 偏移 0（LBA 0） | 混合镜像：MBR（磁盘一级引导 + 分区表）；纯光盘：CD 引导镜像 |
| 绝对偏移 `0x22000` | BootDescriptor（32 字节） |
| 绝对偏移 `0x22020` | CoreDescriptor（32 字节，与上一个同扇区；Stub 读同一处定位核心阶段） |
| LBA 1 | 混合镜像的 CD 引导镜像（`BaleenIPLCd.bin`，2048 字节） |
| LBA 16/17/18 | 主卷描述符 / El Torito 引导记录 / 卷描述符集结束符 |
| LBA 19 | El Torito 引导目录 |
| LBA 20 起 | 路径表（小端、大端各一份），随后是目录区 |
| 元数据之后 | 原始载荷：Stub、Core、El Torito EFI 引导镜像与 `--raw` 条目，各自 4096 对齐连续存放 |
| 1 MiB（前面已有载荷时顺延） | ESP 的 FAT32 镜像（0xEF 分区项指向它） |
| ESP 之后 | ISO 文件数据：系统卷与 `--file` 条目，按 ISO 路径排序依次连续分配 |

所有写到清单里的位置都是字节偏移，块设备偏移视图可直接使用。

## 四、写出的引导结构

**BootDescriptor**（介质绝对偏移 `0x22000`，小端，与 `Packages/Baleen/Ipl/Include/Const.inc`、`Body.inc` 的常量一致）：

| 偏移 | 宽度 | 字段 | 值 |
| --- | --- | --- | --- |
| 0 | 4 | magic | `0x52445342`（`BSDR`） |
| 4 | 2 | version | 当前格式标记 `1`，尚未发布，开发阶段可直接调整 |
| 6 | 2 | header | `32` |
| 8 | 8 | Stub 偏移 | 低 32 位字节偏移，至少 `0x23000` 且 4096 对齐（光盘 2048 块与 4Kn 存储都能整扇区读取），偏移加文件长度不得产生 32 位进位 |
| 16 | 8 | Stub 字节数 | 非 0，且不超过 `0x8000`（16 个 2048 B 扇区）；纯光盘和混合镜像均受此限制 |
| 24 | 8 | 保留 | 全 0 |

**CoreDescriptor**（介质绝对偏移 `0x22020`，小端，与 `Packages/Baleen/Common/Include/Contract.inc`、`Stub/Src/LoadCore.cpp` 的常量一致）：magic `0x52444342`（`BCDR`）、version `1`、header `32`、8 字节 Core 偏移、8 字节 Core 字节数、8 字节保留；偏移至少 `0x23000` 且 4096 对齐，偏移加长度不得产生 32 位进位，长度非 0 且不超过 `0x400000`。写它的前提是提供 `--core`；缺 Core 时只提示，不写描述符。

**载荷门禁**：与 MakeHdd 同一套判定，实现在 `../Common` 的 `BaleenImage`。`--stub` 与 `--core` 的带头载荷校验头字段、长度、入口前缀、内存跨度与整幅 SHA-256 摘要，通过后把 BuildId 前 4 字节打进构建输出；`MakePayloads` 生成的占位内容同样要过校验，完全没有头的 `PLACEHOLDER-STUB` 占位件放行并提示；两种标识都没有的载荷默认拒绝，`--stub-unchecked` / `--core-unchecked` 显式免除对应载荷的校验（只给测试夹具与特殊用途）。

**El Torito**：校验项（16 字之和为 0，含 `0x55AA`）；默认项平台 BIOS、无仿真、装入段 `0x7C0`、LoadSize = `ceil(引导镜像/512)`；给 `--efi` 时追加平台 0xEF 段首部与 EFI 项，指向该小 FAT 镜像。

**两份 FAT32 镜像、一个规范缺口。** `--esp`（0xEF 分区内容）与 `--efi`（El Torito EFI 项）都写死为 FAT32：UEFI 固件读它们，内核必备的 FAT 实现也是 FAT32。但 El Torito 的扇区计数只有 16 位，上限 65535 × 512 = 33,553,920 字节；而 FAT32 卷的下限（65525 个簇 × 512 字节）约 66,500 个扇区，**该字段按规范表达不了任何 FAT32 镜像**。工具的处理：按上限 65535 写入并在标准错误上提示，清单里同时给出 `sector_count_exact` 与 `sector_count_clamped`。

实测（QEMU + OVMF/EDK2 的本地启动回归）：FAT16 基准、FAT32 且计数写 65535、FAT32 且计数被截断成 4096 三种情形都能挂载 FAT 卷并执行 `\EFI\BOOT\BOOTX64.EFI` —— 说明 EDK2 按 FAT 卷自身尺寸取镜像，不看这个字段。**风险在别处**：若某个固件按字段截断，超过 32 MiB 的镜像会被切掉尾部。要不要接受这个风险由发行侧决定；要完全避开它，只能把 `--efi` 收回成小的 FAT12/16 镜像（那就是规范能表达、发行版通行做法）。

**混合 MBR 分区表**（仅在给 `--mbr` 时写入，按顺序）：0 号 = ESP（类型 `0xEF`），1 号 = Ext4 系统卷（类型 `0x83`）；起点与长度取实际字节偏移换算的 `--sector-bytes` 指定单位 LBA，CHS 字段写 `FE FF FF`，强制读取方按 LBA 访问。

**Rock Ridge**：主目录 `.` 项带 SP、RR、PX、TF、ER（`RRIP_1991A`），文件与子目录项带 RR、NM、PX、TF；RR 项的标志位按记录里实际存在的字段置位（`PX|NM|TF`）。ISO 标识符统一为 8.3 大写加 `;1`，真实名字由 NM 承载，因此**读取方必须实现 Rock Ridge**，与[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md)第三节"主目录与 Rock Ridge"的要求一致。本写入器不生成 CE 续接项，单条目录记录超过 255 字节会直接报错。

## 五、构建清单

`--manifest` 输出的 JSON 记录所有分配结果，供后续步骤（布局描述生成、写盘工具、验证脚本）使用：

- `total_bytes` / `sector_bytes` / `total_sectors_2048`
- `boot_image`：LBA、偏移、长度、LoadSize（512 字节单位）、装入段
- `boot_catalog`：引导目录的位置
- `descriptor`：BootDescriptor 的偏移与 Stub 偏移/长度
- `core_descriptor`（含 Core 偏移与长度）、`stub` / `core` / `efi_image` / `raw[]`：原始载荷的偏移与长度
- `system_volume`：ISO 路径、偏移、长度
- `files[]`：每个 ISO 文件的名字、偏移、长度

## 六、可复现性

同一组输入产出逐字节相同的镜像：文件按 ISO 路径排序分配、目录记录按标识符排序、日期取 `--timestamp`（或 `SOURCE_DATE_EPOCH`，未设则写全零），不读取宿主文件的时间戳。挂载所需的 PVD 时间字段同样来自该时间戳。

## 七、边界与待定项

- **不生成** `BootDescriptor` 与 `CoreDescriptor` 之外的引导期结构：`BaleenLayout.bin` 的字段、落点仍待后续阶段设计。工具把 Core 等载荷的偏移写进清单，等规格确定后由生成器落盘，或直接用 `--raw` 指定位置；IPL 不解析这些内容。
- **不装配系统卷**：Ext4 系统卷镜像（内核槽位、发行内容）与磁盘形态镜像都属「布局计算与镜像组装」的产出，尚未加入；本工具只消费调用方给的镜像并记录位置。
- **混合镜像固定使用 MBR 分区表**：GPT 的主头与标准项区（LBA 1–33）会覆盖 El Torito 引导镜像（字节 `0x800`）与 ISO9660 元数据（主卷描述符固定在 LBA 16），备份表又要占盘尾的文件数据区；需要 GPT 的是安装器装配的磁盘布局，见[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md) 4.2。
- **不做** ISO 之外的分发打包、签名与信任锚、Joliet/UDF 卷、压缩与引导镜像校验。
- **不写** 任何持久写回：镜像是一次性生成的文件，写入 U 盘由外部工具（如 `dd`）负责。
