# CheckExt4Module

宿主侧 C++20 检查工具：核对内核 Ext4 模块的只读卷实现与 `FileSystemOps` 九槽表。编译后输出 `Tools/Bin/CheckExt4Module`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckExt4Module
Tools/Bin/CheckExt4Module
Tools/Bin/CheckExt4Module --work /tmp/checkext4module --keep
```

## 检查对象与复用方式

工具按相对路径直接编译**模块将要登记的那份源码**，不做第二份参考实现：

| 源文件 | 作用 |
| --- | --- |
| `Packages/KernelModules/Ext4/Src/Ext4Volume.cpp` | 只读卷解析：superblock、块组描述符、inode、extent 树、线性目录项 |
| `Packages/KernelModules/Ext4/Src/Ext4Ops.cpp` | `FileSystemOps` 九槽表与模块内静态卷实例 |
| `Packages/KernelModules/Ext4/Src/Module.cpp` | 模块入口：上下文校验与向 `Fs.Vfs` 登记（本工具用替身回填区调用 `_Module_Init`） |
| `Packages/KernelModules/Common/Libc.cpp` | 模块侧最小 C 运行库（符号 hidden），宿主侧同样编进来满足模块源码的引用 |

模块源码按目标侧同一条件编译（`-ffreestanding`）：`Common/Libc.hpp` 把最小运行库声明成 hidden，宿主模式下会与内建声明冲突（`-Wattributes` 是错误）。夹具用 `mke2fs` 生成 32 MiB、4096 字节块的真实 Ext4 卷，文件树按系统卷布局布置（`/LikesProgram/System/LikesProgram.os`、`/LikesProgram/System/Modules/Module.osm`、`/big.bin`、`/empty.bin`）；块设备按契约层 `BlockDeviceOps` 的形状用内存实现注入（逻辑块 512 可配 4096），挂载、查询与读取都经 `FileSystemOps` 表完成。

## 检查内容（26 项用例）

**挂载与事实**

- `Format` 返回 `ext4`；`Caps` 只读、可列目录、不跟随符号链接、名字区分大小写
- 未挂载时 `Stat` / `Open` / `VolumeIdentity` 返回 `InvalidArgument`
- 挂载事实：卷内块大小 4096、块数 8192、inode 数非零、根路径解析到 inode 2
- 卷标识：16 字节原字节，与 `mkfs -U` 指定的一致；空输出缓冲拒绝
- 拒绝读写挂载（`NotSupported`）；拒绝空设备表、非 2 的幂逻辑块、缺读原语的设备表
- 卸载后读取拒绝，重挂后事实与读取恢复；4096 字节逻辑块的介质上同样可挂载读取

**读取路径**

- 多级目录事实、32 KiB 内核镜像、12345 字节奇数长度文件、空文件、4 MiB 大文件跨 extent 抽样
- 文件尾与参数边界：尾部按实际长度回报、越过文件尾读到 0、空缓冲与零长度拒绝
- 路径错误映射：不存在 `NotFound`、中间项不是目录 `NotDirectory`、非绝对路径 / `.` / `..` / 尾斜杠 `InvalidArgument`
- 读取面错误：对目录读取 `IsDirectory`、对文件列目录 `NotDirectory`、对象标识 0 或越界 `Corrupt`
- 目录遍历：目录项集合、类型与长度、游标推进；名字缓冲不足 `NameTooLong` 且游标不推进，换大缓冲可继续

**特性拒绝与失败透传**

- 坏 superblock 魔数 `Corrupt`；未知 `INCOMPAT` 位、缺 `FILETYPE` 前提位、`RO_COMPAT BIGALLOC`、未干净卸载的卷、64 位卷高位块数非零都 `NotSupported`
- 挂载期与读取期的读盘失败透传 `Io`

**模块 Init 登记路径**

- 用替身 `Fs.Vfs` 回填区调用 `_Module_Init`：登记使用模块的 `FileSystemOps` 表与静态卷实例、登记标识来自返回值；空上下文 / 上下文过短 / `ModuleId` 不符被拒；导入未回填返回 `kState`；`_Module_Abort` 是空操作

**未由本工具覆盖**：真实介质上的挂载与读文件（T5.8 的 QEMU 端到端）、Vfs 侧的挂载点与句柄语义（`CheckVfs`）、Baleen 侧只读实现（`CheckExt4`）、容器打包与装载（`PackModule --verify`、`CheckContainer`、`CheckLoader`）。

## 选项

| 选项 | 说明 |
| --- | --- |
| `--work <目录>` | 工作目录（夹具与临时文件），默认建在系统临时目录下 |
| `--keep` | 无论成败都保留工作目录 |
| `-h`, `--help` | 显示用法 |

环境变量 `MKFS` 指定卷生成工具（默认 `mke2fs`）；缺工具返回 2。

退出码：`0` 全部通过、`1` 有用例失败、`2` 参数错误或基础设施缺失。
