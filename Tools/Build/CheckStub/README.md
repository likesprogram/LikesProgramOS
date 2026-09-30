# CheckStub

宿主侧 C++20 回归工具：用真实的 IPL 与 Stub 产物加内嵌探针 Core 组装镜像，在 QEMU 里核对 Stub 阶段的行为与失败路径。编译后输出 `Tools/Bin/CheckStub`。**手工执行，不构成默认构建依赖。**

```sh
make -C Tools/Build/CheckStub
Tools/Bin/CheckStub --stub-dir Packages/Baleen/Stub/Out/Bin --ipl-dir Packages/Baleen/Ipl/Out/Bin
```

需要 `nasm`、`qemu-system-x86_64`，以及可执行的 `MakeHdd`、`MakeIso`——默认从可执行文件同目录寻找，`--root <项目根>` 时改用 `<项目根>/Tools/Bin`。`NASM`、`QEMU` 可指定外部程序，`CHECKSTUB_TEST_TIMEOUT` 为每例启动秒数（默认 12，范围 1..300）。

夹具是 `Src/Fixtures.h` 内嵌的探针 Core：由工具汇编后组装入口前缀与完整性头，经真实组装器门禁放进镜像，Stub 校验头与摘要后把它装到 1 MiB 并交权。探针先核对交权块的格式标记，再依次调用 `write`、`sectorSize`、`readSectors` 与 `memoryMapQuery` 四个服务，全部通过就向 `isa-debug-exit` 端口写约定值退出，任一失败经 `write` 打印带原因的行后停机。**这四个服务在 Core 运行期、Stub 已冻结的场景下被真正调用**，不是只验证交权能跳进去。

30 例分五组：

- **成功路径 3 例**（HDD / CD / 4Kn HDD）：核对 Stub 的完整步骤输出、Core 摘要校验与四个服务；4Kn 用例的镜像按 4096 逻辑扇区组装并挂在 NVMe 设备上，探针核对的扇区大小与会话单位一致。
- **Stub 自身自检 4 例**：在镜像副本上篡改 Stub 头的格式标记、摘要、`ImageBytes` 与 `MemoryBytes`，核对 `FATAL` 原因文本。
- **CoreDescriptor 11 例**：格式标记、版本、头长、保留字段、偏移高位、未对齐、落回描述符扇区、长度为零 / 高位非零 / 超限、偏移加长度进位。
- **Core 镜像头 9 例**：与 Stub 同构的头字段（标记、版本、标志、摘要算法、BuildId 长度、保留）、入口前缀、`ImageBytes` 与描述符不一致、`MemoryBytes` 超限。
- **摘要、读盘与装载区 3 例**：只改代码字节的摘要不符、Core 偏移指向介质之外的读盘失败、虚拟机内存压到 4 MiB 时的装载区不可用。

断言方式：成功用例核对 `isa-debug-exit` 退出码与输出文本；失败用例读 `0xE9` 调试口，要求期望的 `FATAL` 原因文本出现并在短暂等待后保持。失败时保留该例的日志与证据目录（`case.txt`、`debug.log`、`qemu.log`、`console.txt`），`--keep` 无论成败都保留。

不覆盖：**找不到弹跳窗口**依赖内存图注入，当前工具不覆盖。
