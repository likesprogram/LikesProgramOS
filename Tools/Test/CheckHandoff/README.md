# CheckHandoff

宿主侧 C++20 检查工具：内核交接包 9 步核对（`Packages/LikesProgramOS/Core/Src/Handoff.cpp`）与清单副本解析（`ManifestView.cpp`）的宿主侧检查。编译后输出 `Tools/Bin/CheckHandoff`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckHandoff
Tools/Bin/CheckHandoff
Tools/Bin/CheckHandoff --stage Packages/LikesProgramOS/Out/KernelSetStage
```

工具用 `vpath` 把目标侧源码按相对路径直接编进本工具（不复制实现）；宿主 C 运行库提供 `memcmp` / `memcpy`，`Libc.cpp` 不链接。判定口径见[内核启动与固件边界](../../../Docs/Specs/内核启动与固件边界.md)第一节与 [Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 6.2，编号见[内核诊断编号](../../../Docs/Specs/内核诊断编号.md)。

## 复用方式与宿主替身

- 直接编译：`Packages/LikesProgramOS/Core/Src/Handoff.cpp`、`ManifestView.cpp`，以及共用的 `Packages/Common/Src/KernelImage.cpp`（`ReadU16` / `ReadU32`）与 `Packages/Common/Src/Sha256.cpp`（列表路径主名派生 `ModuleId`）；生成的契约头来自 `Packages/Common/Out/Contract/`（`BootClosure.hpp` / `ModuleId.hpp`）
- 宿主替身：把清单副本、列表副本、记录数组与 13 个模块字节放在真实缓冲里，给它们各分配一个低 4GiB 的"物理地址"；`PhysicalMap` 注入 `HostMap`，按已登记区间把物理地址换算成宿主指针，越界或未登记即返回空（模拟不可读）
- 正例用真实集产物构造：`Packages/LikesProgramOS/Out/KernelSetStage/` 的 `LikesProgram.os`、`Manifest.mft`、`Modules.list` 与 `Modules/` 下 13 个预装 `.osm`。`.os` 的身份与主版本用 `KernelImage::CheckImageData` 取，清单条目用 `ManifestView::ParseManifest` 取；记录按 Baleen README 6.2 的 16 字节布局拼出，记录数等于引导闭包成员数（13）
- 负例都在夹具副本上改字段，不改真实产物

## 检查内容

**用例（55 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，失败返回 1；用例数固定，运行时与实跑数核对，防止用例被误删。**负例按 E-KRN 编号断言**（比较 `Diagnostics::Diagnostic::code`），不按文本子串断言。

- **正例**：真实集交接包通过，核对闭包成员数 / 记录数 / 列表条目数 / 清单条目数（15 条 = 1 个 `.os` + 13 个 `.osm` + 1 个 `Modules.list`）/ 列表 major 与 revision / 模块主版本 / `BuildId` / 模块字节合计 / 三类缓冲区间与每条模块区间；另核对 `MapPhysical` 对零地址、越过 4GiB 与跨窗口末尾的区间返回空
- **存在性（E-KRN-50）**：缺清单副本地址、清单长度为 0、缺列表副本地址、列表长度为 0、缺记录数组地址
- **清单副本（E-KRN-51/52）**：清单地址越过 4GiB 恒等窗口、区间跨窗口末尾、长度超 64 KiB、区间不可读；魔数不符、条目数低于下限、`Flags` 非零、签名算法不符、清单被截断、条目路径长度越出条目区
- **列表副本（E-KRN-50/55）**：列表地址越过窗口、长度超 16 KiB、区间不可读、列表长度与清单 `Type=3` 条目登记值不符；魔数不符、列表被截断、条目不在 `Modules/` 下、路径重复、修订号为 0、条目前缀不是 `module`
- **列表与清单集合（E-KRN-54）**：清单 `.osm` 条目没有出现在列表里（集合不相等）
- **运行期条目**：列表追加一条清单已登记的 `.osm`（非闭包成员）时交接通过，`closureCount=13`、`runtimeCount=1`，运行期条目的清单下标被带出；记录占用运行期条目报 `E-KRN-54`
- **记录数组与记录（E-KRN-50/53/54/57）**：记录数组不可读、记录数 0、记录数超上限、数组头保留字段非零、数组地址越过窗口；单条记录保留字段非零、长度 0、条目下标越界、地址越过窗口、区间跨窗口末尾、模块字节不可读；记录指向非 `.osm` 条目、长度与清单不符、路径不在列表、两条记录争用同一列表条目、记录数与列表条目数不符；模块容器短于固定字段（E-KRN-57）
- **主版本门与 BuildId（E-KRN-56）**：内核模块主版本为 0、列表 major 不符、模块 `ModuleVersionMajor` 不符、`BuildId` 不符、`BuildId` 指针为空
- **期望集完整性（E-KRN-55）**：容器 `ModuleId` 不在生成期望集（未知成员）、两个记录命中同一期望集成员（重复）、闭包成员被运行期条目顶替（列表里缺该闭包路径）；列表里的非闭包条目本身不是错误

**未覆盖**：第 1 步 BootInfo 自身可读性（`E-KRN-01..04`）由 `Core.cpp` 在调用前完成；第 9 步摘要策略按规格不复算摘要，只确认条目带摘要字段（摘要与来源由 `E-KRN-58` 的装载器路径负责）；真实物理内存的直映射、页帧标占与复制接管不在本工具范围。

## 选项

| 选项 | 说明 |
| --- | --- |
| `--stage <目录>` | 集产物目录，默认 `Packages/LikesProgramOS/Out/KernelSetStage`（从仓库根执行）；目录里需要有 `LikesProgram.os`、`Manifest.mft`、`Modules.list` 与 `Modules/` |
| `-h`, `--help` | 显示用法 |

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与期望编号 / 实际编号），或用例数与固定值不符 |
| 2 | 真实集产物缺失或不可用（先跑 `make packages modules`），或命令行错误 |

## 破坏性自测

把任一用例的期望编号改坏（例如把某个 `kFailPreloadEntry` 改成 `kFailHandoffMissing`）后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。这一自测用于证明编号断言不是恒真。
