# MakeManifest

宿主侧 C++20 工具：生成内核集清单 `Manifest.mft`（格式标记 `LPMS`）与模块加载列表 `Modules.list`，或按清单与实际产物重算核对；签名区留零，再由 [SignManifest](../SignManifest/README.md) 填写签名。编译后输出 `Tools/Bin/MakeManifest`。

`Modules.list` 是**内核侧书写的完整模块加载列表**：条目由该修订的全部 `--artifact 2:` 自动生成，条目集合必须与清单 `Type=2` 条目集合相等；Baleen 只预装其中命中引导闭包期望集的成员，其余条目由内核挂卷后装载。

清单与 `BuildId` 的口径见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md) 2.2/2.3；模块修订、`Modules.list` 与版本保留见[模块修订与更新](../../../Docs/Specs/模块修订与更新.md)。本工具与引导侧的解析共用 `Packages/Common/Include/Manifest.hpp` 一处偏移定义，`.os` 的身份写入与结构校验共用 `Tools/Build/Common` 的 `KernelImage`。

```sh
make -C Tools/Build/MakeManifest

# 生成：读未分配身份的原型 .os，派生发布线 BuildId，写最终 .os、Modules.list 与清单
Tools/Bin/MakeManifest --out Out/KernelSetStage/Manifest.mft \
    --kernel-in Packages/LikesProgramOS/Core/Out/Bin/LikesProgram.os \
    --kernel-out Out/KernelSetStage/LikesProgram.os --kernel-path LikesProgram.os \
    --security-version 1 --module-revision 1 --module-list Out/KernelSetStage/Modules.list \
    --build-id-out Out/BuildId

# 核对：用同一套派生函数从清单与登记的实际产物重算
Tools/Bin/MakeManifest --verify --in Static/LikesProgram/System/KernelSets/<BuildId>/Manifest.mft \
    --artifact 1:LikesProgram.os:<该集目录>/LikesProgram.os \
    --artifact 3:Modules.list:<该集目录>/Modules.list
```

## 生成模式

| 参数 | 含义 |
| --- | --- |
| `--out` | 输出的清单路径（本项目取 `Manifest.mft`） |
| `--kernel-in` | 原型 `.os`：结构必须合法，`BuildId` 与 `Digest` 两段必须全零，`ModuleMajor` 非零；已带身份的 `.os` 拒绝作为输入 |
| `--kernel-out` | 最终 `.os` 的写出路径：`BuildId` 与按新 `BuildId` 复算的 `Digest` 写在这里，必须与 `--kernel-in` 不同（路径规范化后比较） |
| `--kernel-path` | 最终 `.os` 在集内的相对路径；清单第一条 `Type=1` 条目用它，也参与 `BuildId` 派生 |
| `--security-version` | 单调安全版本号：模块修订每次发布只增不减，防回滚比较的依据 |
| `--module-revision` | 模块修订号（`u64`，非零），写进 `Modules.list`；补丁发布时单调递增 |
| `--module-list` | 生成的 `Modules.list` 输出路径；清单以 `Type=3` 条目登记它，路径固定为 `Modules.list` |
| `--artifact` | 一条 `.osm` 产物：`2:<相对路径>:<文件>`；路径必须在 `Modules/` 下且文件名带版本，可多次，顺序即 `Modules.list` 条目顺序，上限 30 条 |
| `--build-id-out` | 可选：把 64 位小写十六进制 `BuildId` 加换行写进这份文本文件，供构建链命名集目录与卷内路径 |

生成顺序固定：

