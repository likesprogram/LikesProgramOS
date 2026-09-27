# MakePayloads

宿主侧 C++20 占位载荷生成器。编译后输出 `Tools/Bin/MakePayloads`，默认构建直接调用该正式工具，不依赖本地测试目录或脚本。

```sh
make -C Tools/Build/MakePayloads
Tools/Bin/MakePayloads Out/Payload
```

需要生成缺项时，PATH 中必须有 `nasm`、`mkfs.vfat`、`mkfs.ext4`。缺工具或任一生成步骤失败都会返回非零；所有生成步骤成功后才发布缺项。**任何已存在的目标都不会被覆盖**；五项齐全且通过基本标记检查时直接复用，缺项时只补缺的文件。已有文件应带对应占位标记；Ext4 的约定 UUID 只识别来源，并不能证明卷仍为空。符号链接、目录与未识别文件会被拒绝。请始终使用专门的占位输出目录。

| 产物 | 内容 |
| --- | --- |
| `BaleenStub.bin` | 16 位占位程序，向 COM1、0xE9 和 BIOS 文本输出写 `PLACEHOLDER-STUB` 后停留 |
| `BaleenCore.bin` | 4096 字节占位数据，含 `PLACEHOLDER-CORE` |
| `Efi.img` | 36 MiB FAT32，含 `/EFI/BOOT/BOOTX64.EFI`；只输出 `MAKEISO-EFI-BOOT-OK` 和 `PLACEHOLDER-EFI` 后停留 |
| `SystemVolume.img` | 16 MiB 空 Ext4，不含内核或发行内容 |
| `BaleenLayout.bin` | 512 字节占位说明，不定义正式布局格式 |

工具不实现 Stub、Core、UEFI 加载器或内核。顶层 Makefile 优先使用各子包真实产物，缺失时才选择这些占位件。

占位汇编内嵌于 C++ 源码，NASM 输出裸代码；C++ 生成 PE32+ 封装与重定位表，并直接更新空 FAT32 的目录、FAT 和主/备 FSInfo，无需挂载或 mtools。生成使用独立临时目录，正常退出及异常路径会清理临时件。

可复现设置包括固定 FAT 元数据、PE 时间戳、Ext4 UUID/哈希种子/根所有者，关闭 Ext4 延迟初始化。`SOURCE_DATE_EPOCH` 默认 `1700000000`，传给文件系统工具，同时设置 `E2FSPROGS_FAKE_TIME`；接受 `0..2147483647` 的十进制秒数。改变该值或要求全部重新生成时，请使用新的空输出目录，现有文件不会被改写。同一工具链与文件系统配置下应生成相同字节；不同版本的 NASM、dosfstools、e2fsprogs 或 `mke2fs.conf` 可能改变产物布局。
