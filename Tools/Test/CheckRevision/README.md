# CheckRevision

宿主侧 C++20 检查工具：核对引导状态记录的 `ModuleRevision` 字段与**模块修订选择 / 回退**语义。编译后输出 `Tools/Bin/CheckRevision`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckRevision
Tools/Bin/CheckRevision
```

## 检查对象与复用方式

工具按宿主选项编译**目标侧的同一份源码**，不做第二份参考实现：

| 源文件 | 作用 |
| --- | --- |
| `Packages/Common/Src/BootState.cpp` | 记录字节表、槽位选择、写入顺序、`PublishCandidate` / `ConfirmAttempt` / `Repair` / `Clear` 等写方实现 |
| `Packages/Baleen/Core/Src/SelectSet.cpp` | 四态决策、修订来源解析（当前清单 / `Manifest.<rev>.mft` 快照）、两阶段回退 |
| `Packages/Common/Src/{Revocation,Storage/Error,Storage/Path,Storage/FileSystem}.cpp` | 上面两份源码依赖的共用实现 |

源码由 `Makefile` 用相对路径（`../../../Packages/...`）直接引用，**不复制、不内嵌副本**。

**替身**（本工具提供、不属于检查对象）：

- `VerifyKernelSet`：按夹具登记的“清单来源”（当前修订或旧修订快照）返回验证结论，并填出清单事实（修订号、安全版本、清单/列表副本、内核路径）。真实的清单结构、签名、防回滚与逐产物摘要判定由 `CheckRevocation` 与实机路径覆盖。
- `ReadBootListRevision`：返回夹具登记的“当前列表修订号”。生产实现读集目录里 `Modules.list` 的固定头，宿主上没有存储层，因此用替身控制路由输入。
- `WriteDiagnostic`：把诊断行收进夹具供断言，不装控制台。
- `Crypto::FindTrustAnchor` / `Crypto::Ed25519Verify`：选版把这两个函数交给吊销记录加载用；本工具介质上没有吊销记录，只为链接提供同一组符号。

因此本工具覆盖的是**记录字段、写方义务、修订路由与回退决策**；「签名是否成立」不在本工具的判定范围内（由 `CheckRevocation` 覆盖，`CheckSelect` 覆盖选版状态机本身）。

## 检查内容（16 项用例）

**记录字段与写方**

- `ModuleRevision` 落在槽位 `0x0D8..0x0E0`（写方向核对槽位字节、读方向按规格字节表构造并解出）；记录有效长度仍为 `0xF0`、八槽环形不变
- 保留区 `0x0E0..0x0EC` 的 12 字节逐字节必须全零，任一字节非零按损坏拒绝
- `Checksum` 覆盖 `[0x000, 0x0EC)`：改 `0x0D8` 不重算即拒，重算后接受并解出新值
- 读写往返：`0`（无修订）与 `0xFFFFFFFFFFFFFFFF` 最大修订号都原样读回
- 发布候选继承回退目标：`PublishCandidate` 不改变已有 `Confirmed` 记录的 `ModuleRevision`；空区域发布按 0 起步
- 确认提交写已确认修订：`ConfirmAttempt` 身份相符才写、修订号为零拒绝、写进 `Confirmed` 记录
- 维护动作保留：`Repair` 重建槽位与 `AcceptSeenSecurityVersion` 下调已见版本都不动 `ModuleRevision`（撤销事实一并保留）
- `Clear` 之后按当前列表修订重建：空区域首次启动写出的 `Confirmed` 记录 `ModuleRevision` = 当前 `Modules.list` 的 `revision`
- 旧记录（`ModuleRevision == 0`）兼容：可读、选择按当前列表的 revision 解释、无事实变化时不升级字段、`Repair` 保留 0

**修订选择算法**

- 目标修订等于当前列表修订：取当前 `Manifest.mft` / `Modules.list`
- 目标修订不等于当前列表修订：取 `Manifest.<rev>.mft` / `Modules.<rev>.list` 快照，且只验证这一份来源
- 目标修订不可选：快照验签失败或快照不存在都终止，不落到当前修订或别的修订、不写状态

**两阶段回退**

- 防环：回退目标与失败候选是同一集、同一修订时不重试同一组合，按“良好集也不可用”终止
- 候选预装失败：写 `Failed`、输出 `WARN E-CORE-98`，按记录里的回退目标修订改选上一修订
- 已知良好集装载失败：原样终止，不写状态转换
- 只读介质（写不成 `Failed`）：`WARN E-CORE-70` 后按降级路径回退已知良好集，介质上仍停 `Trying`

**未由本工具覆盖**：真实签名 / 结构 / 摘要判定（`CheckRevocation`）、四态状态机与记录损坏矩阵（`CheckSelect`）、`FixBootState` 命令行行为（工具级手工验证）、真实介质上的修订切换启动演练（启动矩阵，随 M6 G6d）。

## 登记

工具目录与可执行文件已就绪；顶层 `Makefile` 的 `TEST_TOOL_DIRS` 与 `Tools/Test/CheckAll.sh` 的 `host_tools` 静态清单**由协调者统一登记**（本任务不改那两处）。

规格依据：[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)第二、四节与 2.2（修订字段与修订选择算法）；[内核集更新与回滚](../../../Docs/Specs/内核集更新与回滚.md)第三节（四态）、第四节（失败范围与回退）。

退出码：`0` 全部通过、`1` 有用例失败、`2` 参数错误。
