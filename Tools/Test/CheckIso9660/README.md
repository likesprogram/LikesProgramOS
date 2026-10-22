# CheckIso9660

宿主侧 C++20 检查工具：核对内核 ISO9660 模块的只读卷实现与 `FileSystemOps` 九槽表。编译后输出 `Tools/Bin/CheckIso9660`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckIso9660
Tools/Bin/CheckIso9660
Tools/Bin/CheckIso9660 --work /tmp/checkiso9660 --keep
```

## 检查对象与复用方式

工具按相对路径直接编译**模块将要登记的那份源码**：`Packages/KernelModules/ISO9660/Src/` 下的
`Iso9660Volume.cpp`、`Iso9660Ops.cpp` 与 `Module.cpp`，以及模块公共运行库 `Common/Libc.cpp`；
不做第二份参考实现。模块源码按目标侧同一条件编译（`-ffreestanding`），因为
`Common/Libc.hpp` 的最小运行库声明是 hidden，宿主模式下会与内建声明冲突。

夹具用 `xorriso -as mkisofs -R` 生成带 Rock Ridge 的真实 ISO9660 卷（卷标识 `TESTVOL`），
文件树为 `/SystemVolume.img`（16 MiB）、`/Hello.txt`、`/LongNameFile.bin`（12345 字节）、
`/sub/Module.osm`；块设备按契约层 `BlockDeviceOps` 的形状用内存实现注入（逻辑块 2048 或 512），
挂载、查询与读取都经 `FileSystemOps` 表完成。

## 检查内容（21 项用例）

- 格式名 `iso9660`、能力位（只读、可列目录、不跟随符号链接、名字 ASCII 折叠）
- 未挂载状态拒绝；挂载事实（根目录对象、目录事实）；卷标识（Volume Identifier 取前 16 字节补零）
- 拒绝读写挂载、空设备表、非 2 的幂逻辑块、缺读原语的设备表
- 根目录项集合（Rock Ridge 长名）；子目录遍历与读取
- 名字命中规则：真实名、小写折叠、ISO 标识符带 `;1`、去版本折叠四种写法指向同一项
- 16 MiB 系统卷镜像的整文件读取与中部/尾部抽样；文件尾与参数边界
- 目录名字缓冲不足返回 `NameTooLong` 且不推进游标，换大缓冲可继续
- 读取面错误（对目录读 `IsDirectory`、对文件列目录 `NotDirectory`）与路径错误映射
- 坏主卷描述符 `Corrupt`、非 2048 逻辑块 `NotSupported`、双端序字段不一致 `Corrupt`
- 读盘失败透传 `Io`；512 字节设备逻辑块；卸载与重挂
- 模块 Init 登记路径：替身 `Fs.Vfs` 回填区调用 `_Module_Init`，核对登记参数与
  `kInvalidArgument` / `kCorrupt` / `kState` 分类

**未由本工具覆盖**：真实光盘上的 QEMU 端到端（T5.8）、Vfs 侧挂载点与句柄语义（`CheckVfs`）、
容器打包与装载（`PackModule --verify`、`CheckContainer`、`CheckLoader`）。

## 选项与退出码

`--work <目录>` 指定工作目录（默认系统临时目录）、`--keep` 保留工作目录、`-h/--help` 显示用法；
环境变量 `XORRISO` 指定 ISO 生成工具。退出码 `0` 全部通过、`1` 有用例失败、`2` 参数错误或
基础设施缺失（缺 xorriso）。
