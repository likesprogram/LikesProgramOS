# ISO9660 — 引导闭包成员（M6 只读实现）

`ISO9660` 是引导闭包里的光盘卷只读文件系统模块：解析主卷描述符、目录记录与
SUSP/Rock Ridge（NM 长名、PX 权限、SL 符号链接识别），回答路径查询、目录遍历与文件读取。
`Init` 只做静态校验并把 `FileSystemOps` 九槽表与模块内静态卷实例交给 `Fs.Vfs` 的
`RegisterFileSystem` 登记；不挂载、不读卷、不导出接口。

| 项 | 值 |
| --- | --- |
| `MODULE_NAME` / `MODULE_LOGICAL_NAME` | `ISO9660`（与 `Packages/Common/Contract/BootClosure.def` 的 `closure` 行逐字一致） |
| `ModuleId` | `SHA-256("LPMOD01" ‖ "ISO9660")[0..16)` = `29af8e3e3bc4cd6c949c8bc458c6a041` |
| 模块版本 | `1.0.0` |
| ELF 产物 | `Out/ISO9660/ISO9660.elf` |
| 容器产物 | `Out/Modules/ISO9660.1.0.0.osm`（0 导出 / 3 导入 / 1 依赖 / 10 重定位 / ImageBytes 0x7000） |
| 导出 | 无（`CONTRACT_EXPORTS` 为空） |
| 导入 | `Fs.Vfs:1:1:8`、`Core.Log:1:1:4`、`Core.Mem:1:1:4`（回填区都在 `.bss`） |
| 依赖 | `Vfs:1:0`（提供者先于消费者，装载器按依赖拓扑排序） |

## 源码结构

| 文件 | 作用 |
| --- | --- |
| `Include/Iso9660Volume.hpp` | 只读卷声明与 `IsoDevice`（底层只读块设备视图，签名与 `BlockDeviceOps` 读侧一致） |
| `Src/Iso9660Volume.cpp` | 主卷描述符、目录记录、SUSP/Rock Ridge、路径解析、目录遍历与读取 |
| `Include/Iso9660Ops.hpp` / `Src/Iso9660Ops.cpp` | `FileSystemOps` 九槽表与静态卷实例 |
| `Src/Module.cpp` | `_Module_Init` / `_Module_Abort`、三个导入回填区、向 `Fs.Vfs` 登记 |

## 名字匹配规则（给 T5.8 / baleen-integration 的验证口径）

- **ISO 标识符是 8.3 大写形式**：光盘内文件名 `SystemVolume.img` 在目录记录里写作
  `SYSTEMVO.IMG;1`（`;1` 是版本号）；**真实名字由 Rock Ridge 的 NM 项保留**，
  本项目的 `MakeImage` ISO 写入器与 `xorriso -R` 都会写 NM。
- **命中规则**：模块对每一层目录项同时保留两个名字——Rock Ridge 显示名（NM，大小写原样）与
  ISO 标识符（文件去掉 `;N` 版本后缀）——**两者都按 ASCII 折叠比较**（`Caps().nameCase = Insensitive`），
  比较前再各自去掉一次版本后缀。因此下列写法都命中同一项：
  `/SystemVolume.img`、`/systemvolume.img`、`/SYSTEMVO.IMG;1`、`/systemvo.img`。
- **给内核侧的建议**：`Stat` / `Open("/SystemVolume.img")` 直接按 MakeImage 的
  `--iso-name`（默认 `SystemVolume.img`）写即可，模块负责折叠；不要自己拼 `;1`。
- **目录遍历**交出 Rock Ridge 显示名（无 NM 时交出 ISO 标识符去版本形式）；`.` 与 `..` 不交出。

## 与系统卷挂载的分工

- **ISO 档的系统卷（Ext4 镜像）仍由 `Boot.Volume` 按 `BootInfo.volumeOffset` 偏移视图挂载**，
  不经过本模块；本模块用于读取光盘文件系统本身，并按上面的规则定位卷内文件做第二来源核对。
- **卷标识**：`VolumeIdentity` 取主卷描述符的 Volume Identifier（32 字节空格填充文本），
  去掉尾部空格后取前 16 字节、其余补零；未挂载返回 `InvalidArgument`，卷标识为空返回
  `NotSupported`。它不参与系统卷身份核对（`Boot.Volume` 只按 `ext4` 的 `s_uuid` 核对）。

## 边界与未支持项

- 只读：不写卷；`Mount` 只接受只读模式，读写挂载返回 `NotSupported`。
- 逻辑块固定 2048：其它逻辑块大小的卷明确拒绝（`NotSupported`），不按 512/1024 猜。
- 未支持并明确拒绝：Joliet、多区段文件（目录记录 flags bit7）、符号链接（SL）与关联/保护属性、
  带扩展属性记录的目录项。双端序字段不一致、`CD001` 不符按 `Corrupt` 拒绝。
- 所有读取有界：记录不跨 2048 逻辑块，目录遍历有块上界，缓冲都是实例内固定数组。

## 构建与核对

```sh
make -C Packages/KernelModules/ISO9660            # 构建 ELF
make -C Packages/KernelModules/ISO9660 check      # 无未定义符号、仅 RELATIVE、无 .init_array/TLS、入口在 .text
make -C Packages/KernelModules/ISO9660 package    # 契约校验 + PackModule 打包 + --verify
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/ISO9660.1.0.0.osm
```

宿主夹具：`Tools/Test/CheckIso9660`（21 例，xorriso 生成带 Rock Ridge 的真实卷 + 内存块设备，
覆盖 PVD / 卷标识 / 根目录与子目录 / 长名与版本形式命中 / 跨块读取 / 坏结构拒绝 / Init 登记路径）。

规则与限制见 `../README.md`，契约声明写法见 `../Common/Contract.hpp`。
