# CheckExt4

宿主侧 C++20 检查工具：核对 BaleenCore 的 Ext4 只读实现（`Packages/Baleen/Core/Src/Ext4.cpp`）、块设备适配器（`BiosBlockDevice.cpp`）与刷盘路径（`DiskFlush.cpp` 的 AHCI 命令编码）。编译后输出 `Tools/Bin/CheckExt4`。**手工执行，不构成默认构建依赖**；用例内嵌在源码里，Ext4 夹具由 `mke2fs` 现场生成。

```sh
make -C Tools/Test/CheckExt4
Tools/Bin/CheckExt4
Tools/Bin/CheckExt4 --work /tmp/checkext4 --keep
```

工具按宿主选项编译**目标侧的同一份源码**（不做第二份参考解析），把它接到模拟的介质与低内存上运行：介质影像模拟分区内的卷，模拟读盘服务复刻 Stub 的契约——只接受落低 1 MiB、16 字节对齐的目的地，正是块设备适配器需要中转的那条约束。

## 检查内容

**用例（28 项）**：用 `mke2fs` 生成 32 MiB、4096 字节块的 Ext4 卷，根目录内容由工具写出的文件树填充；另有一组伪 AHCI 控制器用例：

- 挂载与卷事实：块大小、inode 与块计数、卷标识（与 `mkfs` 指定的 UUID 逐字节一致）、能力位、根目录
- 按路径读取并与原文件逐字节比对：32 KiB 常规文件、`/System/Module.osm` 的 12345 字节奇数长度、空文件、4 MiB 大文件（跨多个窗口读盘）
- 路径错误映射：不存在的路径 `NotFound`、中间项不是目录 `NotDirectory`、非绝对路径 `InvalidArgument`、`Stat` 传播解析错误
- 目录遍历：内核集目录与根目录的目录项集合、类型与长度（`.` 与 `..` 不作为目录项交出）、游标推进与遍历结束、名字缓冲过小返回 `NameTooLong` 且不推进游标、对常规文件返回 `NotDirectory`、空缓冲与越界标识的错误映射
- 故障注入：坏 superblock 魔数（`Corrupt`）、未知 `INCOMPAT` 位与明确拒绝的特性（`NotSupported`，含 `INLINE_DATA` 与 `BIGALLOC`）、缺失解析前提位（`FILETYPE` 与 `EXTENTS`，`NotSupported`）、64 位卷的高位块数非零（`NotSupported`）与描述符大小非 64（`Corrupt`）、非干净卷状态位（`NotSupported`）、块号越界（`Corrupt`）、错误卷偏移（`Corrupt`）、读盘失败（`Io`）
- 块设备适配器：窗口中转（256 KiB 读拆成多次窗口读盘）、未对齐偏移与长度、空指针、越过容量、未绑定行为、非法绑定参数（非 2 的幂扇区大小、未对齐窗口）
- 偏移视图：未对齐起点、零长度、越过容量、合法区间
- **伪 AHCI 控制器**：把 `DiskFlush` 的 AHCI 命令接到按 AHCI 1.3.1 字段规则执行的伪控制器上——命令头 DW0 的 CFL 必须是 5、PRDTL 与命令相符（刷盘 0、读 1）、DW1（PRDBC）下发前为 0，FIS 的 LBA 与扇区数按逻辑扇区换算且与 PRDT 字节数一致。QEMU 对命令长度字段宽容，这一层不宽容：字段位置写反或 4Kn 按 512 换算都会变红。覆盖 512 字节扇区、4Kn 逻辑扇区（4096 字节读只有 1 个扇区）与内容不符判未覆盖三类

**未由夹具覆盖**：extent 树的索引层（`depth > 0`）需要物理不连续的文件才能触发，当前夹具按连续分配生成叶子层；随真实内核集镜像落地后补测。引导状态区域的读写、BaleenLayout 的消费路径也要等内核集阶段。伪 AHCI 控制器校验的是字段编码，不替代真实控制器上的执行验证（见[缺陷与修复记录](../../../Docs/Test/缺陷与修复记录.md)第三节）。

## 选项

| 选项 | 说明 |
| --- | --- |
| `--work <目录>` | 工作目录（夹具与临时文件），默认建在系统临时目录下（`checkext4.<pid>`） |
| `--keep` | 无论成败都保留工作目录 |
| `--strict` | 缺少 `mke2fs` 时返回失败（默认返回 2） |
| `-h`, `--help` | 显示用法 |

环境变量 `MKFS` 指定卷生成工具（默认 `mke2fs`）。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部检查通过 |
| 1 | 有用例失败；工作目录保留，便于查错 |
| 2 | 缺少 `mke2fs`，夹具无法生成；加 `--strict` 时按失败处理 |

失败时工具打印相关日志的末尾若干行，完整日志留在工作目录里。
