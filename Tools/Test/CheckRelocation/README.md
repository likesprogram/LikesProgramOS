# CheckRelocation

宿主侧 C++20 检查工具：M4 映像映射（`Packages/LikesProgramOS/Core/Src/ModuleImage.cpp`）与重定位（`Packages/LikesProgramOS/Core/Src/ModuleRelocation.cpp`）的宿主侧检查。编译后输出 `Tools/Bin/CheckRelocation`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckRelocation
Tools/Bin/CheckRelocation
```

容器格式见[内核模块容器](../../../Docs/Specs/内核模块容器.md)第五、十、十二节；本工具用 `vpath` 把内核目标侧源码按相对路径直接编进本工具（不复制实现），容器解析同样取交付的 `ModuleContainer.cpp`。

## 检查对象与夹具

- **检查对象**：`ModuleImage.cpp` 的 `MapModuleImage` / `ReleaseModuleImage` / `ImageErrorText`，`ModuleRelocation.cpp` 的 `ApplyModuleRelocations` / `RelocationErrorText`
- **合成容器**：工具内嵌一份规范容器（四段 TEXT / RODATA / DATA / BSS 各一页、首段之前留一页空洞、`ImageBytes=0x5000`、一条 `RELATIVE64` 与一条 `RELATIVE32`），先经真实的 `ParseModuleContainer` 解析取得 `ContainerView`，再交给映射与重定位；负例在解析之后改视图或容器字节，构造映射与重定位要拒绝的输入
- **伪映像平台**：按 `ImagePlatform`（取连续页 / 归还连续页 / 物理地址换算）注入替身
  - 取页与归还记录调用次数、起址与页数，可注入分配失败（返回 0）与越过 4 GiB 的基址
  - 物理地址换算指向工具内 128 KiB 对齐缓冲，缓冲预填 `0xEE`，因此能逐字节断言段内容、BSS / 空洞全零与重定位写入值；也可注入"不可映射"

## 检查内容

**用例（43 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，失败返回 1；用例数固定，末尾与实跑数核对，防止用例被误删。负例按 `ImageError` / `RelocationError` 分类断言（不按文本子串）。

- **映射正例（4）**：基址 4 KiB 对齐且等于映像平台给出的基址、`ImageBytes` / 页数 / 可写指针一致、一次取齐；段文件字节逐字节落位；BSS、各段未落盘尾部与段间空洞全零；连续页覆盖映像且不写映像之外
- **映射负例（21）**：视图无容器字节、映像平台无回调、`ImageBytes` 为零 / 非 4 KiB 倍数 / 超上限、页帧不足、基址未对齐并归还、基址加跨度越过 4 GiB 并归还、物理地址不可映射并归还、段类型未知、段表为空、段数超上限、BSS 带文件字节、非 BSS 段无文件字节、段文件区间越出容器、段内存跨度为零 / 偏移未对齐 / 越过 `ImageBytes` / 重叠、归还页帧并清空记录（重复归还是空操作）
- **重定位正例（3）**：`RELATIVE64` 与 `RELATIVE32` 都写入 `Base + Addend`（32 位只覆盖 4 字节）；`RELATIVE32` 结果取到 `0xFFFFFFFF`；BSS 段可写
- **重定位负例（14）**：`Base + Addend` 溢出 64 位、`RELATIVE32` 结果超出 32 位（均校验被拒绝条目不写入）、目标在 TEXT / RODATA 段、目标越过映像跨度、目标越过可写段尾、`RELATIVE64` 目标未按 8 字节对齐、未知类型、映像跨度 / 基址 / 可写指针与视图不一致、表区越过容器字节、条目数超过容器上限、视图无容器字节、没有条目时不写字节
- **夹具自检（1）**：合成容器先经真实解析器，核对段数、`ImageBytes`、入口与重定位条数

**未覆盖**：`PageAllocator` 的真实分配路径（靶机侧由 `PageAllocatorPlatform` 适配，分配与归还的语义在 `CheckMemory` 覆盖）；容器解析的全部负例（`CheckContainer`）；导入解析、依赖检查、`Init` 调用与回滚（T4.4–T4.6）；M6 的 4 KiB 精细映射与 NX / W^X（换页表后才落地）。

## 与内核集成

- 两个源文件要加入 `Packages/LikesProgramOS/Core/Makefile` 的 `LOCAL_OBJS`：`ModuleImage.o`、`ModuleRelocation.o`（本任务未改 `Core/Makefile`，登记由构建接线负责）
- 不新增链接依赖：`ModuleImage.cpp` 只用 `<Libc.hpp>` 的 `memcpy` / `memset`（内核 `Libc.o` 已提供）与 `Paging.hpp` 的 `kMapLimitBytes`；`ModuleRelocation.cpp` 只 include `ModuleRelocation.hpp`，读取条目沿用 `ModuleContainer.cpp` 的 `ReadContainerRelocation`
- 载入编排（T4.6）的用法：`Kernel::PageAllocatorPlatform(allocator)` 得到映像平台 → `MapModuleImage(view, platform, image)` → `ApplyModuleRelocations(view, image)`；任一失败按 `ImageErrorText` / `RelocationErrorText` 作为 `E-KRN-57` 的 detail，并 `ReleaseModuleImage` 回收整段页帧
- 若编排层已有自己的平台抽象（如 `LoaderPlatform` 的 `AllocatePages` / `FreePages` / `MapPhysical`），用 `ImagePlatform` 的三个回调直接桥接即可，不需要再写一套映射或重定位
- 内核没有运行库：`ImagePlatform` 是函数指针结构，不引虚表；分类枚举与文本都是常量，不动态分配

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与期望 / 实际分类），或用例数与固定值不符 |

## 破坏性自测

把实现里的一处保护改坏（例如删掉 `MapModuleImage` 的整段清零）或把某条负例的期望分类改坏后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。本仓库已验证：删掉整段清零后 3 个映射用例失败；把页帧不足的期望改成段分类后该用例失败。这一自测用于证明断言不是恒真。
