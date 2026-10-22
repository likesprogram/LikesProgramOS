# FAT32File — 引导闭包成员（M6 只读实现）

`FAT32File` 是引导闭包里的 FAT32 卷只读文件系统模块：解析 BPB、按 FAT 链读簇、解析 8.3 与
LFN 目录项，回答路径查询、目录遍历与文件读取。`Init` 只做静态校验并把 `FileSystemOps` 九槽表
与模块内静态卷实例交给 `Fs.Vfs` 的 `RegisterFileSystem` 登记；不挂载、不读卷、不导出接口。

| 项 | 值 |
| --- | --- |
| `MODULE_NAME` / `MODULE_LOGICAL_NAME` | `FAT32File`（与 `Packages/Common/Contract/BootClosure.def` 的 `closure` 行逐字一致） |
| `ModuleId` | `SHA-256("LPMOD01" ‖ "FAT32File")[0..16)` = `62598649d4b4a785597b2703ab06424e` |
| 模块版本 | `1.0.0` |
| ELF 产物 | `Out/FAT32File/FAT32File.elf` |
| 容器产物 | `Out/Modules/FAT32File.1.0.0.osm`（0 导出 / 3 导入 / 1 依赖 / 10 重定位 / ImageBytes 0x8000） |
| 导出 | 无（`CONTRACT_EXPORTS` 为空） |
| 导入 | `Fs.Vfs:1:1:8`、`Core.Log:1:1:4`、`Core.Mem:1:1:4`（回填区都在 `.bss`） |
| 依赖 | `Vfs:1:0`（提供者先于消费者，装载器按依赖拓扑排序） |

## 源码结构

| 文件 | 作用 |
| --- | --- |
| `Include/Fat32Volume.hpp` | 只读卷声明与 `FatDevice`（底层只读块设备视图，签名与 `BlockDeviceOps` 读侧一致） |
| `Src/Fat32Volume.cpp` | BPB、FAT 链、目录项（8.3 与 LFN）、路径解析、目录遍历与读取 |
| `Include/Fat32Ops.hpp` / `Src/Fat32Ops.cpp` | `FileSystemOps` 九槽表与静态卷实例 |
| `Src/Module.cpp` | `_Module_Init` / `_Module_Abort`、三个导入回填区、向 `Fs.Vfs` 登记 |

## 名字匹配规则

- **命中按 ASCII 折叠**（`Caps().nameCase = Insensitive`）：每个目录项同时保留 LFN 显示名与
  8.3 短名，两者都能命中。例如 mtools 写入的 `LongNameFile.bin` 短名是 `LONGNA~1.BIN`，
  下列写法都命中同一项：`/LongNameFile.bin`、`/longnamefile.bin`、`/LONGNA~1.BIN`、`/longna~1.bin`。
- **LFN 只接受 ASCII 子集**：LFN 里出现非 ASCII 码元时退回 8.3 名（不做字符集猜测）；
  LFN 与短名的校验和不符时同样退回 8.3 名。
- **目录遍历**交出 LFN 显示名（无合法 LFN 时交出 8.3 名）；`.` 与 `..` 不交出。

## 卷标识裁决（`VolumeIdentity`）

BPB 没有 16 字节卷标识字段。本模块的裁决是：**取 BPB 的 `Volume ID`（偏移 `0x43`，4 字节原字节）
与 `Volume Label`（偏移 `0x47`，11 字节，去掉尾部空格），按 4 + 11 = 15 字节顺序写入输出，
其余字节补零**；未挂载返回 `InvalidArgument`；序号全零且卷标为空时按“格式无标识能力”返回
`NotSupported`。它不参与系统卷身份核对（`Boot.Volume` 只按 `ext4` 的 `s_uuid` 核对）。
若后续规格给 FAT32 定出别的 16 字节口径，只需改 `VolumeIdentity` 一处与对应用例。

## 边界与未支持项

- 只读：不写 FAT、不建目录项、不更新 FSInfo；`Mount` 只接受只读模式，读写挂载返回 `NotSupported`。
- 只做 FAT32：`rootEntries` 非零或 `fatSize16` 非零（FAT12/16 形态）、`FsType` 不是 `FAT32`、
  簇字节数超过 4 KiB 的卷都明确拒绝（`NotSupported`），不猜布局。
- 链损坏明确拒绝：坏簇标记、自环、指向数据区之外的链、比声明长度短的链都按 `Corrupt` 拒绝；
  链遍历都有簇数上界，不做无界循环。
- 所有读取有界：簇缓冲与设备块缓冲都是实例内固定数组（≤ 4 KiB）。

## 构建与核对

```sh
make -C Packages/KernelModules/FAT32File            # 构建 ELF
make -C Packages/KernelModules/FAT32File check      # 无未定义符号、仅 RELATIVE、无 .init_array/TLS、入口在 .text
make -C Packages/KernelModules/FAT32File package    # 契约校验 + PackModule 打包 + --verify
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/FAT32File.1.0.0.osm
```

宿主夹具：`Tools/Test/CheckFat32`（20 例，mkfs.vfat + mtools 生成真实卷 + 内存块设备，
覆盖 BPB / 卷标识 / 根目录与子目录 / LFN 与 8.3 命中 / 跨簇读取 / FAT 链损坏拒绝 / 只读拒绝 / Init 登记路径）。

规则与限制见 `../README.md`，契约声明写法见 `../Common/Contract.hpp`。
