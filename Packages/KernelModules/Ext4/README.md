# Ext4 — 引导闭包成员（M5 只读实现）

`Ext4` 是引导闭包里的 Ext4 只读文件系统模块：从 BaleenCore 的只读实现移植（解析语义不变），
`Init` 只做静态校验并把 `FileSystemOps` 九槽表与模块内静态卷实例交给 `Fs.Vfs` 的
`RegisterFileSystem` 登记；不挂载、不读卷、不导出接口。挂载由 Vfs 的 `Boot.Volume` 在候选
设备上调用（T5.8 接线），读取路径只读、无写、无日志回放。

| 项 | 值 |
| --- | --- |
| `MODULE_NAME` / `MODULE_LOGICAL_NAME` | `Ext4`（与 `Packages/Common/Contract/BootClosure.def` 的 `closure` 行逐字一致） |
| `ModuleId` | `SHA-256("LPMOD01" ‖ "Ext4")[0..16)` = `5baccdf1462dd2b98ac6bb010b8a5051` |
| 模块版本 | `1.0.0`（major 跟随 `.os` 发布线） |
| ELF 产物 | `Out/Ext4/Ext4.elf`（`PackModule` 的输入，不 strip） |
| 容器产物 | `Out/Modules/Ext4.1.0.0.osm`（0 导出 / 2 导入 / 1 依赖 / 10 重定位 / ImageBytes 0x8000） |
| 导出 | 无（`CONTRACT_EXPORTS` 为空）：文件系统实例不是接口实例，登记由 Vfs 持有 |
| 导入 | `Fs.Vfs:1:1:8`（槽 0 `RegisterFileSystem` 必需）、`Core.Log:1:1:4`（Init 失败原因写日志） |
| 依赖 | `Vfs:1:0`（提供者先于消费者，装载器按依赖拓扑排序，不依赖预装顺序） |

## 源码结构

| 文件 | 作用 |
| --- | --- |
| `Include/Ext4Volume.hpp` | 只读卷的类声明与 `Ext4Device`（底层只读块设备视图，签名与 `BlockDeviceOps` 读侧一致） |
| `Src/Ext4Volume.cpp` | superblock / 块组描述符 / inode / extent 树 / 线性目录查找与遍历；缓冲都是实例内固定数组 |
| `Include/Ext4Ops.hpp` | `FileSystemOps` 表与静态卷实例的访问器 |
| `Src/Ext4Ops.cpp` | 九槽表：`Mount` 把 `BlockDeviceOps + deviceContext` 归一成 `Ext4Device`；表是常量初始化（仅 RELATIVE 重定位） |
| `Src/Module.cpp` | `_Module_Init` / `_Module_Abort`、两个导入回填区、向 `Fs.Vfs` 登记 |

## 卷标识与特性集合

- **卷标识事实源是卷内 superblock 的 `s_uuid`（偏移 `0x68`，16 字节）**：`VolumeIdentity` 原字节
  写出，Vfs 的 `Boot.Volume` 与 `BootInfo.VolumeUuid` 逐字节核对；未挂载返回 `InvalidArgument`。
- **特性集合当前阶段按 `Storage::Ext4Features::kBaleenAccepted` 判定**（与 Baleen 只读实现同一集合）：
  `kKernelAccepted` 是阶段 3 的超集（含日志回放需要的 `RECOVER`），只作预留，当前阶段任何路径都
  不得按它接受卷，否则会宣称尚未实现的回放能力（VFS 规格 3.6 规则 3）。
- 挂载阶段先核完解析前提：`FILETYPE` 与 `EXTENTS` 必需位、卷干净位、64 位卷高位块数与描述符
  大小；`RO_COMPAT` 只拒绝 `BIGALLOC`。

## 构建与核对

```sh
make -C Packages/KernelModules/Ext4            # 构建 ELF
make -C Packages/KernelModules/Ext4 check      # ELF 形态：无未定义符号、仅 RELATIVE、无 .init_array/TLS、入口在 .text
make -C Packages/KernelModules/Ext4 package    # 契约校验 + PackModule 打包 + --verify
Tools/Bin/PackModule --verify Packages/KernelModules/Out/Modules/Ext4.1.0.0.osm
```

宿主夹具：`Tools/Test/CheckExt4Module`（26 例，复用本模块源码 + mke2fs 真实卷 + 内存块设备，
覆盖挂载 / 读取 / 目录遍历 / 卷标识 / 特性拒绝 / Init 登记路径）。Baleen 侧只读实现由
`Tools/Test/CheckExt4` 覆盖，两者共用 `Storage/Ext4Features.hpp` 的位表。

规则与限制见 `../README.md`，契约声明写法见 `../Common/Contract.hpp` 与 `../README.md`。
