# CheckLoader

模块装载编排（11 步）的宿主侧检查，复用内核同一份实现：`ModuleLoader.cpp`、`ModuleRegistry.cpp`、`ModuleContainer.cpp`、`InterfaceRegistry.cpp`、`DependencyGraph.cpp`、`Objects.cpp`、`Memory.cpp` 与共享 `Sha256.cpp`。

检查对象是[内核模块容器](../../../Docs/Specs/内核模块容器.md)第十二节的 11 步装载顺序、[模块契约层](../../../Docs/Specs/模块契约层.md)第六节入口 ABI 与[模块与驱动装载](../../../Docs/Specs/模块与驱动装载.md)的失败回滚语义；平台差异（物理映射、页帧分配、入口调用）由本工具的替身实现注入，因此**不执行任何模块机器码**。

```sh
make -C Tools/Test/CheckLoader
Tools/Bin/CheckLoader        # CheckLoader: 60 cases, 0 failed
```

## 用例分组（60 项，数量固定）

| 分组 | 覆盖 |
| --- | --- |
| 正例 | 完整模块（1 导出 + 1 导入 `Core.Log` + 1 依赖 + 1 重定位）：装载成功、状态 `Initialized`、Init 调用一次、映像 3 页、段拷贝、BSS 清零、重定位写入 `Base + Addend`、导入槽回填为提供者槽指针、成功不归还页 |
| 第 1 步 摘要与来源 | 摘要不符（E-KRN-58）、清单条目不是 `.osm`（58）、闭包模块缺交接事实（58）、闭包模块来源正例 |
| 第 2/3 步 容器结构 | 魔数错、段内存区间重叠、入口不在 TEXT（均 E-KRN-57） |
| 第 4 步 标识/版本/ABI | `ModuleId` 全零（57）、主版本硬门（56）、ABI 区间不接受（59）、重复装载同一 `ModuleId`（57） |
| 第 5 步 提供者唯一 | 与内核核心已登记的同 `(InterfaceId, Version)` 冲突（59） |
| 第 6 步 映射 | 页帧分配失败（59），且注册表不留记录、Init 未执行 |
| 第 7 步 重定位 | `RELATIVE32` 溢出（57），映像页归还、记录 `Failed` |
| 第 8 步 导入解析 | 版本不存在、槽数超过提供者、无提供者（均 59） |
| 第 9 步 依赖检查 | 未装载、未 `Initialized`、版本不匹配（均 59） |
| 第 10/11 步 归属与 Init | 对象归属正确；Init 失败调用 Abort 一次、映像页归还、对象回收、记录 `Failed`、导出不发布；无 Abort 入口时不调用；导出池水位回滚 |
| 依赖拓扑（T4.5 接线） | 提供者先于消费者；成环（59）、缺提供者（59）、模块列表非法（57） |

**「Init 前不执行」**：替身平台统计入口调用次数；所有步骤 1–10 的失败用例都断言计数为 0，只有正例与 Init 失败用例出现一次调用。

## 替身平台

`FakePlatform` 实现 `Kernel::LoaderPlatform`：假物理空间是一段宿主缓冲（物理地址 = `0x20000000` + 偏移），`AllocatePages` 按水位分配、`FreePages` 记数，`CallEntry` 按入口偏移区分 `Init` 与 `Abort` 并返回可注入的 `KernelError`。装载编排把这三个回调桥接成 T4.2 的 `ImagePlatform`，所以**第 6/7 步跑的是内核同一份 `MapModuleImage` / `ApplyModuleRelocations`**（写入经 `image.memory`），只有入口调用是替身；用例因此可以注入映射失败、Init 失败、Abort 失败与「Init 里登记对象」等现场，而不需要真实页表与模块机器码。

## 容器构造器

`BuildContainer` 按容器规格第三节至第十一节的字节表拼出合法容器（文件头、段表、段文件区、导出/导入/依赖/重定位表、字符串表），用例再按需改字段或改字节。**契约生成头**（`ModuleId.hpp` / `InterfaceId.hpp` / `Slots.hpp` / `Abi.hpp`）由 Makefile 在缺失时先跑 `MakeContract` 生成，不提交。

## 退出码与输出

- `0`：全部用例通过，最后一行打印 `CheckLoader: 60 cases, 0 failed`
- `1`：有失败用例，失败行打印 `FAIL <用例名> - <detail>`
