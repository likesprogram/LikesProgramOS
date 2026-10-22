# CheckRegistry

接口注册表的宿主侧检查，复用内核同一份实现 `Packages/LikesProgramOS/Core/Src/InterfaceRegistry.cpp`。

检查对象是[模块契约层](../../../Docs/Specs/模块契约层.md) 4.3 / 4.5 / 5.3 在注册表侧的判定：导出登记与提供者唯一、导入区间取最高版本、槽数校验、槽位回填、`Core.Abi` 的运行时版本核对；判定通过后由 T4.6 的装载编排消费。

```sh
make -C Tools/Test/CheckRegistry
Tools/Bin/CheckRegistry        # 21/21 cases passed
```

## 用例分组（21 项，数量固定）

| 分组 | 覆盖 |
| --- | --- |
| 导出登记 | 内核核心 10 个 `Core.*` 替身与闭包模块导出进入同一张表，提供者与槽表指针正确；模块导出与内核核心合并 |
| 提供者唯一 | 重复 `(InterfaceId, Version)` 拒绝且不覆盖先登记者；整组原子（一条失败整组不写入）；组内重复；模块与内核核心冲突；闭包单例提供者必须与生成头 `kClosureProviders` 一致 |
| 导入版本选择 | 区间内取最高版本、`0xFFFF` 上限哨兵、锁定单版本；区间不满足返回 `kNotFound`；区间非法（下限为零、下限大于上限）与全零接口标识返回 `kInvalidArgument`；未定义接口返回 `kNotFound` |
| 槽位 | 使用槽数超过所选最高版本时拒绝且不回退低版本；回填逐槽指针与边界；一站式导入解析；导入声明字段非法 |
| ABI 与生命周期 | `VerifyAbi` 核对 `Core.Abi` 槽 0/2 的实际返回值；返回值与生成头不一致返回 `kVersionMismatch`；缺 `Core.Abi` 返回 `kNotFound`；`AbiVersion()` / `DriveSdkVersion()` 与生成头一致；`Mark` / `Rollback` 撤销模块导出 |

## 替身表

用例用构造的导出表驱动注册表，**不链接**内核核心的 `_Contract_Export_*` 槽表符号：T4.4 阶段该符号尚未定义（装载器在 T4.6 定义真实的 `Core.*` 槽表），宿主检查先验证注册表逻辑；T4.6 落地后可再加链接真实导出表的用例。

替身槽表的槽数与生成头 `kKernelExports` 一致（`Core.Abi` 3、`Core.Boot` 2、`Core.Dma` 2、`Core.Log` 4、`Core.Mem` 4、`Core.Mmio` 2、`Core.Object` 4、`Core.Page` 5、`Core.PhysMap` 3、`Core.Time` 3）。

## 退出码与输出

- `0`：全部用例通过，最后一行打印 `CheckRegistry: 21/21 cases passed`
- `1`：存在失败用例、参数错误或内部用例数不符；失败用例单独打印到 stderr
- `-h` / `--help`：打印用法

## 构建依赖

`Src/main.cpp` 与内核的 `InterfaceRegistry.cpp` 一起编译；契约生成头在 `Packages/Common/Out/Contract/`，缺失时 Makefile 先跑 `MakeContract` 生成（与顶层 `contract-generated` 同一份工具）。编译选项 `-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror`。
