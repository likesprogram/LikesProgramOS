# CheckStub

宿主侧 C++20 回归工具：用真实的 IPL 与 Stub 产物加内嵌探针 BaleenCore 组装镜像，在 QEMU 里核对 Stub 阶段的行为与失败路径。编译后输出 `Tools/Bin/CheckStub`。**手工执行，不构成默认构建依赖。**

```sh
make -C Tools/Test/CheckStub
Tools/Bin/CheckStub --stub-dir Packages/Baleen/Stub/Out/Bin --ipl-dir Packages/Baleen/Ipl/Out/Bin
# 实模式栈探针：先建探针版 Stub（产物在 Out/Probe/Bin），再附加三例核对判定行
make -C Packages/Baleen/Stub STACK_PROBE=1
Tools/Bin/CheckStub --stub-dir Packages/Baleen/Stub/Out/Probe/Bin --ipl-dir Packages/Baleen/Ipl/Out/Bin --stack-probe
# 弹跳窗口探针：另一种探针版（产物在 Out/WindowProbe/Bin），用注入的内存图走 E-STUB-36
make -C Packages/Baleen/Stub WINDOW_PROBE=1
Tools/Bin/CheckStub --stub-dir Packages/Baleen/Stub/Out/WindowProbe/Bin --ipl-dir Packages/Baleen/Ipl/Out/Bin --window-probe
```

需要 `nasm`、`qemu-system-x86_64`，以及可执行的 `MakeImage`、`TestInstaller`——默认从可执行文件同目录寻找，`--root <项目根>` 时改用 `<项目根>/Tools/Bin`。`NASM`、`QEMU` 可指定外部程序，`CHECKSTUB_TEST_TIMEOUT` 为每例启动秒数（默认 12，范围 1..300）。

夹具是 `Src/Fixtures.h` 内嵌的探针 BaleenCore：由工具汇编后组装入口前缀与完整性头，经真实组装器门禁放进镜像，Stub 校验头与摘要后把它装到 1 MiB 并交权。探针先核对交权块的格式标记，再依次调用 `write`、`sectorSize`、`readSectors`、`writeSectors` 与 `memoryMapQuery` 五个服务，全部通过就向 `isa-debug-exit` 端口写约定值退出，任一失败经 `write` 打印带原因的行后停机。**这五个服务在 BaleenCore 运行期、Stub 已冻结的场景下被真正调用**，不是只验证交权能跳进去。

写盘服务按契约只写引导状态区（字节偏移 `0x21000`，即本次会话单位下的对应 LBA）：探针把低地址缓冲填成花码后写一个扇区，再经 `readSectors` 读回同一批扇区逐字节比对，结论行是 `write-sectors verified`；只读介质（光盘）无法写，探针要求服务返回 `0` 而不是停机，结论行是 `write-sectors read-only`。期望值由工具按介质传入（`EXPECT_WRITE_SECTORS`），探针本身不判断介质性质。另有 `CHECK_READ_FAILURE_CLEAR` 构建的探针：先读介质之外的 LBA 制造一次失败，再成功读 LBA 0，核对交权块里 `lastReadFailure` 的四个字段都被成功返回清空（结论行 `read-failure cleared`）。

31 例分五组：

- **成功路径 4 例**（HDD / CD / 4Kn HDD / HDD 失败现场清空）：前三例核对 Stub 的完整步骤输出、BaleenCore 摘要校验与五个服务（含写盘的写后读回比对；光盘一侧核对只读返回 `0`）；4Kn 用例让设备报告 4096 逻辑扇区、镜像里的位置字段仍是 512 字节基准（由 Stub 按会话单位换算），探针核对的扇区大小与会话单位一致；第四例核对成功返回把先前失败留下的读盘现场清零。
- **Stub 自身自检 4 例**：在镜像副本上篡改 Stub 头的格式标记、摘要、`ImageBytes` 与 `MemoryBytes`，核对 `FATAL` 原因文本。
- **描述符区 11 例**：头部标记、头长、头部版本、头长非契约值（由 Stub 的 CoreDescriptor 校验拒绝，后者构造出让 IPL 仍能走通的段序列）、本区起始位置、本区结束位置倒置（由 Stub 校验）、Stub 段的段长与段标记（由 IPL 的遍历先拦下）、BaleenCore 段的起始位置为零 / 落回描述符扇区 / 结束倒置。
- **BaleenCore 镜像头 9 例**：与 Stub 同构的头字段（标记、版本、标志、摘要算法、BuildId 长度、保留）、入口前缀、`ImageBytes` 与描述符不一致、`MemoryBytes` 超限。
- **摘要、读盘与装载区 3 例**：只改代码字节的摘要不符、BaleenCore 偏移指向介质之外的读盘失败、虚拟机内存压到 4 MiB 时的装载区不可用。读盘失败一例除原因文本外还核对失败现场行：失败请求的 LBA、块数、固件 `AH` 与失败阶段（`Phase=SingleEdd`）。

`--window-probe` 模式（28 例）把成功路径 4 例、装载区 1 例与摘要不符 1 例换成窗口失败 3 例：注入的内存图让"正常启动"与"装载区不可用"的前提不成立，摘要校验又在挑窗口之后，这六例在这份构建上走不到；其余拒绝路径用例不受影响，仍逐一核对。

`--stack-probe` 模式再附加 3 例（HDD / CD / 4Kn），要求 Stub 是 `STACK_PROBE=1` 的构建。这三例核对 Stub 打印的实模式栈探针判定行（`Probing the real-mode stack` 与 `Verdict=Ok`）：探针在每次实模式弹跳前后填、扫 `0x7C00` 以下的保护区，量出固件用掉的栈深；门限与判定都在 Stub 侧，工具不重复一份深度策略。探针做什么、为什么这段区域不做 E820 门禁，见 [Stub 约定与结构](../../../Packages/Baleen/Stub/README.md) 第六节。

断言方式：成功用例核对 `isa-debug-exit` 退出码与输出文本；失败用例读 `0xE9` 调试口，要求期望的 `FATAL` 原因文本出现并在短暂等待后保持。失败时保留该例的日志与证据目录（`case.txt`、`debug.log`、`qemu.log`、`console.txt`），`--keep` 无论成败都保留。

**找不到弹跳窗口**（`E-STUB-36`）由 `--window-probe` 覆盖：`WINDOW_PROBE=1` 的 Stub 在 E820 之后注入一张 `[0x10000, 0x80000)` 整段保留的内存图，三种介质各核对该编号与原因文本；判定本身的合并规则与边界由 [CheckMemory](../CheckMemory/README.md) 在宿主侧覆盖。

不覆盖：**找不到弹跳窗口**之外的固件环境差异（内存图形态、异常固件行为）依赖真机或换固件复现；**失败后的受控恢复**属维护阶段，见[引导状态记录](../../../Docs/Specs/Baleen/引导状态记录.md)第八节。
