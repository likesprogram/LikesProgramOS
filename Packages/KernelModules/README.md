# 内核模块包（Packages/KernelModules）

本目录产出每个模块的**输入 ELF 与打包后的 `.osm`**：各模块 `Module.mk` 先编出 ELF，`package` 目标经 `MakeContract --module-check` 校验接口声明后交 `PackModule` 打包并自动 `--verify`。顶层构建把 13 个引导闭包模块纳入内核集，随系统卷与清单一起交付；运行期夹具（`Fault` / `Optional`）只进验证变体集，不进正式发布集。模块的容器格式、入口语义与契约层规则分别在[内核模块容器](../../Docs/Specs/内核模块容器.md)、[模块契约层](../../Docs/Specs/模块契约层.md)定义；本文件只写构建侧的事实与接口约定。

## 一、目录职责

| 路径 | 职责 |
| --- | --- |
| `Common/Module.ld` | 链接布局：四段按 4 KiB 页隔离、入口固定、基址 0，见第三节 |
| `Common/ModuleRules.mk` | 模块包公共规则：编译选项、链接选项、链接后规范化、`check` 断言与 `package`（打包 + 契约校验） |
| `Common/ModuleTemplate.mk` | `Module.mk` 的模板与变量约定，新模块照它填写 |
| `Common/ModuleAbi.hpp` | 模块侧契约入口：转发生成头（`KernelError`、`ModuleId`、`KernelAbiContext`、三入口与槽位结构） |
| `Common/Contract.hpp` | 模块侧契约聚合与导入/导出表宏：`LP_CONTRACT_EXPORT`、`LP_CONTRACT_IMPORT` |
| `Common/Libc.hpp` / `Common/Libc.cpp` | 最小 C 运行库：`memcpy`/`memset`/`memcmp`/`memmove`/`strlen` |
| 13 个闭包模块目录 | `DevModel`、`DriverLoader`、`AtaPio`、`Ahci`、`Nvme`、`UsbCore`、`Xhci`、`UsbHub`、`UsbStorage`、`Vfs`、`Ext4`、`ISO9660`、`FAT32File`；逻辑名与预装顺序由 `Packages/Common/Contract/BootClosure.def` 固定，全部为真实实现，边界见各模块 README |
| `Fault/` / `Optional/` | G6d 运行期模块故障注入夹具（必需失败 / 可选失败与成功变体），只进验证变体集，见第四节 |
| `Hello/` | 模块骨架样板：不在引导闭包里，用来演示最小容器与一条 `Core.Log` 导入，复制它新增模块 |
| `Out/` | 构建产物：`Out/<模块>/<模块>.elf`、`Out/<模块>/Obj/` 与打包后的 `Out/Modules/<逻辑名>.<版本>.osm`，不提交 |
| `Common/ModuleEntry.cpp` | `_Module_Activate` / `_Module_Abort` 的弱符号默认实现，按需链接 |

## 二、构建方式

```sh
make -C Packages/KernelModules            # 构建全部模块包（ELF + 打包 .osm）
make -C Packages/KernelModules check      # 构建后逐个模块包复核 ELF 硬约束
make -C Packages/KernelModules/Hello      # 单包构建
make                      # 顶层构建把 13 个闭包模块打进内核集与系统卷
make -C Packages/KernelModules clean      # 删除 Out/
```

- 模块包由 `*/Module.mk` 是否存在识别，顶层 Makefile 不维护名单；没有模块包时构建成功（空集合合法）。
- 产物固定 `Out/<MODULE_NAME>/<MODULE_NAME>.elf`；对象在 `Out/<MODULE_NAME>/Obj/`。
- 可覆盖变量：`OUT_DIR`、`CXX`、`LD`、`OBJCOPY`、`CXXFLAGS`、`LDFLAGS`。`CXXFLAGS` 是完整替换而不是追加，覆盖时须保留第三节要求的选项。
- 顶层 `make` 已把本目录纳入：`packages` 目标构建全部模块，随后 `MakeManifest` 把 13 个闭包模块的 `.osm` 写进内核集；只改模块时也可从本目录或单包直接构建。

