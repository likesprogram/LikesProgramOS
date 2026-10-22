# CheckPreload

宿主侧 C++20 检查工具：核对引导闭包预装共享机制的判定面与交接布局。编译后输出 `Tools/Bin/CheckPreload`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckPreload
Tools/Bin/CheckPreload
```

## 为什么单独立一份

预装机制（`Packages/Baleen/Core/Src/BootClosure.cpp`）同时被 BaleenCore 与 UEFI 侧链接：BaleenCore 在内存图里给模块找落点，UEFI 侧把落点交给固件页分配器，两侧共用同一份列表解析、期望集核对、最小读取面、主版本门与摘要判定。`Modules.list` 是内核侧书写的完整模块加载列表；Baleen 只预装其中命中 `BootClosure.def` 期望集的成员，其余条目是运行期模块，跳过且不报错。这些判定一旦出偏差，表现是"某个集在真机上不起"或"挪了一颗模块文件后清单与列表悄悄对不上"，靠启动矩阵抓成本高。本工具把这套判定连同 BootInfo 交接包的定宽布局在宿主侧钉死。

`/tmp` 或调试脚本里也能跑同类断言，但预装是交接包的一部分：字段偏移、记录长度、条目上限、列表容量都是公共契约，必须有随源码交付、可重复执行的检查，不能只留在个人脚本里。

## 复用方式（不复制实现）

工具按相对路径把目标侧源码直接编进来，检查的就是将要交付的那一份：

- `Packages/Baleen/Core/Src/BootClosure.cpp`：列表解析与期望集（`PlanPreload`）、内存图落点搜索（`FindModuleSpan` 与 `MemoryMapAllocator`）、逐模块核对与记录填写（`PreloadModules`）；
- `Packages/Baleen/Core/Src/ScreenFacts.cpp`：BIOS 侧屏幕事实的纯映射（`DecodeVgaScreenFacts`）；
- `Packages/Common/Src/Sha256.cpp` 与 `Packages/Common/Src/Storage/{FileSystem,Path,Error}.cpp`：目标侧摘要实现与存储抽象接线。

平台侧由替身注入：模块文件来自内存缓冲，落点来自假的对齐分配器；清单只按 `ManifestFacts` 的条目视图构造，工具不解析清单字节。目标侧按 `-m32` 编译，本工具按宿主 64 位编译同一份源码；`BootClosure.cpp` 已避免 64 位除法一类会拉进运行库辅助函数的写法，因此两种位宽下都能用 `-nostdlib` 的方式链接语义，不需要额外桩。

## 检查内容（30 项用例）

**落点搜索（`FindModuleSpan` / `MemoryMapAllocator`）**：

- 退化输入：空图、零长度、超出低 4 GiB、只有保留条目、可用区间装不下都返回 0（`E-CORE-97` 的判定面）
- 相邻的可用条目合并成一段；中间差一个字节的空洞、夹一条保留条目都不得被跨过
- 避开引导器自身装载跨度与内核装载跨度；两个跨度同时存在时继续后退
- 避开低 1 MiB：整段从 0 起可用的内存图必须从 1 MiB 起给落点，1 MiB 以下没有落点
- 末端跨 4 GiB 不上送；4 GiB 以上的条目不参与；4 GiB 下最后整页可用
- 64 位长度回绕按 4 GiB 上界截断；回绕条目不产生落点
- 结果按 4 KiB 对齐；非对齐起点向上取整
- 已分配落点互斥：分配器连续分配不重叠、归还后可复用；显式传入的已分配落点同样被避开

**列表解析（`PlanPreload`）**：

- 合法 13 条产出计划：条数、总字节、修订号、集目录、清单副本与列表副本指针、逐条清单下标 / 长度 / 摘要
- 容量超 16 KiB、条目超 30 条被拒（结构检查先于语义核对）
- 重复路径、路径不在 `Modules/` 下、路径含 `..`、条目不在清单里被拒
- 固定头与结尾换行：格式标记、主版本为零、修订号为零、缺结尾换行、行内 CR 各一例
- 期望集缺 / 重两种现场：共用 `E-CORE-96`，按诊断文本区分；非闭包条目不能顶替闭包成员
- 列表含运行期条目（`Modules/Fault…` 与嵌套的 `Modules/Optional/Optional…`）时只预装闭包成员：计划条数、总字节与条目顺序只按闭包成员计

**主版本门与最小读取面**：

- 列表主版本与 `.os` 的 `ModuleMajor` 不符（`E-CORE-95`）；来源不给主版本时按列表值
- 模块头 `0x66` 的 `ModuleVersionMajor` 与 `.os` 不符（`E-CORE-95`）
- 最小读取面五项：`Magic`、`Version`、`HeaderBytes`、`Flags`、`TotalBytes` 各一例（`E-CORE-94`）；清单 `DataBytes` 与实际文件长度不符同样按 `E-CORE-94`
- 摘要不符（`E-CORE-68`）、打开/读取失败（`E-CORE-67`）、落点分配失败（`E-CORE-97`）
- 失败不交接半套：失败后结果全零、已分配落点归还；随后跑正例记录数组头必须回到完整条数
- 大模块（96 KiB）按块读入，内容与文件字节逐字节一致

**交接布局**：

- 记录数组 `8 + 32 × 16` 字节、记录 `16` 字节、头 `8` 字节、`count` 上限 30；条目上限、列表容量、落点对齐与地址边界常量
- 记录字段偏移与 Baleen README 6.2 一致（`phys` 0x00 / `bytes` 0x08 / `artifactIndex` 0x0C / `reserved` 0x0E）
- BootInfo 五字段偏移 `0xE0 / 0xE8 / 0xF0 / 0xF8 / 0xFC` 与结构长度 `0x100`

**屏幕事实（BIOS 侧纯映射）**：

- 模式 2 / 3 → `VgaText`、0xB8000、80×25、步长 160、字节数 4000、格式 `VgaText`
- 模式 7（MDA）→ 0xB0000
- 非文本模式（0、1、4、6、0x13）与列数不是 80 都给 `None`，地址为零

## 不覆盖的部分

- **选版回退触发**：候选集预装失败后写 `Failed` 与改选良好集 / 上一修订（`E-CORE-98`）属选择器职责，由 [CheckSelect](../CheckSelect/README.md) 覆盖；本工具只判预装侧失败原因。
- **真实卷读取与落点写入**：替身文件与替身落点只验证判定，不替代启动矩阵；BIOS / UEFI 两侧真正预装进内核的端到端证据由启动矩阵给出。
- **屏幕事实的 BDA 采集**：`CollectScreenFacts` 读 0x400 段的两个字节，宿主上不模拟物理内存；本工具只核对纯映射 `DecodeVgaScreenFacts`。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过，输出 `CheckPreload: N/N cases passed` |
| 1 | 有用例失败；失败用例与原因打印在标准错误 |
