# CheckIpl

宿主侧 C++20 回归工具：核对一级引导产物的静态布局，并在 QEMU 里实测 IPL 的装载、交权与失败路径。编译后输出 `Tools/Bin/CheckIpl`。**手工执行，不构成默认构建依赖**；夹具内嵌在源码里，不读取不上传的测试目录。

```sh
make -C Tools/Build/CheckIpl
Tools/Bin/CheckIpl --layout-only Packages/Baleen/Ipl/Out
Tools/Bin/CheckIpl --ipl-dir Packages/Baleen/Ipl/Out/Bin
```

`--layout-only <IplOut>` 只做静态检查，读 `<IplOut>/Bin` 的两份产物和 `<IplOut>/Obj` 的汇编清单（清单由 `make -C Packages/Baleen/Ipl` 生成）：

| 检查项 | HDD / USB-HDD | CD |
| --- | --- | --- |
| 产物长度 | 恰好 512 字节 | 恰好 2048 字节 |
| 偏移 510..511 | `55 AA` | `55 AA` |
| 代码末端（清单里 `Code_End:` 之后的地址） | 不超过 446 | 不超过 510 |
| 代码末端到上限之间 | 全为 0 | 全为 0 |
| 保留区 | — | 偏移 8..63 与 512..2047 全为 0，代码余量至少 16 字节 |

`--ipl-dir <IplOut/Bin>` 跑 51 例启动回归：用 `MakeHdd` / `MakeIso` 与真实 IPL 组装镜像，再在 QEMU 里启动并核对。除 IPL 产物外需要 `nasm`、`qemu-system-x86_64`，以及可执行的 `MakeHdd`、`MakeIso`——默认从可执行文件同目录寻找，`--root <项目根>` 时改用 `<项目根>/Tools/Bin`。`NASM`、`QEMU` 可指定外部程序，`IPL_TEST_TIMEOUT` 为每例启动秒数（默认 12，范围 1..300）。

用例分四组：

- **交权与完整装载**：4 KiB、介质上限减 1、介质上限（HDD `0x8200`、CD `0x8000`）三种长度；核对 16 位实模式状态、`CS:IP`、`DS` / `ES` / `SS:SP`、`DL` / `DH`、中断与方向标志，以及装入的载荷字节是否完整。
- **描述符字段破坏 14 例**：魔数、版本、头长、保留字段、偏移未对齐、偏移高位、偏移落回描述符扇区、偏移加长度进位、长度为零 / 高位非零 / 超介质上限 / 上取整溢出、Stub 不可读；逐例核对 IPL 停在约定的 `D` / `S` / `L` 错误码。
- **介质形态**：混合 ISO（带 MBR）的光盘入口、HDD 与混合 ISO 的 USB 形态。
- **BIOS 故障注入**：寄存器破坏、无 EDD、批量失败并改写 DAP、前两次失败后恢复、永久失败、CD 备用 `E1` 探测；另有最大 Stub 批量失败后的逐扇区重读与 `BX` 回绕、CD 原始 `DL` 无效时回退 `E0`、`DL=0x90` 优先并保留。

失败用例同时核对退出码、`0xE9` 调试口输出、VGA 文本里的独立错误码、停机位置（与 IPL 内唯一的 `CLI`/`HLT` 循环比对）以及停机时中断已关闭。`--e9 0` 对应 `ENABLE_E9=0` 的产物：不要求 `0xE9` 有输出，改为要求它为空，只核对 VGA。

夹具是 `Src/Fixtures.h` 内嵌的两段 NASM 源码（校验载荷与故障引导器），运行时写到临时目录再汇编。

每例的日志与证据写在临时工作目录里：`case.txt`（用例名与期望）、`qemu.log`、`debug.log`、`registers.txt`、`vga.txt`。失败时保留该目录并打印路径；`--keep` 无论成败都保留。退出码为全部通过 `0`、有失败 `1`、信号中断 `128 + 信号号`。