## 三、模块 ELF 的形态约定

链接命令（`ModuleRules.mk` 的默认值）：

```sh
ld -m elf_x86_64 -shared -nostdlib -Bsymbolic --no-undefined --build-id=none \
   -z max-page-size=0x1000 --no-ld-generated-unwind-info --orphan-handling=error \
   --fatal-warnings -T Common/Module.ld -o Out/<模块>/<模块>.elf <对象>
```

| 约定 | 实现方式 |
| --- | --- |
| ELF64、`ET_DYN` | `ld -shared`；链接基址 0，装载器按 `Base + 偏移` 使用 |
| 段按页隔离 | `.text` 从 `0x1000`、`.rodata` `0x2000`、`.data` `0x3000`、`.bss` `0x4000` 起；容器要求 `MemoryOffset`/`MemoryBytes` 是 4 KiB 整数倍，且一页只属于一个段（W^X） |
| 入口符号 | `_Module_Init` 必须存在且在 `.text`；`_Module_Activate`/`_Module_Abort` 可缺省；符号默认可见性、C 调用约定 |
| 无静态构造 / TLS / 栈展开 | `-fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables`；`--orphan-handling=error` 让 `.init_array`/`.fini_array`/`.tdata`/`.tbss`/`.eh_frame` 在链接期直接失败，不静默丢弃 |
| 无动态链接材料 | `/DISCARD/` 掉 `.dynamic`/`.dynsym`/`.dynstr`/`.hash`；`--no-undefined` 保证没有未定义符号 |
| 重定位只有 `R_X86_64_RELATIVE` | `-fPIC` + `-Bsymbolic` + `-fvisibility=hidden`；指针表放可写段；`check` 拒绝其他类型 |
| 重定位表不进入映像 | `.rela.dyn` 由 `objcopy --set-section-flags .rela.dyn=readonly` 清成非 alloc |
| 可复现 | `-ffile-prefix-map`、`--build-id=none`、无调试信息、`Out/` 不提交 |

`check` 目标逐项断言上表：ELF64/ET_DYN/x86-64、`nm -u` 为空、重定位仅 RELATIVE、必需段存在且 `sh_addr` 4 KiB 对齐、禁止段不出现、`.rela.dyn` 不带 alloc 标志、`_Module_Init` 是默认可见性的全局符号且在 `.text`、入口偏移非零。

## 四、与 `PackModule` 的接口

`PackModule` 读本目录产出的 ELF，写成 `.osm`。输入侧的事实：

| 输入项 | 取值来源 | 说明 |
| --- | --- | --- |
| ELF 路径 | `Out/<MODULE_NAME>/<MODULE_NAME>.elf` | 每个模块一个 ELF，不 strip |
| 段表 | `.text`/`.rodata`/`.data`/`.bss` 的 `sh_addr` 与 `sh_size` | `sh_addr` 即容器 `MemoryOffset`（链接基址 0）；`sh_size` 向上取整到 4 KiB 即 `MemoryBytes`；`.bss` 是 `SHT_NOBITS`，`FileBytes` 取 0 |
| 入口 | `.symtab` 中 `_Module_Init` / `_Module_Activate` / `_Module_Abort` 的 `st_value` | 即容器 `EntryInit`/`EntryActivate`/`EntryAbort`；符号缺失时对应入口写 0 |
| 重定位 | `.rela.dyn`（`SHT_RELA`，非 alloc） | 只允许 `R_X86_64_RELATIVE`；`r_offset` → 容器 `Target`，`r_addend` → 容器 `Addend`（链接基址 0，不再减基址） |
| 导出 / 导入 | `Module.mk` 的 `CONTRACT_EXPORTS` / `CONTRACT_IMPORTS` | `ModuleRules.mk` 的 `package` 先跑 `MakeContract --module-check` 校验声明并输出 `--export`/`--import` 参数（导出槽数由 `Interfaces.def` 推出）；槽表与回填区由模块源码用 `LP_CONTRACT_EXPORT`/`LP_CONTRACT_IMPORT` 定义，符号名分别是 `_Contract_Export_<稳定名去点>_v<版本>` 与 `_Contract_Import_<稳定名去点>_v<版本>`（`Core.Log` → `CoreLog`） |
| `ModuleId`、版本、`KernelAbiMin/Max` | 不在 ELF 里 | 由 `Module.mk` 与生成的契约头取值；ELF 只承载代码、段与重定位 |

