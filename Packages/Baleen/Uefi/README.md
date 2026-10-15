# Packages/Baleen/Uefi — IA32 侧的不支持提示件

本包当前只产出一件：`Out/Bin/BOOTIA32.EFI` —— 一个 **PE32 的 IA32 EFI 应用**，装进 ESP 的
`\EFI\BOOT\BOOTIA32.EFI`，由**32 位 UEFI 固件**按可移动介质回退路径加载。

**它不引导任何东西，也不打算引导。** 本项目的内核入口是长模式、UEFI 侧的引导器是 x64 固件应用
（[Baleen 引导器](../../../Docs/Specs/Baleen/README.md) 3.1 把 UEFI x64 列为必须支持的环境），
所以 32 位固件上没有任何可引导的载荷。这一件的职责只有一句提示：告诉用户这台平台为什么不支持
（32 位 UEFI），以及需要什么（x64 固件 + 有长模式的 CPU），然后停住让用户读到。

```sh
make -C Packages/Baleen/Uefi          # 产出 Out/Bin/BOOTIA32.EFI
make -C Packages/Baleen/Uefi check    # 复核是 PE32 EFI 应用且提示文本在位
```

## 形态与装配

| 项 | 约定 |
| --- | --- |
| 形态 | PE32（IA32）EFI 应用，子系统 `10`；`ld -m i386pe --image-base 0x400000 --enable-reloc-section --subsystem 10` 直接链出——ImageBase 非零与 `.reloc` 段是固件加载的硬要求 |
| 入口 | `efi_main`，只用 Simple Text Output 协议的 `OutputString`；不读表里其它成员、不调用其它固件服务 |
| 装配 | 顶层 `make payloads` 把本产物拷进载荷目录，`Tools/Bin/MakePayloads` 把它装进 ESP；产物缺失时 ESP 里就没有这一项，回退路径与以前一致 |
| 停住 | 打印后 `hlt` 循环，不返回固件：返回会把控制权交回启动管理器，提示随即被下一个启动项刷掉 |

## 边界

- **不是引导路径**：即使 CPU 有长模式（32 位固件 + 64 位 CPU 的机器），本件也只提示、不拉升模式。
  真要在那类机器上引导，需要另一件 IA32 应用（验证 → `ExitBootServices` → 自建 GDT/CR3/EFER →
  跳 64 位内核入口），且要把 IA32 纳入支持范围；当前不在范围里。
- **诊断编号**：打印的是 `[ FATAL ] E-UEFI-11 …`，登记在[失败诊断编号](../../../Docs/Specs/Baleen/失败诊断编号.md)第四节。与 BaleenCore 的 `E-CORE-11` **同号异前缀**——两侧都表达“平台的位数/模式不满足内核要求”，判据不同：那边判 CPU 能力，这里判固件架构
- **只在 32 位固件上被加载**：x64 固件走 `BOOTX64.EFI`，不会加载这一件
