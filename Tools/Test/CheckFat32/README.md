# CheckFat32

宿主侧 C++20 检查工具：核对内核 FAT32File 模块的只读卷实现与 `FileSystemOps` 九槽表。编译后输出 `Tools/Bin/CheckFat32`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckFat32
Tools/Bin/CheckFat32
Tools/Bin/CheckFat32 --work /tmp/checkfat32 --keep
```

## 检查对象与复用方式

工具按相对路径直接编译**模块将要登记的那份源码**：`Packages/KernelModules/FAT32File/Src/` 下的
`Fat32Volume.cpp`、`Fat32Ops.cpp` 与 `Module.cpp`，以及模块公共运行库 `Common/Libc.cpp`；
不做第二份参考实现。模块源码按目标侧同一条件编译（`-ffreestanding`）。

夹具用 `mkfs.vfat -F 32 -n TESTVOL -i 12345678` 生成 256 MiB 的真实 FAT32 卷，再用 mtools
（`mmd` / `mcopy`）放入文件树：`/Hello.txt`（100 字节）、`/LongNameFile.bin`（12345 字节，LFN）、
`/BigFile.bin`（300 KiB，跨多个簇）、`/Empty.bin`、`/DIR/Module.osm`；块设备按契约层
`BlockDeviceOps` 的形状用内存实现注入（逻辑块 512 或 4096），挂载、查询与读取都经
`FileSystemOps` 表完成。

## 检查内容（20 项用例）

- 格式名 `fat32`、能力位（只读、可列目录、不跟随符号链接、名字 ASCII 折叠）
- 未挂载状态拒绝；挂载事实（根目录哨兵对象）；卷标识（BPB 卷序号 4 字节 + 卷标 11 字节）
- 拒绝读写挂载、空设备表、非 2 的幂逻辑块、缺读原语的设备表
- 根目录项集合（LFN 长名）与子目录遍历/读取
- 名字命中规则：LFN 真实名、小写折叠、8.3 短名（`LONGNA~1.BIN`）与折叠形式指向同一项
- 小文件、奇数长度文件、空文件、跨簇大文件（首/中/尾抽样）；文件尾与参数边界
- 目录名字缓冲不足返回 `NameTooLong` 且不推进游标；读取面错误与路径错误映射
- 拒绝 FAT16 形态（根目录项数、16 位 FAT 大小、`FsType` 不是 FAT32）与非法扇区大小
- FAT 链损坏（坏簇标记、自环）按 `Corrupt` 拒绝；读盘失败透传 `Io`；4 KiB 设备逻辑块
- 卸载与重挂；模块 Init 登记路径（替身 `Fs.Vfs` 回填区，核对登记参数与错误分类）

**未由本工具覆盖**：真实介质上的 QEMU 端到端、Vfs 侧挂载点与句柄语义（`CheckVfs`）、
容器打包与装载（`PackModule --verify`、`CheckContainer`、`CheckLoader`）。

## 选项与退出码

`--work <目录>` 指定工作目录（默认系统临时目录）、`--keep` 保留工作目录、`-h/--help` 显示用法；
环境变量 `MKFS_VFAT` / `MMD` / `MCOPY` 指定夹具工具。退出码 `0` 全部通过、`1` 有用例失败、
`2` 参数错误或基础设施缺失（缺 mkfs.vfat / mtools）。
