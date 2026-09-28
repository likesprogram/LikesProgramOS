# Baleen — 系统专用引导器

Baleen 是 LikesProgramOS 的自家引导器子项目，与系统本体 `LikesProgramOS` 平级。完整目标是从受支持设备定位指定 Ext4 卷与路径，验证并装载系统内核，完成机器状态准备与交接；内核不解析外部引导协议。

| 想了解 | 看这里 |
| --- | --- |
| 引导模式与介质支持范围、产物清单、选版状态机与内核交接 | [Baleen 引导器](../../Docs/Specs/Baleen/README.md) |
| IPL 当前描述符字段、介质与内存边界、交权状态及失败语义 | [Baleen 一级引导契约](../../Docs/Specs/Baleen/一级引导契约.md) |
| Stub 职责、文件 / 内存尺寸、入口、Baleen BuildId 和摘要开发草案 | [Stub 约定与结构草案](Stub/README.md) |
| 内核侧固件边界与第三方接入条件 | [内核启动与固件边界](../../Docs/Specs/内核启动与固件边界.md) |
| 内核集选版与回滚语义 | [内核集更新与回滚](../../Docs/Specs/内核集更新与回滚.md) |
| 产物后缀约定 | [后缀设计](../../Docs/Specs/后缀设计.md) |

本目录的实现进度必须按阶段理解。

| 阶段 | 职责 | 当前状态 |
| --- | --- | --- |
| `Ipl` | 校验 `BootDescriptor` 结构、文件偏移与长度，装入 Stub，按约定交权；失败诊断并停机 | 已有 HDD / CD 分路源码和构建；实际验证结果单独记录 |
| `Stub` | A20、内存图、实模式磁盘会话、自身内存初始化、Core 装载 | 已实现并构建为真实 `BaleenStub.bin`：A20、运行期 GDT/IDT、E820 内存图、扇区探测与读盘，以及按 Core 契约的定位、高位装载与交权；自身完整性头与摘要自检仍是草案 |
| `Core` | BIOS 侧的 `BaleenLayout.bin`、Ext4、内核集摘要 / 签名、选版和持久状态、内核交接 | 尚未实现；Stub → Core 的定位、装载与交权契约已定稿（见 Stub 说明第二节），Core 侧本身还没有实现 |
| `Uefi` | UEFI 侧对应的布局、卷访问、内核集验证、选版与交接 | 尚未实现；该路径绕过 BIOS IPL 与实模式 Stub |

IPL 不解析 `BaleenLayout.bin`、Stub 头、`CoreDescriptor` 或 Ext4，不负责 Stub BSS、A20、BuildId、摘要、签名和 Core 装载。Stub/Core 未实现或占位载荷停机，不构成 IPL 缺陷；IPL 仍须独立证明满足自己的契约。HDD / CD 入口分别包含 `Hdd.inc` / `Cd.inc`，由其组织 `Init`、`Body`、`Error`、`Read`，并通过 `Init` 引入 `Const`。

`BootDescriptor`、`CoreDescriptor`、交权块与 Stub 头均未发布。当前描述符 `Version=1` 仅为开发格式标记；首发前可按需要直接修订，首发后再建立版本管理规则。本次 Stub 头草案不改变 IPL 格式或入口，不要求占位载荷采用。Stub 执行后的自校验不能构成执行前认证或信任根，其中 BuildId 属于 Baleen 构建，不是内核集 BuildId。

构建中同名占位件只服务介质组装与入口验证，不能代表正式 Stub/Core/UEFI 已完成；不得在 `Stub/Out/Bin/` 生成空壳或停机件，让“真实产物优先”误选它。正常构建只依赖正式 C++、ASM、Makefile、sh 源码与工具，不依赖不上传的 `Test`；尺寸和格式可由 NASM 汇编期断言及正式 C++ 工具检查，回归工具手工执行，不作为默认构建运行依赖。

验证报告需区分产物检查、IPL 交权、后续引导功能与真实硬件。模拟器成功不能替代真机、物理 USB、物理光驱和旧 BIOS 验证，UEFI 成功不能计作 IPL 测试通过。
