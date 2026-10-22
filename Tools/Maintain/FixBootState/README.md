# FixBootState

宿主侧 C++20 维护工具：在镜像或整盘设备上操作引导状态区域——报告、受控恢复、人工接受防回滚，以及**代执行两个运行期写入方**（`--publish` 对应 Updater 的 `Pending` 发布、`--confirm` 对应 Init 的 `Confirmed` 提交，调用的是与运行期同一份实现）。编译后输出 `Tools/Bin/FixBootState`。**手工执行，不进入构建与启动链**；操作对象是介质，动作会改写状态区域。

```sh
make -C Tools/Maintain/FixBootState
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --report
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --repair --audit release/boot-state.log
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --clear
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --accept-seen 3 --audit release/boot-state.log
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --publish --build-id <64 位十六进制> --digest <64 位十六进制>
Tools/Bin/FixBootState --image Out/LikesProgram.hdd --confirm 4 --revision 2
```

## 动作

| 动作 | 语义 | 失败条件 |
| --- | --- | --- |
| `--report`（缺省动作） | 给出 8 个槽位各自的结论（未写过 / 有效 / 不可用）与代次、区域结论、选中记录的全部字段（含撤销事实：纪元、最低允许安全版本、已吊销密钥；含 `ModuleRevision`，为零时标注“无修订”）；**不写任何字节** | 区域不可读 |
| `--repair` | 用有效记录重建每一个不可用槽位，把冗余恢复到满；每次写抬一代，记录内容不变（`ModuleRevision` 与撤销事实原样保留） | 没有可用记录（空区域、全部槽位不可用、最高代次出现两次）——不猜目标集 |
| `--clear` | 全部槽位清零，回到出厂形态，选版按“无记录”直启缺省集；之后首次启动重建 `Confirmed` 记录时按当前 `Modules.list` 的 `revision` 写 `ModuleRevision`（与出厂首次启动同一口径） | 介质不可写、写失败、读回不是全零 |
| `--accept-seen <版本>` | 人工接受：把 `SeenSecurityVersion` 下调到该值（防回滚的唯一例外） | 接受值不低于当前值（抬高由选择器随清单完成）、没有可用记录、写不成立 |
| `--publish --build-id <hex> --digest <hex>` | 写 `Pending`：候选标识、清单摘要与默认尝试次数；已有记录上的良好集、已见版本、撤销事实与 `ModuleRevision`（回退目标修订号）原样保留——候选发布不改变回退目标 | 候选标识全零、记录损坏、介质不可写或写不成立 |
| `--confirm <尝试身份> --revision <修订号>` | 写 `Confirmed`：**先按 `AttemptId` 比较再提交**，候选字段一次转成已知良好集字段并清零；`--revision` 是本次装载所用模块修订号（当前 `Modules.list` 的 `revision`），写进 `ModuleRevision` 字段。新写方写出的 `Confirmed` 记录该字段必须非零，**缺 `--revision` 或给 0 按参数错误拒绝** | 记录不是 `Trying`、尝试身份不符、没有可用记录、写不成立 |

选项：`--image`、`--offset`（默认 `0x21000`）、`--bytes`（默认 `4096`）、`--revision`（只与 `--confirm` 一起用）、`--audit <文件>`。退出码 `0` 成功、`1` 动作失败、`2` 参数错误。

**旧记录的兼容口径**：`ModuleRevision == 0` 的旧记录不是“修订 0”，而是“无修订”——`--report` 标注、`--repair` 写回保留 `0`；选择器把这种记录的目标修订按当前 `Modules.list` 的 `revision` 解释。`--publish` 不需要修订号：候选修订号由选择器从当前列表头解析，记录里的 `ModuleRevision` 只描述已知良好侧的**回退目标**。`--confirm` 需要显式给 `--revision`，因为本工具读不到系统卷里的 `Modules.list`，不替调用方猜修订号。

## 边界

- **只写状态区域**：写入范围限于 `StateOffset` 起的 `StateBytes` 字节，绝不碰系统卷；`--offset 0` 被拒绝（`0` 表示“没有状态区域”，是布局描述的约定）
- **不猜目标集**：没有一个槽位可验证时只报告并拒绝修复；`--clear` 是操作者的显式放弃，不是工具的自动降级
- **同一份实现**：读写经 `Packages/Common/Src/BootState.cpp`，与引导期选择器和运行期写入方（`PublishCandidate` / `ConfirmAttempt`）共用；刷盘经宿主的 `fsync`，对应真实后端的 `FLUSH CACHE`
- **代执行不等于取代**：`--publish` 与 `--confirm` 是运行期两方未就绪时的操作入口，语义与校验和运行期完全一致；Updater / Init 落地后它们保留为运维通道，不改变“确认点必须是成功条件”这条规则
- **审计**：每次动作在标准输出留一行结论，`--audit` 另按 UTC 时间追加一行（动作、镜像与偏移、原区域结论、新代次；人工接受还记原值与新值）
- **只读拒绝对照共用实现**：介质不可写的判定在 `Packages/Common` 的 `medium.Writable()` 上，本工具不另设开关
- **撤销事实不由本工具改动**：`--repair` / `--clear` / `--accept-seen` 只动状态区域里它们该动的东西；清零会把撤销事实一起抹掉，那是操作者的显式放弃，不是工具的自动降级
- **不做的事**：不改清单、不动内核集文件、不重置 `BuildId`，也不把“修复成功”当成“这次启动一定安全”

规格依据：[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)第八节（恢复与维护入口、记录方式、运行期写入的介质接口）。
