# CheckVfs

宿主侧 C++20 检查工具：Vfs 模块（`Block.Registry` / `Fs.Vfs` / `Boot.Volume`）的宿主侧检查。编译后输出 `Tools/Bin/CheckVfs`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckVfs
Tools/Bin/CheckVfs          # CheckVfs: 52/52 cases passed
```

工具用 `vpath` 把 `Packages/KernelModules/Vfs/Src/` 的目标侧源码按相对路径直接编进本工具（不复制实现），并编译共用的 `Packages/Common/Src/Storage/Path.cpp`。替身注入：

- 伪 `BlockDeviceOps`：内存缓冲 + 可配置块大小 / 容量事实（可按查询次数覆盖，用于模拟探测与切窗口之间的事实变化）/ 写能力 / 刷新原语 / 读写错误注入，回调按共用块设备语义校验对齐、范围与指针
- 伪 `FileSystemOps`：小型固定目录树（`/`、`/hello.txt`、`/System/`、`/System/readme`），挂载探测检查设备首个逻辑块的 `FAKEFS` 魔数，可配置格式名、卷标识、挂载 / `Stat` 错误注入
- 伪 `Dev.Model` 导入槽：`Enumerate` 按句柄枚举；`Binding` 零初始化（bytes == 0）查询时按状态回填 `BindingFacts`（Started 才带 ops 与 context），非查询路径返回 kOk 但不回填；Vfs `Init` 按 Started 事实自动 `Publish`，驱动不直接调用 `Publish`
- 伪 `Core.*` 导入槽：`Core.Boot.Info`、`Core.Log.Write/Line`、`Core.Mem.Copy/Zero/Compare`、`Core.Page.PageBytes`；Init 前填入，`_Module_Init` 用真实入口

## 检查内容

**用例（52 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，失败返回 1；用例数固定，运行时与实跑数核对，防止用例被误删。

- **Init 与入口（5）**：`_Module_Init` 拒绝空上下文、过短上下文与外部模块标识（`kInvalidArgument`），缺导入槽返回 `kNotSupported`；`_Module_Abort` 清空已登记设备（`DeviceExists` 归假）。Init 经 `Dev.Model` 绑定事实登记设备的 7 例注册在下面的 `Boot.Volume` 块内，见该节
- **Block.Registry（12）**：`Publish` 按契约拒绝零句柄、破损 ops、非零 `flags`、非 2 的幂块大小与重复句柄，能力位声明刷新但 `Flush` 为空也拒绝（无能力位且回调为空可登记）；`Acquire` / `Release` 维护引用计数，下溢与未知句柄分别以 `InvalidArgument` / `NotFound` 拒绝；设备表耗尽 `NoSpace`；`SubDevice` 按父设备逻辑块切出对齐窗口，容量为窗口长度、块大小与可写性随父，读转发到父偏移；未对齐 / 零长 / 越界 / 回绕 / 非零 `flags` 窗口与未知父句柄被拒，子视图可嵌套并逐层映射
- **Fs.Vfs 登记与挂载（6）**：`RegisterFileSystem` 拒绝空 ops 与缺失 `VolumeIdentity`，成功返回非零 id 且可按格式名查到（未注册格式查不到）；`Mount` 对未知文件系统 / 未知设备返回 `NotFound`，按 v1 拒绝非 `/` 挂载点（`NotSupported`）、相对路径（`InvalidArgument`）与读写模式（`NotSupported`）；挂载持有设备引用、同设备重复挂载以 `InvalidArgument` 拒绝，`Unmount` 释放引用并调用实现 `Unmount`；实现返回错误时原样透传且不残留挂载与引用
- **路径解析与文件操作（10）**：`Stat` 把 `/`、`/hello.txt` 的卷内路径原样交给文件系统，缺失 `NotFound`、中间项非目录 `NotDirectory`；相对 / 空路径与空输出指针 `InvalidArgument`、超长路径 `NameTooLong`；`Open` / `Close` 覆盖引用生命周期，重复关闭与零引用 `InvalidArgument`、引用表耗尽 `NoSpace`；`Read` 读满 / 截断 / 文件尾零长，零长度与零引用 `InvalidArgument`，目录读 `IsDirectory`；`ReadDirectory` 逐项返回名字与目录标记、结束 `NotFound`，名字缓冲放不下 `NameTooLong` 且不推进游标；有打开引用时 `Unmount` 拒绝、关闭后可卸载、卸载后 `Stat` 返回 `InvalidArgument`；`Stat` 的 `Corrupt` / `Io` 原样透传
- **挂载点最长匹配（3）**：内部 `MountAt` 同时挂 `/` 与 `/mnt` 后，`/hello.txt` 交根文件系统、`/mnt/hello.txt` 交深层文件系统（卷内路径为 `/hello.txt`）；`/mntx` 按分量边界不命中 `/mnt`，仍走根挂载；挂载点自身映射为卷内根 `/`；同一设备不允许二次挂载（`InvalidArgument`）
- **Boot.Volume（16）**：前 9 例核对 `MountSystemVolume`——输入为空 / 无 `BootInfo` / 空事实 `InvalidArgument`；卷标识全零、卷区段缺失、区段越过 `mediumBytes`、无设备、未注册 ext4（缺 ext4 时在探测候选之前返回）均 `NotFound`；候选探测逐个核对块大小与容量事实：未按设备逻辑块对齐、起点已到容量边界的候选被跳过（不读盘），只有通过探测的候选才进入切窗口与挂载；身份不符 `NotFound` 且不残留挂载与引用；唯一候选成功时返回 52 字节事实（`volumeUuid` 与 `buildId` 来源正确）、打印 `[ OK ] system volume mounted: uuid=<hex>` 标记、挂载表为 1 且根设备不被引用（挂载的是偏移子视图）、可卸载；两个候选都匹配按歧义 `NotFound` 且不残留挂载；未传 `BootInfo` 时改用导入的 `Core.Boot.Info()`。后 7 例是 Init 经 `Dev.Model` 绑定事实登记设备（源码注册在 `Boot.Volume` 块内，语义属 Init 入口）：`Started(2)` 且带 ops 的绑定由 Init 自动 `Publish` 且 `Acquire` 事实正确；未绑定（`kNotFound`）跳过；块大小非 2 的幂等非法绑定事实、`Bound(1)` 未启动、`Binding` 返回 `kOk` 但未回填事实都按 `kCorrupt` 拒绝且不登记；缺 `Dev.Model.Binding` 导入返回 `kNotSupported`；绑定查询按零初始化（`bytes == 0`）回填长度、状态与 ops / context，非查询路径返回 `kOk` 但不回填

**未覆盖**：真实块设备驱动与真实 Ext4 卷（分别由 `CheckStorage` / `CheckExt4` 与 T5.7 覆盖）；布局描述交叉核对（VFS 规格 4.5 第 2 步，当前未实现，见模块 README）；`Boot.Volume` 的 `Dev.Model.BootCandidates` 候选收窄（T5.7/T5.8 接线）。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与原因），或用例数与固定值不符 |

## 破坏性自测

把任一断言改坏（例如把某个期望错误码改成 `Ok`）后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。这一自测用于证明断言不是恒真。
