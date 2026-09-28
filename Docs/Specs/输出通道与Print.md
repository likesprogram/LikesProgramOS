# 输出通道与 Print

本文定义引导阶段与内核共用的文本输出接口：前端 Print、输出目标（OutTarget）的职责边界，各阶段的装配与重定向规则，无可用通道时的行为，以及当前目标实现的范围。

**一句话口径：Print 只持有一个可替换的目标指针，字符能否显示由目标决定；引导期用固件给出的文本模式、模拟器调试口与串口，内核在帧缓冲就绪后切换到自绘目标，任何阶段都没有可用目标时输出被静默丢弃——不停机、不阻塞、不影响失败判定。**

本文定义接口与装配规则，不表示内核显示模块、UEFI 侧目标或串口平台适配已经实现；实现状态与验证边界见第五、六节。

## 一、分层与职责

| 层 | 职责 | 不承担 |
| --- | --- | --- |
| `Print` 前端 | 唯一写入入口，持有当前目标，转发文本 | 不格式化、不加锁、不探测设备、不管理目标生命周期 |
| `OutTarget` 目标 | 把文本写到一条具体通道 | 不分配内存、不抛异常、不阻塞、写不出去即丢弃 |
| `MultiplexTarget` | 把一次写入扇出到多个目标 | 不做去重、不保证顺序之外的语义 |
| 阶段二进制 | 静态定义目标实例，初始化设备并 `SetTarget` | 不跨交权传递目标对象 |

目标清单与实现状态：

| 目标 | 位置 | 通道 | 状态 |
| --- | --- | --- | --- |
| `Print::VgaTextTarget` | `Packages/Baleen/Common/Include/Print/VgaTextTarget.hpp`，与 `Baleen::Devices::Vga` 同文件 | 0xB8000 文本页，接续固件光标与文本模式 | 已实现，仅引导期装配 |
| `Print::DebugPortTarget` | `Packages/Common/Include/Print/DebugPortTarget.hpp` | 0xE9 调试口，Bochs / QEMU 捕获，标准 PC 无此设备 | 已实现 |
| `Print::SerialTarget` | `Packages/Common/Include/Print/SerialTarget.hpp` | 16550 兼容串口，轮询发送 | 已实现 |
| `Print::MultiplexTarget` | `Packages/Common/Include/Print.hpp` | 多路扇出 | 已实现 |
| UEFI 文本目标 | Baleen UEFI 侧 | `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`，即 ConOut | 未实现 |
| 内核自绘目标 | 内核显示侧 | BootInfo 帧缓冲加字形渲染 | 未实现 |

**声明与实现分列**：各目标的类声明在 `Include` 下，实现在同名 `Src` 文件中（`Packages/Common/Src`、`Packages/Baleen/Common/Src`）。

**VGA 文本设备与其目标同文件、随 Baleen 引导期提供，不进入内核**；内核侧的文本通道由自绘目标与串口承担。

引导期（BIOS 路径）的装配封装在 `Baleen::PrintTargets::BiosConsole`（`Packages/Baleen/Common/Include/PrintTarget.hpp`），组合 VGA 文本、0xE9 与 COM1 三路目标。

## 二、接口契约

前端接口（`Packages/Common/Include/Print.hpp`）：

| 接口 | 语义 |
| --- | --- |
| `SetTarget(OutTarget*)` | 安装当前目标；传空指针回到未安装状态，可重复调用 |
| `Target()` | 返回当前目标；未安装时为空 |
| `Write(const char*)` | 写入以 `'\0'` 结尾的文本；未安装目标或空指针时丢弃 |
| `PutChar(char)` | 写入单个字符 |
| `WriteHex(uint32_t)` | 写入 `0x` 加 8 位大写十六进制，只用移位，不引入除法运行库 |
| `ClearScreen()` | 清空当前目标已显示的文本；未安装目标时丢弃。流式通道收到后不做任何事 |

目标接口为 `Ready()`、`Write(const char*)` 与 `ClearScreen()`。`Write` 与 `ClearScreen` 都提供默认实现而不是纯虚函数：引导镜像可能不带 C++ 运行库，纯虚函数会让链接引入 `__cxa_pure_virtual`。同理，目标不通过基类指针销毁，虚析构被有意省略。

清屏只对有屏幕的目标有意义，`ClearScreen` 的默认实现为空：串口与调试口是流式通道，向它们写清屏控制序列会污染自动化抓取的日志，所以这两个目标有意不实现它。`MultiplexTarget` 按加入顺序扇出，单个目标不支持不影响其他目标。

**硬规则：**

1. 目标由阶段二进制的静态存储持有，前端只存非拥有指针；目标须在安装期间保持有效。
2. 目标不分配内存、不抛异常、不阻塞、不返回错误；写入失败即丢弃。
3. 换行统一写成 `"\r\n"`；目标不做换行补全，也不翻译字符集。
4. 文本按字节传递，保证可打印的是 `0x20..0x7E`；其他字节的行为由目标决定，当前实现直接写入或丢弃。
5. 装配与写入都发生在单线程阶段；内核启用调度与抢占后由其同步设计补充串行化规则，Print 自身不加锁。
6. 静态实例必须常量初始化，不产生 `.init_array` 与运行库依赖；这一点由目标实现与验证记录共同保证。

## 三、各阶段装配与重定向