产物命名 `<逻辑名>.<major>.<minor>.<patch>.osm` 由 `ModuleRules.mk` 的 `package` 目标生成并落 `Out/Modules/`；清单条目与集目录由顶层构建与 `MakeManifest` 负责。

> 接口现状冒烟：`make -C Packages/KernelModules/Hello package` 走 `MakeContract --module-check`（导出/导入声明校验）→ `PackModule` 打包 → 自动 `--verify`；Hello 带 1 条导入（`Core.Log:1:1:4`），容器 4 段、1 条重定位、`ImageBytes 0x5000`。13 个闭包模块按同一配方打包，各自的容器事实与导入面见模块 README。

## 五、已知限制

1. **契约接入已落地**：`Common/ModuleAbi.hpp` 转发生成头，手写 `KernelError`/`ModuleId`/`KernelAbiContext` 副本已删除；入口与槽表符号的 `visibility("default")` 由生成头声明给出，模块编译带 `-fvisibility=hidden` 也能被 `PackModule` 取到。
2. **重定位类型**：`-fPIC` 下当前只产 `R_X86_64_RELATIVE`（自描述表里的指针）；配方不产生 `R_X86_64_32/32S`，`check` 会拦下任何非 RELATIVE 重定位。
3. **`-fcf-protection=none`** 不在计划 T1.2 的选项清单里：本机 GCC 15 默认 `-fcf-protection=full`，会插 `endbr64` 与 `.note.gnu.property`；ring 0 不启用 CET，显式关闭以保持段表干净。换工具链时按宿主默认值复核。
4. **浮点 / SIMD** 禁用靠源码约定（契约层 6.4）：编译选项未加 `-mno-sse`，与内核 Core 的构建选项保持一致；模块源码不得使用浮点与 SIMD。
5. **公共库按模块重编**，不做 `.a`：弱符号默认入口放在静态库里不会被链接器拉入，`_Module_Activate`/`_Module_Abort` 会在链接期静默丢失。`Libc` 符号是 hidden（最终为局部符号），不进入模块对外符号面。
6. **工具链版本**：配方在 GNU ld 2.46 / GCC 15.2 上验证；`--orphan-handling=error`、`--no-ld-generated-unwind-info` 需要较新的 binutils，换更老的 `ld` 时先跑 `make check`。
7. `Out/` 已被根 `.gitignore` 忽略，不提交。
8. 不复制骨架、自己没有数据段的模块也能链接与通过 `check`，但 ELF 里会出现零长度的 `PT_LOAD` 段（链接脚本固定声明三个 PHDR）；段表由 `PackModule` 按可分配节生成，不受影响，生产模块仍建议保留骨架的四段内容。

## 六、新增模块

1. `cp -r Packages/KernelModules/Hello Packages/KernelModules/<新名>`。
2. 改 `<新名>/Module.mk`：`MODULE_NAME`、`MODULE_LOGICAL_NAME`（闭包模块必须与 `Packages/Common/Contract/BootClosure.def` 逐字一致）、`MODULE_MAJOR/MINOR/PATCH`。
3. 改 `<新名>/Src/Module.cpp` 的 `_Module_Init`；需要导出/导入时按契约层 2.4 填 `CONTRACT_EXPORTS := <稳定名>:<版本>` / `CONTRACT_IMPORTS := <稳定名>:<Min>:<Max>:<使用槽数>`（导出不写槽数，由接口定义推出），并在源码里用 `LP_CONTRACT_EXPORT(...)`/`LP_CONTRACT_IMPORT(...)` 定义表（第一个参数是稳定名去点写法）。
4. 跑 `make -C Packages/KernelModules/<新名> check`；顶层 `make -C Packages/KernelModules` 自动纳入。

变量语义与可复制模板见 `Common/ModuleTemplate.mk`。
