# CheckNvme

宿主侧 C++20 检查工具：NVMe 读路径模块逻辑（`Packages/KernelModules/Nvme/Src/Nvme.cpp`）的宿主侧检查。编译后输出 `Tools/Bin/CheckNvme`。**手工执行，不构成默认构建依赖**。

```sh
make -C Tools/Test/CheckNvme
Tools/Bin/CheckNvme
```

寄存器、命令编码与 Identify 字节表以 `Packages/KernelModules/Nvme/Src/NvmeRegisters.hpp` 为唯一来源，本工具用 `vpath` 把 `Nvme.cpp` 按相对路径直接编进本工具（不复制实现）；`Module.cpp`（契约表接线）不参与，它的导入/导出与段形态由模块 `make check` 与 `PackModule --verify` 核对。

## 伪设备

- **伪寄存器阵列**：实现 `CAP`/`VS`/`CC`/`CSTS`/`AQA`/`ASQ`/`ACQ` 与门铃语义——`CC.EN` 置位决定 `CSTS.RDY`，`AQA` 决定 Admin 队列深度，提交队列尾门铃触发命令执行
- **命令执行**：`Identify`（CNS 1 填控制器数据、CNS 0 填命名空间数据）、创建/删除 I/O 队列、`READ`（从伪介质拷贝到 PRP1 指向的宿主缓冲）；完成条目按阶段位写入，生产者绕圈时阶段翻转
- **队列基址以协议为准**：伪设备不预置队列指针，Admin 队列从 ASQ/ACQ 寄存器值换算，I/O 队列从创建命令的 PRP1 换算；基址为零、未按页对齐或不在登记区间都返回无效字段状态——驱动若漏写 PRP1 或写错地址，用例会直接失败（这正是真实 QEMU 上暴露过的缺陷）
- **注入**：`completeCommands = false` 模拟命令永不完成（超时），`statusOverride` 注入下一条命令的状态码，`rdyOnEnable = false` 模拟 RDY 不就绪
- **DMA 区间**：队列页与数据页都是宿主缓冲，伪设备按登记的假物理地址换算回宿主指针，控制器写 PRP1 的就是这些假地址

## 检查内容

**用例（31 项）**：每个用例打印 `[ OK ]` 或 `[ FAIL ]`，失败返回 1；用例数固定，末尾与实跑数核对，防止用例被误删。

- **初始化与寄存器（10）**：正常初始化（CC 使能、AQA/ASQ/ACQ、两条 Identify、创建 I/O 队列、门铃计数、队列页对齐）、Identify 命令编码（CNS/NSID/PRP1）、创建队列命令编码（深度/PC/CQID/队列基址 PRP1/保留 NSID）、VS 主版本不受支持、缺 NVM 命令集、RDY 超时、队列深度不足、最小内存页不匹配、队列条目大小不足、指针缺失与队列基址未对齐
- **Identify 解析（7）**：4Kn（LBADS 12 → 4096）与容量、512e（LBADS 9 → 512）、型号去尾空格与 MDTS、容量为零拒绝、LBA 格式下标越界、LBA 大小不受支持、命名空间数量为零
- **读路径（10）**：读命令编码与命令标识递增、4Kn/512e 数据一致、跨页分块、MDTS 解析与单 PRP1 分块、阶段位绕圈翻转、**完成延迟到达仍被观察到**（伪设备把完成交给测试线程、2ms 后写入）、命令超时映射 I/O 错误、状态码映射表、注入错误状态、参数与范围校验（空指针/零长度/未对齐/越界/未初始化）
- **块设备与停机（3）**：ops 表语义（容量/可写/写只读/刷新）、标识行格式（4Kn 与 512e）、停机删除队列与清使能
- **常量自检（1）**：队列条目、门铃、状态位与 Identify 偏移

**未覆盖**：`Module.cpp` 的 Dev.Model 查找、BAR0 映射与 `Drv.Loader` 登记（QEMU NVMe 档由 baleen-integration 端到端验证）；真实硬件的中断、DSTRD 非零门铃步长与多队列。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部用例通过 |
| 1 | 有用例失败（打印 `[ FAIL ]` 与原因），或用例数与固定值不符 |

## 破坏性自测

把模块实现里的一处保护改坏（例如让 `ParseNamespace` 固定返回 512 字节逻辑块）或把某条用例的期望值改坏后重新 `make`，工具必须打印 `[ FAIL ]` 并返回 1；确认后恢复源码。本仓库已验证：固定 512 字节逻辑块后 4Kn 与 LBA 大小负例失败；把 LBA 越界状态映射改坏后状态映射用例失败；**把创建队列命令的 PRP1 改回 0（真实 QEMU 上暴露过的缺陷）后 18 个用例失败**。
