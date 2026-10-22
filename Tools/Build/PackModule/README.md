# PackModule

宿主侧 C++20 工具：把 ELF64 `ET_DYN` 模块（`LINK_MODE=module` 产物，不 strip）打包成内核模块容器 `.osm`，或用同一份解析器核对已有容器。编译后输出 `Tools/Bin/PackModule`。

容器字节表以[内核模块容器](../../../Docs/Specs/内核模块容器.md)为唯一事实源；接口 `InterfaceId` 与模块 `ModuleId` 的派生按[模块契约层](../../../Docs/Specs/模块契约层.md)第三节。容器写出器与解析校验器同源放在 `Src/Osm.cpp`，`--verify` 与 `Tools/Test/CheckOsm` 都走这一份实现；ELF 读取与翻译在 `Src/Elf.cpp`，`Src/main.cpp` 只做命令行。

```sh
make -C Tools/Build/PackModule

# 造一个最小 ELF64 ET_DYN 夹具（gcc 的 cc 与 ld 即可，产物不进 Git）
Tools/Build/PackModule/TestData/BuildFixture.sh /tmp/packmodule

# 打包：入口符号、契约表符号与动态重定位都从 ELF 取，三张接口/依赖表由命令行声明
Tools/Bin/PackModule --elf /tmp/packmodule/module_fixture.elf --out /tmp/packmodule/module.osm \
    --name FixtureModule --version 1.2.3 --abi-min 1 --abi-max 0xFFFF \
    --export Demo.Iface:1:3 --import Core.Page:1:1:4 --depends DevModel:1:0

# 核对：按容器规则复算全部约束，不写文件
Tools/Bin/PackModule --verify /tmp/packmodule/module.osm
```

## 打包模式

| 参数 | 含义 |
| --- | --- |
| `--elf` | 未 strip 的 ELF64 `ET_DYN` 模块；必须 64 位、小端、x86-64 |
| `--out` | 输出的 `.osm` 路径 |
| `--name` | 模块逻辑名（`[A-Z][A-Za-z0-9]*`，≤64 字节）：派生 `ModuleId`，同时作为人类可读的 `ModuleName` |
| `--version` | 模块版本 `major.minor.patch`，缺省 `1.0.0`；`major` 非零 |
| `--abi-min` | `KernelAbiMin`，缺省 `1`，非零 |
| `--abi-max` | `KernelAbiMax`，缺省 `0xFFFF`（不设上限） |
| `--export` | `<稳定名>:<版本>:<槽数>`，可多次；槽表取 ELF 符号 `_Contract_Export_<稳定名去点>_v<版本>` |
| `--import` | `<稳定名>:<最低版本>:<最高版本>:<槽数>[:<符号版本>]`，可多次；回填区取 ELF 符号 `_Contract_Import_<稳定名去点>_v<符号版本>`，省略符号版本时依次试 `MaxVersion`、`MinVersion` |
| `--depends` | `<逻辑名>:<最低 major>:<最高 major>`，可多次；最高 major 为 `0` 表示不设上限 |

字段来源：

- **文件头**：`Magic`/`Version`/`HeaderBytes`/`Flags` 写规格固定值；`TotalBytes` 与 `ImageBytes` 由布局计算；三个入口偏移取 ELF 符号 `_Module_Init`（必需、非零）、`_Module_Activate`、`_Module_Abort`（可缺省或为 0）；`ModuleId` 由 `--name` 派生；版本与 ABI 范围来自命令行；`Reserved` 写零
- **段表**：从 ELF 可分配节提取，按 4 KiB 页分组：执行节 → `TEXT`（R|X），只读节 → `RODATA`（R），可写节 → `DATA`（R|W），`NOBITS` → `BSS`（R|W）；同一 4 KiB 页里出现不同权限即失败，要求模块用 `-z separate-code` 与 4 KiB 页链接。段的内存偏移沿用 ELF 地址（模块基址为 0），代码里的 RIP 相对寻址与重定位加数才成立；非 BSS 段按整页落盘，空洞与尾部零初始化数据以零字节编码；段表按 `MemoryOffset` 升序书写
- **导出表**：`InterfaceId = SHA-256("LPIF01"‖稳定名)[0:16)`；槽表符号指向函数指针数组，槽值取该位置的 `R_X86_64_RELATIVE` 加数（无重定位时取文件初值），并要求落在 `TEXT` 段
- **导入表**：`InterfaceId` 同上；回填区符号给出各函数指针槽的内存偏移，要求落在可写段且 8 字节对齐
- **依赖表**：`ModuleId = SHA-256("LPMOD01"‖逻辑名)[0:16)`；`--depends` 的 major 范围展开为 `Min=(minMajor,0,0)`、`Max=(maxMajor,0xFFFF,0xFFFF)`，最高 major 为 0 时写无上限
- **重定位表**：从 ELF 的 `SHT_RELA` 取动态重定位，只接受 `R_X86_64_RELATIVE`（类型 8，符号下标 0），翻译成 `RELATIVE64`，按 `Target` 升序书写；其他类型、符号引用型重定位、目标不在可写段都按构建错误失败
- **字符串表**：偏移 0 是空串，随后按固定顺序拼接段名（段表顺序）、模块名、导出稳定名、导入稳定名、依赖逻辑名，每条以 NUL 结尾

