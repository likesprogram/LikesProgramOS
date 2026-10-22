# PackImage

宿主侧 C++20 工具，处理两类镜像的完整性：

- **Baleen 镜像**（Stub 与 BaleenCore 同构）：填头里的 `BuildId` 与 `Digest`，再复核头字段与整幅镜像；工具按头里的格式标记判定类别（`BLNSTUB` / `BLNCORE`），两种镜像走同一条填充与校验路径。
- **内核镜像**（`LikesProgram.os`）：文件起点是[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)，工具按 `LPOSKRN` 标记判定，**只校验、不填写**。内核镜像的 `BuildId` 是 `.os` 发布线身份（模块补丁不变），由 [MakeManifest](../MakeManifest/README.md) 读原型派生并写进最终 `.os`；本工具核对结构、`ModuleMajor` 非零、身份两段的状态与摘要，`--expect-build-id` 再与给定值逐字节比较。

编译后输出 `Tools/Bin/PackImage`：`Packages/Baleen/Stub` 与 `Packages/Baleen/Core` 的 Makefile 在 `objcopy` 之后调用它做 Baleen 镜像收尾；`Packages/LikesProgramOS` 的 `check` 用它的内核镜像校验核对内核集身份。

```sh
make -C Tools/Build/PackImage
Tools/Bin/PackImage          Packages/Baleen/Stub/Out/Bin/BaleenStub.bin        # Baleen 镜像：填充后就地写回
Tools/Bin/PackImage --verify Packages/Baleen/Stub/Out/Bin/BaleenStub.bin        # Baleen 镜像：只校验
Tools/Bin/PackImage --verify Packages/LikesProgramOS/Core/Out/Bin/LikesProgram.os        # 内核镜像：原型，身份未分配
Tools/Bin/PackImage --verify --expect-build-id <64 位十六进制> <最终 .os>       # 内核镜像：BuildId 必须等于给定值
```

## 前提

Baleen 镜像必须已经由链接器或组装器填好头里的固定字段与长度：头在文件偏移 `0x10`、入口在 `0x90`、文件起点的 `0x90` 字节是入口前缀与保留区。Stub 的长度字段由链接脚本按 `__image_end`、`__bss_end` 填出；占位 BaleenCore 由 `MakePayloads` 组装同样的布局。字段表见 [Stub 约定与结构](../../../Packages/Baleen/Stub/README.md) 第三节。

内核镜像必须已经由内核构建填好数据里的固定字段：数据在文件起点占 `0x80` 字节，长度字段由链接脚本按 `__image_end`、`__bss_end` 填出，`LoadAddress`、`EntryOffset` 与 `ModuleMajor` 由汇编常量给出，`BuildId` 与 `Digest` 两段保持全零。字段表见[内核镜像数据](../../../Docs/Specs/Baleen/内核镜像数据.md)。缺数据或格式标记不符时工具报错退出。

## 内核镜像的校验规则

内核镜像的身份只有一个来源：[MakeManifest](../MakeManifest/README.md) 在生成集时派生发布线 `BuildId`、写进最终 `.os` 并按新 `BuildId` 复算 `Digest`。`PackImage` 对内核镜像**只校验、不派生也不填写**，按身份两段的状态分三类：

| 状态 | 判据 | 处理 |
| --- | --- | --- |
| 未分配身份的原型 | `BuildId` 与 `Digest` 两段全零 | 只做结构校验，打印"未分配身份的原型"；原型是 `Core` 的构建中间产物，不是可发布产物 |
| 已分配身份 | `BuildId` 非零且 `Digest` 与整幅内容一致 | 复算摘要并核对；给出 `--expect-build-id` 时还要求镜像里的 `BuildId` 与给定值逐字节相等 |
| 非法 | 两段一个为零、一个非零 | 报错退出，不猜测语义 |

结构校验与内核侧 `CheckImageData` 同口径：格式标记、版本、数据长度、标志、摘要算法、`BuildId` 长度、`ModuleMajor` 非零、保留字段、`ImageBytes` 与实际文件长度一致、`MemoryBytes` 覆盖文件、入口落在数据之后与文件之内、装载地址按 2MiB 对齐且不低于 1MiB、声明了 `BootInfo` 版本。

`--expect-build-id` 只在校验模式下可用，且只对内核镜像有意义。不带 `--verify` 对内核镜像调用一律报错退出、不改写文件：内核镜像身份由 `MakeManifest` 生成，没有"就地补身份"这条路。

## 填充规则（Baleen 镜像）

先写入的 `BuildId` 是**该镜像内容的构建标识**。派生规则是规范定义：

```text
BuildId = SHA-256(镜像字节，BuildId 与 Digest 两字段代入 32 个零)
Digest  = SHA-256(镜像字节，Digest 字段代入 32 个零、BuildId 已填入)
```

规则要点：

- **输入清单**就是打包器看到的整个镜像文件，即文件偏移 `[0, ImageBytes)`，不单独列源文件：镜像字节是源码、配置与工具链的规范化产物，列出源文件清单不会比绑定产物更强，反而会因清单本身需要维护而与实际构建漂移。介质扇区填充、BSS 或其他运行时缓冲、构建时间、路径与环境变量都不在输入内；源码、配置与工具链的变化只要改变镜像字节，`BuildId` 就变化。
- **规范化编码**就是镜像本身：字段小端、无隐式填充、布局由各自的链接脚本或组装器固定；`BuildId` 与 `Digest` 按 32 字节原始摘要存入，不做文本编码，比较为逐字节相等。全零的 `BuildId` 保留为"未分配"，正式产物不得使用。
- **顺序**固定：`BuildId` 先于 `Digest`，不能从包含自身字段的最终文件摘要反推；两步都不改写镜像的其他字节。
- **可复现**：相同源码、配置与工具链（含 `-Ox` 优化级别、`--build-id=none` 这类不落盘设置）必须产出同样的字节，因而同样的 `BuildId`；构建不得把时间戳或路径写进镜像。
- **配对**：Stub 与 BaleenCore 的 `BuildId` 各自独立，都不能代替另一方的标识。内核集的构建级 `BuildId` 是另一套派生规则（`.os`、`.osm` 与清单共享，见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)），与 Baleen 的 `BuildId` 不通用，`PackImage` 不参与后者的派生。

写入时机固定为：链接器或组装器填好长度字段、`objcopy` 产出平坦镜像之后由本工具填充；此后任何被覆盖字节的变化都必须重跑本工具。

## 校验范围

Baleen 镜像的 `--verify` 与填充后的复核都检查：格式标记、版本、头长、标志位、摘要算法、`BuildId` 长度、保留字段、`ImageBytes` 与实际文件长度一致、文件不超过该类别的长度上限（Stub 为两种入口共用的 `0x8000`，BaleenCore 为 `0x400000`）、入口落在头之后与文件之内、入口前缀与 `EntryOffset` 一致、以及整幅镜像的 SHA-256 摘要。任一项不通过即返回非零，镜像保持原样（填充失败时也不会写回）。

内核镜像见上文"内核镜像的校验规则"：结构校验之外，原型不核对摘要，已分配身份必须复算一致，`--expect-build-id` 再核对身份。

组装侧的门禁在 `Tools/Common` 的 `BaleenImage`：`MakeImage` 与 `TestInstaller` 放置 Stub 与 BaleenCore 时对带头载荷做同样的校验并记录 `BuildId`；占位内容同样要校验头与摘要，只有完全没有头的开发占位件与显式 `--stub-unchecked` / `--core-unchecked` 的载荷才放行，被放行的载荷不构成对正式产物的检查。
