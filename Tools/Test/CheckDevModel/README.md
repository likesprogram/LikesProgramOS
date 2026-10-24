# CheckDevModel

宿主侧 C++20 检查工具：`DevModel` 与 `DriverLoader` 模块实现的宿主侧检查。编译后输出 `Tools/Bin/CheckDevModel`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckDevModel
Tools/Bin/CheckDevModel
```

契约与容器规则见[模块契约层](../../../Docs/Specs/模块契约层.md)与[内核模块容器](../../../Docs/Specs/内核模块容器.md)；本工具用 `vpath` 把模块源码按相对路径直接编进本工具（不复制实现）。

## 检查对象与替身

| 来源 | 说明 |
| --- | --- |
| `Packages/KernelModules/DevModel/Src/Pci.cpp` | PCI 枚举与 BAR 解码 |
| `Packages/KernelModules/DevModel/Src/DeviceModel.cpp` | `Dev.Model` v1 六槽 |
| `Packages/KernelModules/DriverLoader/Src/DriverRegistry.cpp` | `Drv.Loader` v1 四槽 |

不参与编译的模块源：`DevModel/Src/PciPorts.cpp`（0xCF8/0xCFC 目标侧访问）、两个 `Module.cpp`（契约表接线）；它们的符号与段落在模块 `make check` 与 `PackModule --verify` 里核对。

替身：

- **伪 PCI 配置空间**：按总线坐标登记功能，BAR 用掩码模拟「写全 1 回读」的长度探测，可观察探测后是否写回原值
- **假 BootInfo 与物理换算**：提供 BIOS 驱动器号线索、启动介质与「线索不可读」注入
- **日志收集器**：保存 `[ tag ] text` 行，用于断言固定英文标记
- **Dev.Model 槽表**：指向 `DeviceModel` 免费函数，与装载器回填后的形态一致；`DriverLoader` 通过它读写设备与绑定事实

## 检查内容

**用例（43 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，失败返回 1；用例数固定，末尾与实跑数核对，防止用例被误删。

- **PCI 枚举与 BAR（11）**：默认树数量与首条事实、多功能展开与单功能限制、总线号编码、容量截断与零容量、头类型 1 / 2 的 BAR 数量、32 位内存 BAR、I/O BAR、64 位 BAR 成对解码（含可预取）、未分配 BAR 标志、未实现与保留类型跳过、探测后恢复原值
- **DevModel 初始化与记录（8）**：记录数 / 存储控制器数 / 候选数与线索事实、重复初始化幂等、平台回调缺失、BootInfo 缺失、UEFI 路径与无线索回退、线索不可读回退、光盘线索排除 NVMe、设备表截断标记
- **DevModel 六槽（6）**：`Enumerate` 句柄连续与结束、`Resources` 资源事实与缓冲校验、`Claim` / `Release` 唯一控制者、`BootCandidates` 候选序列、`Binding` 登记 / 更新 / 清除、绑定期间拒绝归还控制权
- **DevModel v2：Register（5）**：登记成功返回“下标 + 1”的句柄、设备数推进，`Enumerate` 可见且原样保留总线类别、类码、总线地址、厂商 / 设备标识，新登记设备的 `Resources` 为空；同一总线类别重复总线地址返回 `kAlreadyExists`，不同总线类别允许同一地址；空指针、结构长度不足、总线类别为零、类码超 24 位、厂商 / 设备标识超 16 位与 `Init` 之前登记分别以 `kInvalidArgument` / `kState` 拒绝；设备表补满后继续登记返回 `kOutOfResources` 且表不增长；登记后可按 v1 语义 `Claim`，写入的绑定事实可查询回读，绑定期间 `Release` 返回 `kBusy`，清除绑定后可以归还
- **DriverLoader（12）**：初始化与参数校验、注册合法与逐项负例（版本 / 类别 / 标志 / 回调 / 字段宽度）、注册容量上限、匹配绑定与重复绑定、全通配匹配、`Start` 调用与状态推进、`Start` 失败回滚、`Stop` 回收与句柄失效、未启动绑定的 `Stop`、绑定记录同步、`.drv` 返回 `kNotSupported` 与 WARN、`Abort` 回收
- **契约自检（1）**：槽数、结构尺寸与 `kDriveSdkVersion`

**未覆盖**：`PciPorts.cpp` 的 IN/OUT 指令与真实硬件行为（需要目标机）；模块入口与契约表符号（由模块 `make check` 与 `PackModule --verify` 覆盖）；QEMU 中的启动盘控制器识别（T5.8 端到端）。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与原因），或用例数与固定值不符 |

## 破坏性自测

把模块实现里的一处保护改坏（例如让 `DriverLoader::RegisterDriver` 不再比较 `driveSdkVersion`）或把某条用例的期望值改坏后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。本仓库已验证：把 `DeviceModel::Init` 的线索回退分支去掉后回退用例失败；把注册的版本比较去掉后版本负例失败。