不进入容器的 ELF 内容：`.dynamic`/`.dynsym`/`.dynstr`/`.gnu.hash`/`.rela.*` 等链接器元数据、`.note*` 与 `.interp`；`.init_array`/`.fini_array`/`.preinit_array` 非空、TLS 节、`nm -u` 非空（有未定义外部符号）都按构建错误失败。

## 核对模式

`--verify <文件>` 用与写出器同源的 `osm::Parse` 复算全部容器约束，任一项不符即非零退出并打印英文原因：

- 头部：魔数、版本、`HeaderBytes`、`Flags`、`TotalBytes` 与实际长度、64 MiB 上限、`ImageBytes` 的 4 KiB 整数倍、保留字段全零
- 边界与布局：表偏移与段文件偏移的取值区间与对齐（段表/导出/导入/依赖 4 字节，重定位 8 字节）、表区与字符串表、槽表、回填表、段文件数据互不重叠
- 段：数量 1..16、类型白名单、类型与权限组合（`TEXT`=R|X、`RODATA`=R、`DATA`/`BSS`=R|W，W+X 非法）、对齐为 4..4096 的 2 的幂、`MemoryOffset`/`MemoryBytes` 4 KiB 整数倍、`MemoryBytes ≥ FileBytes`、BSS 无文件字节、文件区间在文件内、内存区间不越 `ImageBytes` 且两两不重叠
- 入口：`EntryInit` 非零，三个入口都在 `TEXT` 段内
- 导出：`InterfaceId` 非零、版本非零、槽数 1..64、槽位落在 `TEXT`、同 `InterfaceId+Version` 唯一
- 导入：`InterfaceId` 非零、`MinVersion` 非零且区间不颠倒、槽数 1..64、回填槽在可写段且 8 字节对齐、同 `InterfaceId` 唯一
- 依赖：`ModuleId` 非零且不是自依赖、`MinMajor` 非零、区间不颠倒、`Flags` 为零、条目不重复
- 重定位：类型只允许 `RELATIVE64`/`RELATIVE32`、保留字节为零、目标在可写段且按宽度对齐
- 字符串表：区间在文件内、末字节为 NUL、模块名与段名偏移在表内且名字以 NUL 结尾

**不在核对范围**：`Base + Addend` 的溢出（装载器知道 `Base`）、入口处是否真是函数、导入 `SlotCount` 不超过提供者槽数、跨模块提供者唯一与依赖环（装载期或依赖图检查负责；`CheckOsm` 用 `osm::VerifyDependencyGraph` 覆盖环）。

## 确定性

同一份 ELF、同一组命令行参数必须产出同样的字节：表顺序固定（头、段表、导出表与槽表、导入表与回填表、依赖表、重定位表、字符串表、段文件数据），重定位按目标升序、段按内存偏移升序，字符串表内容与保留字段规范化，不写时间戳、构建路径、用户名或调试符号。验证时同一输入两次打包的 `sha256sum` 必须一致。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 打包成功（含写出后回读自检通过）或核对通过 |
| 1 | 输入 ELF 不可用、容器构造失败或核对失败 |
| 2 | 用法错误：缺少参数、未知选项、模式冲突 |

## 已知限制（T1.4 接入点）

- 导出/导入/依赖表当前由命令行声明，尚未读 `Packages/Common/Out/Contract/` 的生成头与 `_Contract_ModuleTables`；T1.4 接入后命令行仍可作为检查用输入
- 契约表符号名按[模块契约层](../../../Docs/Specs/模块契约层.md) 4.1 的约定拼接为 `_Contract_Export_<稳定名去点>_v<版本>`；生成头的精确命名由 T1.4 对齐，届时只需改 `Src/Elf.cpp` 的符号拼接
- 模块 `ModuleName` 目前取 `--name` 的逻辑名；`ModuleId` 只由 `--name` 派生，不从 ELF 同名符号取
- 依赖表只接受 `<逻辑名>:<最低 major>:<最高 major>`，minor/patch 范围按 0/0xFFFF 展开
- 段提取基于节表（不解析程序头）；要求模块用 `-z separate-code`、4 KiB 页与不产生非 `RELATIVE` 动态重定位的方式链接
- `Tools/Test/CheckOsm` 复用本工具的 `Src/Osm.cpp`，不复制容器实现；两个工具的 `Makefile` 都按相对路径引用同一份源码
