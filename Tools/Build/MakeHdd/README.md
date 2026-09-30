# MakeHdd — 磁盘镜像组装器

MakeHdd 是宿主侧的构建期工具：把磁盘一级引导（MBR）、实模式服务层（Stub）、核心阶段（BaleenCore）、ESP 的 FAT 镜像与 Ext4 系统卷组装成可写入内部硬盘、U 盘或移动硬盘的磁盘镜像。它对应[用户空间与常驻服务](../../../Docs/Specs/用户空间与常驻服务.md)第五节「布局计算与镜像组装」中的磁盘形态那一段。

光盘与 U 盘混合镜像由 [MakeIso](../MakeIso/README.md) 产出；两个工具共用 `../Common` 里的 BootDescriptor、El Torito 与 MBR 分区代码，保证 U 盘混合镜像与磁盘镜像的引导结构只有一份实现。

## 一、构建

```
make            # 产出 ../../Bin/MakeHdd（中间产物在 Obj/；g++，C++20，共享代码在 ../Common）
make clean      # 删除 Obj/ 与本工具的可执行文件（Bin 与 MakeIso 共用，不删别人的）
```

正常构建只依赖正式 C++ 源码，不依赖未交付的本地回归夹具。本地回归覆盖：MBR 引导代码原样保留且 `0xAA55` 在位、0/1 号分区项的类型与起止 LBA 和实际字节偏移一致、2 号槽留空、BootDescriptor 字段与 Stub 对齐、Stub/Core/`--raw` 内容逐字节一致、两个分区内容与源镜像逐字节一致且不越界、`fdisk` 能读出两类分区，以及可复现性与失败路径（坏 MBR、非 512 字节 MBR、超长 Stub、未对齐或落在 MBR 区的 `--raw`、非法 `--pad-to`）；有权限时把两个分区切出来分别按 FAT 与 Ext4 挂载验证。

## 二、用法

```
MakeHdd --out Baleen.img \
    --mbr  Packages/Baleen/Ipl/Out/Bin/BaleenIPL.bin \
    --stub <BaleenStub.bin> \
    --core <BaleenCore.bin> \
    --esp  <ESP 的 FAT 镜像> \
    --system-volume <Ext4 系统卷镜像> \
    --manifest Out/Baleen.img.json
```

| 选项 | 说明 |
| --- | --- |
| `--out PATH` | 输出磁盘镜像（必需） |
| `--mbr PATH` | 磁盘一级引导（`BaleenIPL.bin`，512 字节且带 `0xAA55`，必需） |
| `--stub PATH` | 实模式服务层：原始扇区放置，并把偏移/长度写进 BootDescriptor |
| `--core PATH` | 核心阶段：原始扇区放置，位置进清单 |
| `--esp PATH` | ESP 的 FAT 镜像（含 `\EFI\BOOT\BOOTX64.EFI`），落到 0 号分区 |
| `--system-volume PATH` | 构建期装配好的 Ext4 系统卷镜像，落到 1 号分区 |
| `--raw 宿主路径[@偏移]` | 原始载荷，可重复；偏移须 4096 对齐且不与已安排区重叠 |
| `--sector-bytes N` | 目标磁盘的逻辑扇区大小：`512`（默认）或 `4096`；只决定 MBR 分区表的 LBA 单位，载荷一律 4096 对齐 |
| `--pad-to N` | 镜像总长向上对齐的粒度，默认 `0x100000`（1 MiB） |
| `--stub-unchecked` | 免除 Stub 头与摘要校验：只给测试夹具与特殊用途，正式构建不要用 |
| `--manifest PATH` | 输出构建清单（JSON） |

系统卷镜像是构建期「布局计算与镜像组装」的产出（内容与光盘形态共用同一份卷内布局），该环节尚未实现，所以要由调用方提供；`--esp` 取的是同一个 FAT 镜像，MakeIso 里叫 `--efi`。

## 三、镜像布局

| 字节偏移 | 内容 |
| --- | --- |
| `0x000` | MBR：`BaleenIPL.bin` 的引导代码原样保留，分区表由本工具写实 |
| `0x22000` | BootDescriptor（32 字节；512 / 2048 / 4096 单位下分别是 LBA 272 / 68 / 34，都是扇区边界） |
| `0x22020` | CoreDescriptor（32 字节，与上一个同扇区；Stub 读同一处定位核心阶段） |
| `0x23000` 起 | 原始载荷：Stub、Core 与 `--raw` 条目，各自 4096 对齐，落在未分区的空隙里 |
| 1 MiB（原始载荷越过 1 MiB 时顺延） | 0 号分区：ESP 的 FAT 镜像 |
| ESP 之后 | 1 号分区：Ext4 系统卷镜像 |
| 末尾 | 总长向上对齐到 `--pad-to`，默认 1 MiB |

ESP 的 1 MiB 起点与 `Mbr.asm` 里分区项 0 的占位 LBA 2048（512 字节单位）一致；原始载荷放在 MBR、描述符与 ESP 之间的空隙，BIOS 路径按裸偏移读它们，不需要分区表。写进 U 盘时，固件看到的就是这张分区表。**4096 对齐让同一份原始镜像在 512 / 2048 / 4096 三种逻辑扇区下都能整扇区读取；分区表是另一件事**：LBA 单位由 `--sector-bytes` 固定，写进 4Kn 磁盘要按 4096 重新组装，ESP 内的 FAT BPB 也不会被自动转换。

## 四、写出的引导结构

