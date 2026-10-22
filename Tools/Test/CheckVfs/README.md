# CheckVfs

宿主侧 C++20 检查工具：Vfs 模块（`Block.Registry` / `Fs.Vfs` / `Boot.Volume`）的宿主侧检查。编译后输出 `Tools/Bin/CheckVfs`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckVfs
Tools/Bin/CheckVfs
```

工具用 `vpath` 把 `Packages/KernelModules/Vfs/Src/` 的目标侧源码按相对路径直接编进本工具（不复制实现），并编译共用的 `Packages/Common/Src/Storage/Path.cpp`。替身注入：

- 伪 `BlockDeviceOps`：内存缓冲 + 可配置块大小 / 写能力 / 刷新原语 / 读错误，回调按共用块设备语义校验对齐、范围与指针
- 伪 `FileSystemOps`：小型固定目录树（`/`、`/hello.txt`、`/System/`、`/System/readme`），挂载探测检查设备首个逻辑块的 `FAKEFS` 魔数，可配置格式名、卷标识、挂载 / `Stat` 错误注入
- 伪 `Dev.Model` 导入槽：`Enumerate` 按句柄枚举；`Binding` 零初始化（bytes == 0）查询时按状态回填 `BindingFacts`（Started 才带 ops 与 context），非查询路径返回 kOk 但不回填；Vfs `Init` 按 Started 事实自动 `Publish`，驱动不直接调用 `Publish`
- 伪 `Core.*` 导入槽：`Core.Boot.Info`、`Core.Log.Write/Line`、`Core.Mem.Copy/Zero/Compare`、`Core.Page.PageBytes`；Init 前填入，`_Module_Init` 用真实入口

## 检查内容

**用例（51 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，失败返回 1；用例数固定，运行时与实跑数核对，防止用例被误删。

- **Init 与入口（12）**：空上下文、过短上下文、外部模块标识、缺导入槽（含缺 Dev.Model）都按错误拒绝；Dev.Model 绑定查询必须零初始化（bytes == 0）才回填事实、非查询路径 kOk 不代表有事实；Init 按 Started(2)+ops 才登记、Bound(1) 未启动与非法/无事实绑定返回 kCorrupt、未绑定（kNotFound）跳过；`Abort` 清空注册表
- **Block.Registry（12）**：设备登记路径由伪 Dev.Model 绑定事实驱动（`Init` 内部 `Publish`）；`Publish` 槽本身仍按契约拒绝零句柄 / 破损 ops / 非零 `flags` / 非 2 的幂块大小 / 重复句柄 / 能力位有刷新但回调为空，无刷新能力且回调为空可登记；`Acquire`/`Release` 引用计数与下溢拒绝；未知句柄 `NotFound`；设备表耗尽 `NoSpace`；`SubDevice` 对齐窗口映射、容量 / 块大小 / 可写随父、读转发到父偏移；未对齐 / 零长 / 越界 / 回绕 / 非零 `flags` / 未知父句柄拒绝；子视图可嵌套并逐层映射
- **Fs.Vfs 登记与挂载（7）**：`RegisterFileSystem` 拒绝空 ops 与缺失必需槽、成功返回非零 id、按格式名可查；`Mount` 拒绝未知文件系统 / 未知设备；公共入口按 v1 拒绝非 `/` 挂载点（`NotSupported`）、非法语法（`InvalidArgument`）、读写模式（`NotSupported`）；挂载持有设备引用、重复挂载拒绝、`Unmount` 释放引用并调用实现 `Unmount`；挂载失败透传并清理引用与挂载表
- **路径与文件操作（13）**：根与文件 `Stat` 的卷内路径透传、缺失 `NotFound`、中间项非目录 `NotDirectory`；相对 / 空路径与空输出指针 `InvalidArgument`、超长路径 `NameTooLong`；`Open`/`Close` 引用生命周期与重复 / 零引用拒绝、引用表耗尽 `NoSpace`；`Read` 读满 / 截断 / 文件尾零长 / 零长度与零引用拒绝、目录读 `IsDirectory`；`ReadDirectory` 两项后结束返回 `NotFound`、名字缓冲放不下 `NameTooLong` 且不推进游标；有打开引用时 `Unmount` 拒绝、关闭后卸载成功、卸载后操作 `InvalidArgument`；`Stat` 的 `Corrupt`/`Io` 错误原样透传
- **挂载点最长匹配（3）**：内部 `MountAt` 同时挂 `/` 与 `/mnt` 后，`/mnt/hello.txt` 交给深层文件系统的卷内路径是 `/hello.txt`，`/hello.txt` 仍归根挂载；`/mntx` 按分量边界不命中 `/mnt`（走根挂载）；挂载点本身映射为卷内根 `/`；同一设备不允许二次挂载
- **Boot.Volume（4）**：输入为空 / 无 `BootInfo` / 空事实拒绝；卷标识全零、卷区段缺失、越过 `mediumBytes` 都 `NotFound`；无设备 / 无 ext4 注册 `NotFound`；身份不符 `NotFound` 且不残留挂载与引用；唯一候选成功时返回 52 字节事实（`volumeUuid` 与 `buildId` 来源正确）、输出 `[ OK ] system volume mounted: uuid=<hex>` 标记、挂载表为 1 且卸载成功；两个候选身份都匹配时按歧义 `NotFound` 且不残留挂载；`BootInfo` 为空时改用导入的 `Core.Boot.Info()`

**未覆盖**：真实块设备驱动与真实 Ext4 卷（分别由 `CheckStorage` / `CheckExt4` 与 T5.7 覆盖）；布局描述交叉核对（VFS 规格 4.5 第 2 步，当前未实现，见模块 README）；`Boot.Volume` 的 `Dev.Model.BootCandidates` 候选收窄（T5.7/T5.8 接线）。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与原因），或用例数与固定值不符 |

## 破坏性自测

把任一断言改坏（例如把某个期望错误码改成 `Ok`）后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。这一自测用于证明断言不是恒真。
