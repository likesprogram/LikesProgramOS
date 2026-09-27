# Ipl — 一级引导

Ipl 是 Baleen 引导链的第一段代码：固件装入它，它读 `BootDescriptor`，把实模式服务层 Stub 读进内存并交权。它不解析文件系统、不读 Superblock、不做引导选择——那些属于 Stub 与 BootCore；引导模式、介质与交接契约以 [Baleen 引导器](../../../Docs/Specs/Baleen引导器.md) 为准，本文只描述本目录的源码、产物与构建。

上级说明见 [Baleen](../README.md)。

## 一、产物

| 产物 | 大小 | 介质与载入方 |
| --- | --- | --- |
| `Out/Bin/BaleenIPL.bin` | 512 B | 硬盘 / U 盘（USB-HDD）：BIOS 装入 `0x7C00`，前 446 字节为引导代码，其后是分区表与 `0xAA55` |
| `Out/Bin/BaleenIPLCd.bin` | 2048 B | El Torito 无仿真：引导目录把整个文件作为引导镜像装入 `0x7C00`，LoadSize 记为 4（512 字节单位） |

两种形态的字节不同（本地扇区大小、驱动器探测、描述符所在扇区都不一样），必须分开产出。规格第八节把一级引导记作 `BaleenIPL.bin`，光盘形态在此名字上加 `Cd` 区分。

- 硬盘形态的分区表只写死分区项 0 的 ESP 占位与起始 LBA；分区项 1..3 和 ESP 大小由写盘工具（Mbr.asm 注释中的 MKDISK）在组装镜像时填写。
- 光盘形态的偏移 8..63 留给 XORRISO `-BOOT-INFO-TABLE`，IPL 自身不读该区。

## 二、源码

| 文件 | 职责 |
| --- | --- |
| `Src/Mbr.asm` | 硬盘 / USB-HDD 入口：`SECT_SHIFT=9`、`IPL_MEDIA=MEDIA_HDD`；446 字节上限检查、分区表、`0xAA55` |
| `Src/Cdrom.asm` | 光盘入口：`SECT_SHIFT=11`、`IPL_MEDIA=MEDIA_CDROM`、`IPL_CD`；510 字节上限检查，补零到 2048 |
| `Include/Body.inc` | 两形态共用主体：实模式初始化、读并校验 `BootDescriptor`、按扇区读 Stub、跳转交权、单字符诊断 |
| `Include/Read.inc` | `_Read_Sectors` / `Read_Range`：INT 13h EDD（AH=42）批量读，失败退回逐扇区，无 EDD 时用 AH=02 |
| `Include/Const.inc` | 装入地址、栈顶、BootDescriptor / Superblock 偏移与魔数、介质编号；数值与 `BootInfo.hpp`、`Superblock.hpp` 一致 |

入口源各自定义 `SECT_SHIFT` 与 `IPL_MEDIA`，其余开关在源内有默认值，可由构建期覆盖：

- `ENABLE_E9`：默认 1，0xE9 调试口开；`make ENABLE_E9=0` 关闭（Body.inc 注释中的 BR-21）。

## 三、构建

需要 NASM 3.x 与 GNU make（本目录在 NASM 3.01 下验证），在目录内或 `make -C Packages/Baleen/Ipl` 执行：

```
make                # 产出 Out/Bin/ 下两个镜像（默认目标）
make check          # 构建并校验大小与 0xAA55 签名
make list           # 生成 NASM 汇编清单到 Out/Obj/
make clean          # 删除 Out/
make ENABLE_E9=0    # 关闭 0xE9 调试输出
```

- **必须用 `-Ox`**（多趟优化）让短跳转收敛到最短：实测 `-O0` / `-O1` 会把 Mbr 或 Cdrom 撑过 446 / 510 字节上限，被汇编期 `%ERROR` 拦下。Makefile 已固定该参数。
- `OUT_DIR=` 可重定向输出根目录（默认 `Out`），供顶层构建汇总产物。
- 源码里还有尺寸硬约束：`Src/Mbr.asm` 与 `Src/Cdrom.asm` 用 `%IF ($ - $$) > 446 / > 510` 在汇编期报错，不靠人工检查。

## 四、运行期契约

### 4.1 内存布局

| 物理地址 | 内容 |
| --- | --- |
| `0x0300` | `BootDescriptor`：写盘 / 镜像组装工具写入，IPL 只读 |
| `0x7C00` | 固件装入的 IPL；两种形态都留在原地，不搬移 |
| `0x7E00` | Stub 装入点（`LOAD_TOP`）；Stub 镜像含 BSS 不得越过 `0x10000` |
| `0x7C00` 以下 | 16 位栈（`SS=0`、`SP=0x7C00`，向下增长） |

### 4.2 BootDescriptor

小端字段；硬盘形态（512 B 扇区）位于 LBA 1 偏移 `0x100`，光盘形态（2048 B 扇区）位于引导镜像偏移 `0x300`：

| 偏移 | 宽度 | 字段 | 要求 |
| --- | --- | --- | --- |
| 0 | 4 | magic | `0x52445342`（`BSDR`） |
| 4 | 2 | version | `1` |
| 6 | 2 | header | `32` |
| 8 | 8 | Stub 偏移 | 字节偏移，高 32 位为 0，按本地扇区对齐 |
| 16 | 8 | Stub 字节数 | 高 32 位为 0，非 0，按扇区上取整后 ≤ `0x10000 - 0x7E00` |
| 24 | 8 | 保留 | 全 0 |

### 4.3 交给 Stub 的机器状态

跳转 `JMP 0x0000:0x7E00` 时：

| 寄存器 | 值 |
| --- | --- |
| `CS:IP` | `0x0000:0x7E00` |
| `DS` | `0x07C0`（IPL 的段） |
| `ES` | `0` |
| `SS:SP` | `0x0000:0x7C00` |
| `DL` | 固件驱动器号（光盘形态可能探测为 `0xE0` / `0xE1`） |
| `DH` | `Boot::Media`：`2` = Hdd（硬盘 / USB-HDD）、`4` = Cdrom |

Stub 不得占用 IPL 所在的 `[0x7C00, 0x7E00)`，也不得越过 `0x10000`。

## 五、失败诊断

任一步失败即输出一个字符并停机，字符同时送 VGA 电传与 0xE9 口：

| 码 | 含义 |
| --- | --- |
| `D` | `BootDescriptor` 无效：魔数、版本、头长、保留位、偏移或大小不合规 |
| `R` | 描述符扇区读盘失败 |
| `S` | Stub 字节数为 0、高 32 位非 0，或按扇区上取整后越过 `0x10000` |
| `L` | Stub 读盘失败 |

## 六、本目录不做的事

- 不生成 `BootDescriptor`、`BaleenLayout.bin` 和磁盘 / ISO 镜像——写盘与镜像组装工具尚未加入，混合镜像需在同一份字节上同时满足光盘与磁盘引导记录，见 [启动介质与文件系统](../../../Docs/Specs/启动介质与文件系统.md) 第二节。
- 不解析 Ext4、不读 Superblock、不持久化引导状态——引导选择在 Stub / BootCore / UEFI 侧，见 [Baleen 引导器](../../../Docs/Specs/Baleen引导器.md) 第五、七节。

## 七、当前状态

一级引导的源码与构建已就位（NASM 3.01 下 `make check` 通过）；Stub、BootCore 与 UEFI 侧仍是空骨架。`Out/` 是构建输出目录，可随时 `make clean` 后重建。