**ESP 写死为 FAT32**：UEFI 规范要求 ESP 使用 FAT 文件系统（固件必须能读 FAT12/16/32），固定磁盘上的 ESP 通行且被微软 UEFI 要求采用 FAT32，本项目内核的必备 FAT 实现也只做 FAT32 —— 这条规则符合标准，不需要更换文件系统；工具按 Microsoft FAT 规范的簇数规则判定镜像类型，FAT12、FAT16 与其他格式一律拒绝（错误信息给出实际类型与簇数）。要 FAT32 就有下限：1 簇 1 扇区时至少要 65525 个簇，镜像约 34 MiB，这也是测试夹具取 34 MiB 的原因。

**MBR 分区表**：四个槽位先清空再写有内容的槽位——0 号 = ESP（类型 `0xEF`），1 号 = Ext4 系统卷（类型 `0x83`），2、3 号留空。起点与长度按实际字节偏移换算成 `--sector-bytes` 指定的 LBA 单位，CHS 字段写 `FE FF FF`，强制读取方按 LBA 访问。缺 `--esp` 或 `--system-volume` 时对应槽位保持为空并在标准错误上提示（UEFI 起不来 / 引导链没有系统卷可读）。

**BootDescriptor**（介质绝对偏移 `0x22000`，小端，与 `Packages/Baleen/Ipl/Include/Const.inc`、`Body.inc` 的常量一致）：

| 偏移 | 宽度 | 字段 | 值 |
| --- | --- | --- | --- |
| 0 | 4 | magic | `0x52445342`（`BSDR`） |
| 4 | 2 | version | 当前格式标记 `1`，尚未发布，开发阶段可直接调整 |
| 6 | 2 | header | `32` |
| 8 | 8 | Stub 偏移 | 低 32 位字节偏移，至少 `0x23000` 且 4096 对齐（工具对 512 / 2048 / 4096 三种逻辑扇区的统一策略），偏移加文件长度不得产生 32 位进位 |
| 16 | 8 | Stub 字节数 | 非 0，且不超过 `0x10000 - 0x7E00` |
| 24 | 8 | 保留 | 全 0 |

**CoreDescriptor**（介质绝对偏移 `0x22020`，小端，与 `Packages/Baleen/Common/Include/Contract.inc`、`Stub/Src/LoadCore.cpp` 的常量一致）：字段与校验同 BootDescriptor 的形态——magic `0x52444342`（`BCDR`）、version `1`、header `32`、8 字节 Core 偏移、8 字节 Core 字节数、8 字节保留；偏移至少 `0x23000` 且 4096 对齐，偏移加长度不得产生 32 位进位，长度非 0 且不超过 `0x400000`。写它的前提是提供 `--core`；缺 Core 时只提示，不写描述符。

**载荷门禁**：`--stub` 与 `--core` 的载荷按各自的完整性头判定，Stub 与 Core 同构、只有格式标记与长度上限不同。带头的载荷校验格式标记、版本、头长、标志、摘要算法、`BuildId` 长度、保留字段、`ImageBytes` 与实际文件长度、该类别的长度上限（Stub 为两种入口共用的 `0x8000`，Core 为 `0x400000`）、入口前缀与 `EntryOffset` 一致、`MemoryBytes` 覆盖文件且不越界（Stub 另查低 64KiB 窗口）以及整幅 SHA-256 摘要，通过后把 BuildId 前 4 字节打进构建输出，便于与构建记录对照。`MakePayloads` 生成的占位内容同样要过这套校验；完全没有头的 `PLACEHOLDER-STUB` 占位件放行并在标准错误上提示未校验。两种标识都没有的载荷默认拒绝；`--stub-unchecked` / `--core-unchecked` 显式免除对应载荷的校验，只给测试夹具与特殊用途，被免除校验的载荷不能算作对正式产物的检查。实现与判定规则见 [BaleenImage](../Common/README.md) 与 [PackImage](../PackImage/README.md)。

## 五、构建清单

`--manifest` 的 JSON 记录：`total_bytes`、`sector_bytes` 与按该单位的 `total_sectors`、`mbr`、`descriptor`（含 Stub 偏移与长度）、`core_descriptor`（含 Core 偏移与长度）、`stub` / `core` / `raw[]`、`esp` / `system_volume`（偏移与长度），以及 `partitions[]`（槽位、类型、按该单位的起始 LBA 与扇区数）。后续的布局描述生成与验证脚本按它取字节偏移与长度。

## 六、可复现性

镜像只由输入字节与固定布局规则决定：同一组输入两次构建逐字节相同，不读取宿主文件时间戳、不写入随机值。

## 七、边界与待定项

- **不装配内容**：ESP 的 FAT 与 Ext4 系统卷都取现成镜像；`BaleenLayout.bin` 的字段与落点仍待后续阶段设计，可由 `--raw` 指定位置。IPL 只消费 BootDescriptor；Stub 消费 CoreDescriptor 与 Core 镜像自带的完整性头，其余结构两者都不解析。
- **当前只写 MBR 分区表**：引导区（描述符与载荷）按 MBR 与 GPT 共用的位置排布，GPT 分区表的写入由部署侧实现（见[启动介质与文件系统](../../../Docs/Specs/Common/启动介质与文件系统.md) 4.2）；UEFI 固件按分区表里的 ESP 分区项启动。
- **不做** 物理设备写入（由 `dd` 等外部工具负责）、签名与信任锚、坏块与磨损处理。
- **不擦除**：镜像大小由内容决定，写到更大的介质上时其余空间保持介质原状，需要擦除由写盘步骤自行决定。
