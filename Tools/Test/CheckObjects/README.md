# CheckObjects

宿主侧 C++20 检查工具：核对内核对象与归属登记最小面（T4.0，`Packages/LikesProgramOS/Core/Src/Objects.cpp`）——对象表、句柄与代际、资源区间冲突、按所有者回收，以及 `Core.Object` v1 四个槽的实现表与错误码映射。编译后输出 `Tools/Bin/CheckObjects`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckObjects
Tools/Bin/CheckObjects
```

## 为什么单独立一份

对象表是装载器的归属上下文：模块 `Init` 前装载器切换到该模块的所有者，模块经导入的 `Core.Object.Register` 登记资源；`Init` 失败时按所有者回收，任何一条登记都不能漂到别的模块名下。句柄代际、资源区间冲突与按所有者回收在引导期没有第二次机会，只能在宿主侧先钉死。本工具**直接编译内核交付的 `Objects.cpp`**，不复制实现；对象层不依赖页表与汇编，因此不需要替身头。

槽位布局不手写第二份：`Core.Object` 的四个槽用生成头 `Packages/Common/Out/Contract/Slots.hpp` 的 `CoreObjectSlotsV1` 作槽表类型，实现函数的签名由 `static_assert` 逐一与生成头的槽位成员类型比对，用例经 `Kernel::Objects::kCoreObjectSlots` 同一张表调用。生成头不提交，缺失时本工具的 Makefile 会先构建 `MakeContract` 并生成一次（等价顶层 `make contract-generated`）。

## 检查内容（10 项用例）

**对象表与句柄**：

- 登记成功：句柄非零、对象数增加、句柄槽号在表内、代际从 1 开始，`Generations` / `Owner` 可查；内核核心的归属是全零 `ModuleId`
- 代际递增：槽位复用时代际加一、槽号不变、句柄变化；旧句柄在 `Release` / `Generations` / `Owner` 三个口上都按无效处理，陈旧句柄不得释放新对象；连续复用逐次加一
- 无效句柄：句柄 0、代际为 0、槽号越界（低 8 位 ≥ 表容量）、代际不符都返回 `InvalidHandle`，代际查询返回 0；空归属指针返回 `InvalidArgument`
- 参数与区间溢出：`kind` 为 0、空句柄指针、资源区间 `begin + bytes` 溢出 64 位都拒绝，且不改对象表

**冲突拒绝**：

- 重复登记：同一 `kind` 与 `value` 只允许一条（`DuplicateObject`）；不同 value 或不同 kind 可以共存；释放后可以重新登记同一对
- 资源区间：相交（含跨起点、跨终点、整个包含）返回 `RangeConflict`；相邻与终点相接允许；`bytes` 为 0 不参与检查

**归属与回收**：

- 按所有者回收：只回收目标模块的登记、别家对象保留、句柄失效；重复回收返回 0；全部回收后对象表为空
- 所有者作用域：`OwnerScope` 嵌套切换与退出恢复；显式 `SetCurrentOwner` 与 `CurrentOwner` 同源
- 复位：清空全部登记、当前所有者回到内核核心，复位前的句柄全部失效

**契约槽表**：

- `kCoreObjectSlots` 四个槽都有实现；`Register` 跟随当前所有者；错误码按固定映射返回：参数非法 → `kInvalidArgument`、重复/区间冲突 → `kAlreadyExists`、表满 → `kOutOfResources`、无效句柄 → `kInvalidArgument`；`Generations` 无效句柄返回 0

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败；失败原因与用例名打印在标准错误 |
