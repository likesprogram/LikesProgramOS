# PackImage

宿主侧 C++20 打包工具，处理 Baleen 镜像的完整性头：填 `BuildId` 与 `Digest`，再复核头字段与整幅镜像。**带头的 Stub 与 Core 同构**，工具按头里的格式标记判定类别（`BLNSTUB` / `BLNCORE`），两种镜像走同一条填充与校验路径。

编译后输出 `Tools/Bin/PackImage`，由 `Packages/Baleen/Stub` 的 Makefile 在 `objcopy` 之后调用，是 Stub 构建流程的最后一步；以后 Core 的构建也同样在链接产物落地后调用它。

```sh
make -C Tools/Build/PackImage
Tools/Bin/PackImage          Packages/Baleen/Stub/Out/Bin/BaleenStub.bin   # 填充后就地写回
Tools/Bin/PackImage --verify Packages/Baleen/Stub/Out/Bin/BaleenStub.bin   # 只校验
```

## 前提

镜像必须已经由链接器或组装器填好头里的固定字段与长度：头在文件偏移 `0x10`、入口在 `0x90`、文件起点的 `0x90` 字节是入口前缀与保留区。Stub 的长度字段由链接脚本按 `__image_end`、`__bss_end` 填出；占位 Core 由 `MakePayloads` 组装同样的布局。字段表见 [Stub 约定与结构](../../../Packages/Baleen/Stub/README.md) 第三节。缺头或格式标记不符时工具报错退出。

## 填充规则

先写入的 `BuildId` 是**本镜像内容的构建标识**。派生规则是规范定义：

```text
BuildId = SHA-256(镜像字节，BuildId 与 Digest 两字段代入 32 个零)
Digest  = SHA-256(镜像字节，Digest 字段代入 32 个零、BuildId 已填入)
```

规则要点：

- **输入清单**就是打包器看到的整个镜像文件，即文件偏移 `[0, ImageBytes)`，不单独列源文件：镜像字节是源码、配置与工具链的规范化产物，列出源文件清单不会比绑定产物更强，反而会因清单本身需要维护而与实际构建漂移。介质扇区填充、BSS 或其他运行时缓冲、构建时间、路径与环境变量都不在输入内；源码、配置与工具链的变化只要改变镜像字节，`BuildId` 就变化。
- **规范化编码**就是镜像本身：字段小端、无隐式填充、布局由各自的链接脚本或组装器固定；`BuildId` 与 `Digest` 按 32 字节原始摘要存入，不做文本编码，比较为逐字节相等。全零的 `BuildId` 保留为“未分配”，正式产物不得使用。
- **顺序**固定：`BuildId` 先于 `Digest`，不能从包含自身字段的最终文件摘要反推；两步都不改写镜像的其他字节。
- **可复现**：相同源码、配置与工具链（含 `-Ox` 优化级别、`--build-id=none` 这类不落盘设置）必须产出同样的字节，因而同样的 `BuildId`；构建不得把时间戳或路径写进镜像。
- **配对**：Stub 与 Core 的 `BuildId` 各自独立，都不能代替另一方的标识，也不能代替内核集 `BuildId`。

写入时机固定为：链接器或组装器填好长度字段、`objcopy` 产出平坦镜像之后由本工具填充；此后任何被覆盖字节的变化都必须重跑本工具。

## 校验范围

`--verify` 与填充后的复核都检查：格式标记、版本、头长、标志位、摘要算法、`BuildId` 长度、保留字段、`ImageBytes` 与实际文件长度一致、文件不超过该类别的长度上限（Stub 为两种入口共用的 `0x8000`，Core 为 `0x400000`）、入口前缀与 `EntryOffset` 一致、`MemoryBytes` 覆盖文件且不越界（Stub 另查低 64KiB 窗口）、以及整幅镜像的 SHA-256 摘要。任一项不通过即返回非零，镜像保持原样（填充失败时也不会写回）。

组装侧的门禁在 `Tools/Build/Common` 的 `BaleenImage`：`MakeImage` 与 `TestInstaller` 放置 Stub 与 Core 时对带头载荷做同样的校验并记录 `BuildId`；占位内容同样要校验头与摘要，只有完全没有头的开发占位件与显式 `--stub-unchecked` / `--core-unchecked` 的载荷才放行，被放行的载荷不构成对正式产物的检查。