| 阶段 | 装配点 | 目标 | 说明 |
| --- | --- | --- | --- |
| IPL | 不经 Print | INT 10h 电传 + 0xE9 | 纯汇编，单字符错误码，见[一级引导契约](Baleen/一级引导契约.md)第六节 |
| Stub | `_Stub_Main` | `BiosConsole`：VGA 文本 + 0xE9 + COM1 | 文本模式由 IPL 交权路径保证 |
| Core（BIOS 路径） | 入口确认文本模式后 | 同上，独立实例 | 与 Stub 不共享对象；在自建实例之前可先用 Stub 交权块里的控制台入口过渡，见下 |
| UEFI（待实现） | Boot Services 期间 | ConOut 目标 | `ExitBootServices` 之后 ConOut 失效，必须在此之前停止使用 |
| 内核早期 | `KernelMain` 后 | BootInfo 声明的可用通道（如串口），由内核侧装配 | 帧缓冲尚未就绪；不依赖引导期 VGA 文本目标 |
| 内核运行期 | 显示模块就绪后 | 自绘目标，按需并联串口 | 用 `SetTarget` 切换，调用点不变 |

**重定向规则：**

1. 每个二进制独立装配自己的目标实例；IPL、Stub、Core、UEFI 与内核之间不传递 C++ 对象。
2. `BootInfo` 只传事实（可用输出通道、帧缓冲信息），不传接口对象；具体字段随 BootInfo 定义。
3. 目标切换是阶段行为，不是无限期回退：切换后旧目标不保证继续可用，引导期目标与内核自绘目标各有明确的生效窗口。
4. 引导期使用 BIOS 电传之外的路径是有意的：CSM 下电传是逐字符固件调用，真机上慢到肉眼可见，而 VGA 文本模式在交权后仍然有效，显存直写快几个数量级。

**Stub 与 Core 之间的唯一例外是过渡服务，不是共享对象。** Stub 交权后冻结常驻，它的交权块里带一个控制台入口（`CoreHandoff::write`），Core 在自建目标实例之前可以直接调用：入口是 C 函数指针，不传 C++ 对象，也不改变第 1 条。这样做是因为 VGA 文本目标持有光标位置，两个实例同时写显存会互相踩；Core 一旦接管输出，应当按上表自建实例并停止使用该入口，此后 Stub 的输出路径不再被调用。

## 四、通道的现实边界

- **VGA 文本模式不是跨平台基础设施。** 有 CSM 的机器上它是规范保证的能力；UEFI Class 3 机器没有该路径，0xB8000 不是固件使用的显示通道。内核自绘与 UEFI ConOut 目标正是为这类机器准备的。
- **0xE9 是模拟器专有通道**（Bochs、QEMU 捕获），标准 PC 没有对应设备，真实硬件上写入被忽略。它只作为自动化抓取与调试通道，不能是唯一输出。
- **串口目标按固定端口与分频值初始化**。默认 COM1、分频值 1（1.8432 MHz 时钟下 115200 baud）；端口不存在时轮询达到上限后丢弃字符，不挂死。初始化会重设该端口的线路参数，在依赖固件串口重定向的机器上会改变重定向会话的设置，这是引导期诊断的取舍。
- **无可用通道是合法状态。** 无头机器、未启用串口的虚拟机与纯 UEFI 机器可能一条可见通道也没有；此时 Print 静默丢弃，诊断由内存日志等其它机制承担。

## 五、实现状态

已实现：

- 前端、`MultiplexTarget` 与三个目标；声明与实现分列 `Include`/`Src`，32 位与 64 位、`-ffreestanding -fno-exceptions -fno-rtti` 下编译与重定位链接通过，无未定义符号。
- `Baleen::PrintTargets::BiosConsole` 装配；Stub 与 Core 的阶段实例已接入 Print。
- 主机侧行为验证：假显存检查字符与属性落点、多路扇出、十六进制格式、目标切换、未安装与未就绪目标的缺省行为。
- 静态初始化检查：静态目标实例不产生 `.init_array`、全局构造与 `__cxa_*`、`operator delete` 等运行库符号。
- `Print::ClearScreen`：目标接口、`MultiplexTarget` 扇出与 VGA 文本目标的清屏。QEMU 实测清屏只作用于 VGA：清屏前写入的标记在显存里被抹掉、清屏后的内容从第 0 行开始，而同一进程的串口日志两个标记都完整。

未实现：

- UEFI 文本目标（ConOut）、内核自绘目标（帧缓冲加字形）、内核内存日志 ring 与栈回溯。
- 串口的平台适配（非标准时钟、非 COM1 端口）；当前按标准 PC 假设。
- 真机验证：VGA 文本模式、串口存在性、0xE9 无设备时的行为只在模拟器与主机侧检查过。

## 六、与其它规格的关系

- IPL 的单字符诊断与 0xE9 约定见[一级引导契约](Baleen/一级引导契约.md)第六节，本文不改写它。
- 内核交权后不依赖固件，见[内核启动与固件边界](内核启动与固件边界.md)。
- 内核的完整输出与诊断能力（内存日志 ring、栈回溯、持久输出）见[内核架构与自举闭包](内核架构与自举闭包.md)第二节，Print 是其中文本通道的前端；该节列出的 VGA 文本通道由内核侧确定实现形态，本文的 VGA 文本设备与目标只服务引导期。
- 引导器的失败报告通路见 [Baleen 引导器](Baleen/README.md)第六节。

## 七、外部依据

- [Bochs bochsrc：port_e9_hack](https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html#BOCHSOPT-PORT-E9-HACK)：0xE9 调试口的模拟器约定与默认关闭状态。
- [QEMU hw/char/debugcon.c](https://github.com/qemu/qemu/blob/master/hw/char/debugcon.c)：`isa-debugcon` 对 0xE9 的模拟。
- [OSDev Wiki：Serial Ports](https://wiki.osdev.org/Serial_Ports)：16550 寄存器布局与初始化序列。
- [UEFI Specification 2.10 §12.4](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html)：`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` 作为 ConOut 的最低协议要求与至少 80×25 文本模式的规定。

模拟器与主机侧检查不能替代真机、物理串口与旧固件的验证。