1. 读 `--kernel-in` 原型：核对 `LPOSKRN` 数据与结构、`ImageBytes` 与实际文件长度一致、身份两段全零、`ModuleMajor` 非零；
2. 生成 `Modules.list`：`LPML 1`、`major <ModuleMajor>`、`revision <--module-revision>`，随后按 `--artifact` 顺序为每个 `.osm` 写一条 `module <相对路径>` 条目；
3. 算内容摘要：`C_os` 由原型按"`BuildId` 与 `Digest` 两段代入 32 个零"算出；
4. 按派生规则算 `BuildId`：`"LPKSID02"` 域标记、`.os` 相对路径与 `C_os`；**模块补丁、模块版本与列表都不参与**，同一发布线的模块修订共用同一个 `BuildId`；
5. 写 `--kernel-out`：先填 `BuildId`，再把 `Digest` 字段归零、按整幅内容算 `Digest` 填入；随后回读写出的文件复算身份；
6. 以最终 `.os`、各 `.osm` 与 `Modules.list` 的字节数与整份 SHA-256 填清单条目：`.os` 条目固定第一，`.osm` 按命令行顺序，`Modules.list` 固定 `Type=3`；列表条目与清单 `.osm` 条目一一相等（构建侧按 `Packages/Common/Contract/BootClosure.def` 的闭包顺序在前、其余模块在后传入）；
7. 写清单（头部、条目区、留零的 68 字节签名区）与 `--build-id-out` 文本。

## 核对模式

`--verify` 按同一套派生实现重算，任一项不符即非零退出、不写任何文件：

- 清单结构：`Magic`、`Version`、`HeaderBytes`、`Bytes` 与实际长度一致、`Flags`、条目区与全部保留字段、`BuildId` 非零、条目数 `2..32`、恰好一条 `Type=1`、恰好一条 `Type=3` 且路径为 `Modules.list`、路径合法且不重复、`DataBytes` 非零；**签名区内容不参与核对**（签名由 `SignManifest --verify` 负责）；
- 产物条目：命令行 `--artifact` 必须与清单条目一一对应（数量、类型、路径），逐条核对实际文件的字节数与 SHA-256；
- `Modules.list`：固定头结构合法（`LPML 1` / `major` / `revision`，字段顺序固定），`major` 等于 `.os` 的 `ModuleMajor`，`revision` 非零；条目数必须等于清单 `Type=2` 条目数，每条形如 `module <相对路径>` 的条目必须在清单里找到同路径 `.osm` 项（集合逐一相等），条目不得重复，容量不超过 16 KiB；
- `BuildId`：用清单登记的 `.os` 路径与 `C_os` 重算，必须等于清单 `BuildId`（模块字节不参与）；
- `.os` 身份：`.os` 内的 `BuildId` 必须等于清单 `BuildId`，`.os` 内的 `Digest` 必须与内容一致，`ModuleMajor` 非零。

命令行产物里至少要有恰好一条 `type=1` 与一条 `type=3`，其余按 `2:<路径>:<文件>` 给出；`--artifact` 的给出顺序不影响核对结果。

## 产物与约定

- 路径必须是相对路径：不以 `/` 开头、不含空分量与 `.` 或 `..`、不含 NUL；按字节比较与去重；模块路径必须在 `Modules/` 下
- 零字节产物一律拒绝
- 签名区（算法、密钥编号、保留字节与 64 字节签名值）全部写零，等待 `SignManifest` 填写
- **清单、最终 `.os` 与 `Modules.list` 生成后不得再改**：签名工具只填签名区，其它字节一变就要重新生成并重签；模块补丁必须生成新的 `Modules.list`、新清单并重新签名，`BuildId` 保持不变
- 内容确定性：字段顺序固定、无时间戳、`.osm` 按命令行顺序排列、保留字段写 0，同一份输入产出同样的 `BuildId`、清单、最终 `.os` 与 `Modules.list`
- 清单身份摘要 = 把签名值视作 64 个零后对整份清单的 SHA-256，与签名覆盖的是同一段字节（引导侧据此计算并交给状态记录）

谁在用它：`Packages/LikesProgramOS/Makefile` 在 `Out/KernelSetStage/` 里生成最终 `.os`、`Modules.list`、清单与 `Out/BuildId`，紧接着调 `SignManifest` 签名，再按 `BuildId` 把 `.os`、清单、列表与空的 `Modules/` 目录落进 `KernelSets/<BuildId>/`；`make check` 用核对模式按清单与实际产物重算。
