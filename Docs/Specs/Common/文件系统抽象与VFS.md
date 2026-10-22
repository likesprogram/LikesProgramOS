# 文件系统抽象与 VFS

**本文是存储接口与内核侧 VFS / 块设备层的定稿口径。** 它定义 Baleen 与内核共用的块设备、偏移视图、统一路径与文件系统接口，并定下内核侧块设备层（`Block.Registry`）、VFS（`Fs.Vfs`）与系统卷挂载（`Boot.Volume`）的操作语义、字段需求、系统卷核对与挂载规则、错误模型与阶段边界。按本文实现后，M5 的 Vfs（T5.6）、Ext4 只读（T5.7）与系统卷挂载读清单（T5.8）不再有「接口待定」项；M6 补入 ISO9660 / FAT32File 只读实现、运行期模块读取面与两修订选择后仍按本文的接口语义执行。

**状态：已定稿，M5 落地、M6 同步。** 共用接口声明在 `Packages/Common/Include/Storage/`，共用纯逻辑实现在 `Packages/Common/Src/Storage/`，跨模块的字段与槽位事实在生成头 `Packages/Common/Out/Contract/{Slots,Types}.hpp`（由 `MakeContract` 生成，不手改）。本文与生成头冲突时：**字段、槽位与类型宽度以生成头为准，语义与判定规则以本文为准**；冲突要回到 M0 修正，不允许两边各写一套。

**本文只定 ops 语义与判定规则，不定接口字节。** 接口 ID、稳定名、槽位与顺序、接口版本、`KernelAbiVersion`、`DriveSDKVersion`、模块入口 ABI 由[模块契约层](../模块契约层.md)定义并由生成头落实；`.osm` 容器字节、导出 / 导入表、重定位与装载步骤由[内核模块容器](../内核模块容器.md)定义。本文出现的 `Block.Registry` / `Fs.Vfs` / `Boot.Volume` 是语义归属名，槽位顺序与签名以生成头 `Slots.hpp` 为准；语义以本文为准。

策略与范围（支持哪些介质与文件系统、读写边界）见[启动介质与文件系统](启动介质与文件系统.md)；系统卷目录树见[系统卷布局](系统卷布局.md)；卷标识与布局事实的字节来源见[Baleen 布局描述](../Baleen/布局描述.md)；内核侧模块名与输出名称见[内核模块与驱动清单](../内核模块与驱动清单.md)。

## 一、口径与归属

[Baleen 引导器](../Baleen/README.md)第二节已经定下口径：**引导器与内核各自持有文件系统代码，两者不需要共享代码，但可识别的卷格式范围必须一致。** 本文按这条口径划分共用内容：

| 内容 | 共用 | 位置 |
| --- | --- | --- |
| 接口声明（块设备、偏移视图、统一路径、文件系统、错误码） | 共用一份定义，不复制第二份 | `Packages/Common/Include/Storage/` |
| 无设备依赖的纯逻辑（路径语法与切分、偏移视图、按标识查事实、错误文本） | 共用实现 | `Packages/Common/Src/Storage/`；内核 `Vfs.osm` 编译同一份源码对象，不复制第二份实现 |
| 卷格式识别范围（Ext4 特性位接受 / 拒绝清单与两个集合） | 共用一份清单头文件，两侧实现引用 | `Packages/Common/Include/Storage/Ext4Features.hpp`，规则见 3.6 |
| 块设备实现（BIOS 读盘适配、AHCI、NVMe、USB、固件块协议） | 不共用 | Baleen 各阶段 / 内核引导闭包驱动 |
| 文件系统解析（Ext4 只读最小实现、内核 Ext4、ISO9660、FAT32） | 不共用；内核 Ext4 以 Baleen 实现为移植来源，运行期不带第二份副本 | Baleen 各阶段 / 内核引导闭包 |
| 卷标识查询（Ext4 `s_uuid`） | 不共用实现，归属固定为文件系统实现，块设备层不参与 | 见 4.3 |
| 内核 VFS 与块设备层（设备句柄表、挂载表、统一路径解析、文件引用） | 内核内部，不进共用包 | `Packages/KernelModules/Vfs/` |
| 挂载后的卷内布局语义（系统卷目录树、集目录、清单路径） | 由各自规格定义，存储层不解释 | [系统卷布局](系统卷布局.md)、[内核集清单与签名](../内核集清单与签名.md) |

`Packages/Common` 的定位是「Baleen 各阶段与内核共用」，Print 前端与 `Platform` 原语同在该包；存储抽象沿用同一位置与「声明、实现分列」的组织方式。

**为什么接口共用而实现不共用：** Baleen 的 Ext4 读取是只装载镜像服务的最小只读实现（能力边界见 [Baleen 引导器](../Baleen/README.md)第五节），内核的 Ext4 最终是含日志回放与写入的完整实现；两者运行环境（32 位保护模式、无运行库 与 长模式、有内存管理）也不同。共用解析代码会把只读路径与完整实现绑死，但两侧各自实现同一格式时，特性接受范围必须由同一份清单头文件里的两个集合约束（3.6），否则会出现「引导器读得了、内核读不了」或反向的布局。**本阶段两侧都只读**：内核 Ext4 的写入与日志回放属阶段 3，因此两侧的接受集合都只取只读可解析的部分；`kKernelAccepted` 已按超集定义（含阶段 3 的日志回放位），但本阶段只读不启用。

### 1.1 与契约层及其它规格的分工

| 内容 | 定在哪 |
| --- | --- |
| 操作语义、字段需求、判定顺序、失败分支、挂载与身份核对规则 | 本文 |
| 接口 ID / 稳定名 / 槽位与顺序 / 版本策略 / `KernelAbiVersion` / `DriveSDKVersion` / 入口 ABI / 生成物落点 | [模块契约层](../模块契约层.md) |
| 块设备与文件系统 ops 的字段、顺序与宽度，三个闭包单例的槽位与签名 | 生成头 `Packages/Common/Out/Contract/{Types,Slots}.hpp`（`MakeContract` 生成）；语义与错误分面归本文 |
| `.osm` 容器字节、导出 / 导入表、重定位、装载步骤、提供者唯一 | [内核模块容器](../内核模块容器.md) |
| 错误码取值与语义 | `Packages/Common/Include/Storage/Error.hpp`（共用单点）。存储面（`BlockDeviceOps` / `FileSystemOps` / `Block.Registry` / `Fs.Vfs` / `Boot.Volume`）直接返回 `Storage::Error`；模块面（入口 / `Core.*` / `Dev.Model` / `Drv.Loader`）返回 `KernelError`；两套的转换表由契约层生成（契约层 7.3） |
| 内核诊断编号与号段 | [内核诊断编号](../内核诊断编号.md) 与 `Diagnostics.hpp`；本文不预写具体号 |
| `BootInfo` 字段与启动来源语义 | `Packages/Common/Include/BootInfo.hpp` 与 [Baleen 引导器](../Baleen/README.md) 6.1 |
| 介质访问链、必备文件系统、读写阶段策略 | [启动介质与文件系统](启动介质与文件系统.md) |

**不重复定义接口字节。** 第三节的 C++ 代码块是共用接口的现有形态（Baleen 与内核模块内部直接使用）；跨模块调用面不直接暴露 C++ 虚表，由契约层按「函数指针 + 上下文」生成——`BlockDeviceOps` / `FileSystemOps` 的槽位布局、`Block.Registry` / `Fs.Vfs` / `Boot.Volume` 的槽位与版本都以契约层第八、十节为准。两种形态的语义一一对应，改动必须同步，不维护两套行为。

## 二、分层

自下而上四层，前两层共用，后两层一侧一份实现但语义同源：

| 层 | 解决什么 | 谁实现 |
| --- | --- | --- |
| 块设备 `BlockDevice` | 固定大小逻辑块的随机访问；报告逻辑块大小、容量、可写与刷新原语 | Baleen：固件读盘 / 块协议适配；内核：引导闭包驱动 + `Block.Registry`（4.1） |
| 偏移视图 `SubDevice` | 把一段字节区间映射为独立块设备：分区视图、光盘内的 Ext4 镜像 | 共用纯逻辑实现；内核由 `Block.Registry.SubDevice` 承载（4.1） |
| 文件系统 `FileSystem` | 识别并挂载卷、按统一路径查找与读取（内核侧另有写入，属阶段 3） | Baleen Ext4 只读；内核 Ext4（本阶段只读）、ISO9660、FAT32File |
| 文件系统注册与挂载 / VFS | 注册文件系统、挂载表、统一路径解析、文件引用与目录遍历 | 内核 `Vfs.osm`（4.2）；Baleen 侧不需要 VFS |

**偏移视图是光盘路径的关键。** [启动介质与文件系统](启动介质与文件系统.md)第二节规定：光盘上的系统卷是 ISO9660 卷内的 Ext4 镜像文件，内核与引导器通过块设备偏移视图访问它；采用单区段实现时，镜像必须连续，装载时验证，不支持的布局明确拒绝。`SubDevice` 就是这条视图，`Open` 时检查对齐与边界，连续性的验证由调用方按布局事实完成（4.5）。内核侧系统卷视图的起点与长度来自 `BootInfo` 的布局事实，**不要求先解析 ISO9660 目录树**；M6 的 ISO9660 模块能按名字定位并读取 `/SystemVolume.img`，内核核心也在光盘档做同一目的的第二来源核对（最小解析 PVD + 根目录项，与 `BootInfo.volumeOffset` / `volumeBytes` 比对，`E-KRN-77` / `E-KRN-78`，见 4.4 与[启动介质与文件系统](启动介质与文件系统.md) 3.1）。两者都不成为最小闭包挂载的前置：挂载判定仍只走 `volumeOffset` 偏移视图与 Ext4 卷标识。

**Baleen 侧不需要 VFS。** 它只有「在一个卷里按一条路径取一个文件」的需求，由 `Stat` / `Open` / `Read` 直接完成。内核 VFS 是上表第四层，由本文第四节定义——不再推给「另行定义」，这样 M5 的挂载、路径解析与文件引用都有单一出处。

## 三、共用接口

以下签名是共用接口的骨架。共同的硬规则：

- 不依赖 C++ 运行库：无异常、无 RTTI、不动态分配内存、不使用虚析构（实例不由基类指针销毁，理由与 `Print::OutTarget` 相同）
- 接口方法都提供默认实现，不用纯虚函数（`= 0`）：引导镜像可能不带 C++ 运行库，纯虚函数会让链接引入 `__cxa_pure_virtual`；代价是漏写覆盖变成运行期的 `NotSupported` 而不是编译期错误，由存储层的实现数量可控
- 所有缓冲由调用方提供；接口不返回需要释放的对象
- 参数非法与不支持的情况必须显式返回错误码，不静默截断、不猜测
- **不新增第二套错误码、第二套路径语法或第二份特性清单**：本文与共用头文件是唯一出处；内核侧与引导侧引用同一份，不各写一份

### 3.1 错误模型

```cpp
// Storage/Error.hpp
namespace Storage {
    // 操作结果；除 Ok 外都表示失败，诊断文本由 ErrorMessage 给出
    enum class Error : int32_t {
        Ok = 0,            // 成功
        NotFound,          // 路径或对象不存在；目录遍历结束也用它
        NotDirectory,      // 路径中间项不是目录
        IsDirectory,       // 目标要求常规文件，实际是目录
        NotSupported,      // 卷特性或操作不受支持，含未知的 Ext4 INCOMPAT 位
        Corrupt,           // 结构非法：魔数、校验、范围、环路
        Io,                // 底层读写失败
        ReadOnly,          // 目标只读
        NoSpace,           // 空间或固定容量表耗尽
        NameTooLong,       // 路径或单个名字超出实现上限
        InvalidArgument,   // 参数非法：空指针、零长度、未对齐、越界、状态不符
    };

    // 错误码的诊断文本，不含标点；不认识的取值返回 "unknown error"
    const char* ErrorMessage(Error error);
}
```

错误码是稳定编号：复用既有取值或改语义必须按[ABI 兼容与外部契约](ABI兼容与外部契约.md)第二节走新版本，不就地改写。

**两套错误码的分面与固定映射。** 存储面只出现 `Storage::Error`（`BlockDeviceOps` / `FileSystemOps` 与 `Block.Registry` / `Fs.Vfs` / `Boot.Volume` 的槽都返回它）；模块面只出现 `KernelError`（模块入口与 `Core.*` / `Dev.Model` / `Drv.Loader` 的槽）。两套都是稳定编号，转换只有生成头 `Types.hpp` 里的 `KernelErrorFromStorage` 一张表：

| `Storage::Error` | `KernelError` |
| --- | --- |
| `Ok` | `kOk` |
| `NotFound` | `kNotFound` |
| `NotDirectory` / `IsDirectory` / `NameTooLong` / `InvalidArgument` | `kInvalidArgument` |
| `NotSupported` | `kNotSupported` |
| `Corrupt` | `kCorrupt` |
| `Io` | `kIo` |
| `ReadOnly` | `kPermission` |
| `NoSpace` | `kNoSpace` |

模块内部不得另写第二张映射，也不得把 `KernelError` 混进存储面槽位的返回；反过来 `Core.*` 槽也不返回 `Storage::Error`。**错误码不代替诊断**：存储层不产生 `E-KRN` 号，编号由内核侧的调用点选（4.4）。

三条纪律：

- **存储层只有这一套错误码**：块设备、偏移视图、文件系统与 VFS 的失败语义都由 `Storage::Error` 定义，不另造第三套；分面与映射见上表与 4.4。
- **错误码不代替诊断编号**：存储层与 `.osm` 模块都不产生 `E-KRN` 号；挂载 / 读清单失败由**内核核心**在调用点按 `E-KRN-70`–`76` 报一次（M5 已分配，见 4.4）。
- **`NotFound` 同时表示「目录遍历结束」与「没有唯一候选」**，调用方按上下文区分；不为这两类情况新增错误码。

### 3.2 块设备

```cpp
// Storage/BlockDevice.hpp
namespace Storage {
    // 块设备：固定大小逻辑块的随机访问视图
    // 逻辑块大小由设备报告，实现不得写死 512；本层接受字节偏移与长度，
    // 偏移须按块对齐、长度须是块的整数倍，跨块与非对齐请求由文件系统层自理
    class BlockDevice {
    public:
        // 逻辑块大小，字节，必须是 2 的幂；未绑定的实现返回 0
        virtual uint32_t BlockBytes() const;
        // 设备容量，字节
        virtual uint64_t CapacityBytes() const;
        // 是否可写；只读设备的 Write 返回 ReadOnly
        virtual bool Writable() const;
        // 读 [offset, offset + bytes) 到 buffer；对齐、范围或指针非法返回 InvalidArgument，
        // 底层失败返回 Io；本层不分配内存、不缓存、不重试
        virtual Error Read(uint64_t offset, void* buffer, uint32_t bytes);
        // 写 [offset, offset + bytes)；默认实现返回 ReadOnly，只读设备不必重写
        virtual Error Write(uint64_t offset, const void* buffer, uint32_t bytes);
        // 刷新写缓存；默认无缓存，返回 Ok
        virtual Error Flush();
        // 是否有真正的刷新原语：没有原语时写入仍然成立，但那次提交可能只落在设备缓存里，
        // 掉电会让它回退（回退是丢一次转换，不是损坏——环形槽位保证仍有可用记录）
        // 默认 true：内存介质与有原语的后端都按「提交即持久」处理
        virtual bool HasFlushPrimitive() const;
    };
}
```

接口方法都有默认实现（未覆盖时返回 `NotSupported`、`ReadOnly` 或 0），不用纯虚的理由见第三节开头。读与写取非 const：读盘会改动设备的内部状态（BIOS 会话、缓存、DMA 寄存器），把它写成 const 只是逻辑只读的伪装，实际实现会绕开类型系统。

语义要点：

- **逻辑块大小是设备事实**：取值必须是 2 的幂且不小于 512；本项目的实际取值是 512（512e）、4096（4Kn）与 2048（光盘）。实现不得写死，也不得把容量按固定单位折算。
- **容量是字节事实**，不要求是逻辑块的整数倍；可读范围是容量内按块对齐的最大整块区间，`Read` 越过即 `InvalidArgument`。
- **卷内块与设备块由文件系统层换算**：Ext4 的卷内块是 1024 / 2048 / 4096，设备逻辑块可能是 4096。文件系统按设备块粒度读进实例缓冲再切卷内块，**不要求块设备支持小于设备块的读**；卷内块大于设备块时按整倍数一次读多个设备块。这条换算责任在文件系统层，块设备层只认自己的块。
- **刷新原语是能力事实，不是返回值**：只有当 `HasFlushPrimitive()` 为真时，`Flush()` 成功才代表写缓存已落盘；没有原语时写入可能停在设备缓存，掉电回退（回退不构成卷结构损坏）。调用方不得按单次 `Flush()` 的返回值宣称持久化。内核侧跨模块的 ops 表把这个能力表达为首字段 `capabilities` 的 `bit0`（`kBlockCapabilityFlushPrimitive`，契约层 8.3）；未置位时 `Flush` 允许为空，注册表按「无缓存」处理且不调用。
- **不把实现约束上升为接口语义**：BaleenCore 的「高位缓冲 → 低 1MiB 弹跳窗口 → 拷贝」是适配器内部中转，文件系统层看到的是普通字节偏移读；内核驱动的 DMA 缓冲同理。

### 3.3 偏移视图

```cpp
// Storage/SubDevice.hpp
namespace Storage {
    // 子设备：把底层设备的一段字节区间映射为独立块设备
    // 用于分区视图与光盘内的 Ext4 镜像文件；不拥有底层设备，使用期间底层必须保持有效
    class SubDevice final : public BlockDevice {
    public:
        // 默认构造：未绑定状态，除事实查询外的操作返回 InvalidArgument
        SubDevice();
        // 绑定 [begin, begin + bytes)：起点须按底层块大小对齐，长度非零且不越界，
        // 任一项不满足返回 InvalidArgument 且不修改 out
        // 设备取非 const 引用，理由同块设备的读：挂载与读取会改动设备内部状态
        static Error Open(BlockDevice& device, uint64_t begin, uint64_t bytes, SubDevice& out);
        // 是否已绑定
        bool Bound() const;
        // 逻辑块大小取底层
        uint32_t BlockBytes() const override;
        // 区间长度，字节
        uint64_t CapacityBytes() const override;
        // 写能力随底层；Baleen 侧视图只读
        bool Writable() const override;
        // 区间内读；越界、未对齐或零长度返回 InvalidArgument
        Error Read(uint64_t offset, void* buffer, uint32_t bytes) override;
        // 区间内写；底层只读时返回 ReadOnly
        Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override;
        // 底层刷新；未绑定时返回 InvalidArgument
        Error Flush() override;
        // 刷新原语能力随底层：未绑定时没有原语，绑定时不替底层做假设
        bool HasFlushPrimitive() const override;
    };
}
```

`Open` 只做四项检查：底层块大小合法（非零且 2 的幂）、起点按底层块对齐、长度非零、区间不越界（含 64 位加法溢出、起点等于容量、长度取最大值）。任一不满足返回 `InvalidArgument` 且不修改 `out`；用位与与减法判定，32 位目标上不牵入 64 位除法。

**偏移、长度与块大小的换算规则（内核侧同义，4.1 的子设备槽按此实现）**：

- 设底层逻辑块大小为 `B`、视图区间为 `[begin, begin + bytes)`。绑定检查：`B != 0` 且 2 的幂；`begin & (B - 1) == 0`；`bytes != 0`；`begin <= 容量` 且 `bytes <= 容量 - begin`（减法判定，避免 `begin + bytes` 64 位溢出）。
- 视图的 `BlockBytes()` 恒等于 `B`（不重新换算、不取较大者）；`CapacityBytes()` 恒等于 `bytes`；`Writable()` / `HasFlushPrimitive()` 随底层。
- 视图内读写 `Read(o, n)` / `Write(o, n)`：要求 `o & (B - 1) == 0`、`n & (B - 1) == 0`、`n != 0`、`o <= bytes` 且 `n <= bytes - o`；映射到底层偏移 `begin + o`。任一不满足返回 `InvalidArgument`。**视图不做非对齐拼装**——跨块、非对齐与跨文件尾的拼装是文件系统层的责任（3.2）。
- **长度 `bytes` 不要求是 `B` 的整数倍**：容量是字节事实，视图尾部不足一块的字节不通过 `Read` 访问（文件系统层不会越界读）；把长度向上取整到整块会读到区间之外。
- **起点不对齐不向下取整**：不猜「大概在这里」，直接拒绝。4Kn 介质上系统卷区段起点必须是 4096 的整数倍，这是部署侧要求（见[启动介质与文件系统](启动介质与文件系统.md)第四节）。
- 子视图可嵌套（光盘内的镜像、镜像内的分区视图），每层都按各自的 `B` 重做上述检查；父视图在子视图使用期间必须有效。
- **不验证区间连续性**：分区项、ISO 内镜像文件的 extent 连续性由写入方保证、由调用方按布局事实核对（4.5）。

### 3.4 文件系统

```cpp
// Storage/FileSystem.hpp
namespace Storage {
    // 卷内对象标识：只在所属卷内有效，不跨卷使用
    struct FileId {
        uint64_t value = 0;      // Ext4 为 inode 号，其他格式按各自定义
    };

    // 对象事实
    struct FileInfo {
        uint64_t bytes = 0;      // 常规文件字节数，目录为 0
        uint64_t id = 0;         // 同 FileId::value
        bool directory = false;  // 是否目录
    };

    // 实现能力：调用方据此选择路径，不在运行期试探
    struct FileSystemCaps {
        bool writable = false;                      // 是否支持写入
        bool directoryListing = false;              // 是否支持目录遍历
        bool symlinks = false;                      // 是否解析符号链接
        NameCase nameCase = NameCase::Sensitive;    // 卷内名字的比较语义，见 3.5
    };

    // 挂载模式
    enum class MountMode : uint32_t {
        ReadOnly,    // 只读挂载
        ReadWrite,   // 读写挂载；实现不支持时 Mount 返回 NotSupported
    };

    // 文件系统：识别并挂载块设备上的卷，按统一路径索引与读取卷内对象
    // 路径的形式与分量切分见 3.5：所有实现共用同一条路径语法，查找与比较各实现自理
    class FileSystem {
    public:
        // 格式名，诊断用，如 "ext4"；未覆盖时返回 "unknown"
        virtual const char* Format() const;
        // 识别并挂载；失败返回原因且实例保持未挂载，成功后可反复调用其他方法直至 Unmount
        virtual Error Mount(BlockDevice& device, MountMode mode);
        // 卸载：释放对设备的引用，实例可再次 Mount
        virtual void Unmount();
        // 实现能力
        virtual FileSystemCaps Caps() const;
        // 按路径查询对象事实
        virtual Error Stat(const char* path, FileInfo& info);
        // 按路径解析对象标识；路径指向目录时不算错误，由调用方按 FileInfo 判定
        virtual Error Open(const char* path, FileId& id);
        // 从 offset 起读至多 bytes 字节，实际长度写入 read；读到文件尾返回 Ok 且 read 为 0
        virtual Error Read(const FileId& id, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read);
        // 从 start 起读下一个目录项，名字写入 name 并更新 start；遍历结束返回 NotFound
        virtual Error ReadDirectory(const FileId& id, uint64_t& start, char* name, uint32_t nameBytes,
                                    uint32_t& nameLength, FileInfo& info);

    protected:
        // 实现侧的扩展点：按卷格式实现这几项，基类给出共用的逐段路径遍历
        virtual Error RootId(FileId& id);                                                                     // 根目录的对象标识
        virtual Error FindChild(const FileId& directory, const char* name, uint32_t nameBytes, FileId& child); // 段查找，比较规则自定
        virtual Error StatId(const FileId& id, FileInfo& info);                                               // 按标识取事实
        Error ResolvePath(const char* path, FileId& id);                                                      // 逐段遍历，见 3.5
    };
}
```

- **`Mount` 兼任格式探测**：失败即说明这个块设备不是本实现认识的卷；调用方换下一个候选设备或下一个文件系统，不需要单独的探测接口。失败后实例必须保持未挂载，不留半挂载状态。
- **查找与比较是实现的职责**：`FindChild` 在一层目录里按卷内规则找子项——Ext4 逐字节比较，ISO9660 与 FAT32 按 ASCII 折叠比较；基类不预设任何一侧的语义。
- **共用遍历**：`ResolvePath` 用 `RootId` 与 `FindChild` 把统一路径逐段下降到目标；实现想要别的路径语义（如跟随符号链接）时可以覆盖 `Open` 与 `Stat`。
- **`Stat` 与 `Open` 的组合关系**：默认的 `Stat` 走 `Open` 再 `StatId`；能力位 `directoryListing` 为 false 的实现不必实现 `ReadDirectory`。
- **符号链接**：Baleen 依赖组装器的保证（布局描述给出的路径不得依赖符号链接，见 [Baleen 引导器](../Baleen/README.md)第五节），`symlinks` 能力位为 false 的实现遇到符号链接返回 `NotSupported`，不静默跟随。
- **实例存储由提供方持有**：Baleen 阶段用静态存储就地构造；内核侧由模块静态存储或页分配提供。接口本身不含工厂与生命周期管理，也不经基类指针销毁实例（无虚析构）。
- **卷标识不是共用基类的必选能力**：实现以自有方法给出 16 字节卷标识（Ext4 是 `s_uuid`，Baleen 的 `Ext4FileSystem::Uuid()` 即此形态）；内核侧经 `FileSystemOps` 的 `VolumeIdentity` 槽提供（契约层 8.4 槽 4），归属见 4.3。共用基类不强制该能力，不要求所有格式都提供卷标识。
- **读取边界（已定）**：`Read` 从 `offset` 起至多读 `bytes`，实际长度写回 `read`；`offset` 可以等于或大于文件长度、`bytes` 可以跨越文件尾，都返回 `Ok` 且 `read` 为实际字节数（可能为 0）。`bytes` 为 0、缓冲为空、对目录读、对象标识无效返回 `InvalidArgument`（目录返回 `IsDirectory`）。

**内核侧跨模块形态。** 文件系统模块以生成头 `Types.hpp` 的 `FileSystemOps`（9 个函数指针、首参 `context`，布局 72 字节、随 `DriveSDKVersion` 冻结）暴露上表语义：`Format` / `Mount` / `Unmount` / `Caps` / `VolumeIdentity` / `Stat` / `Open` / `Read` / `ReadDirectory`，全部返回 `Storage::Error`。`FileSystemOps` **不是接口**：不分配 `InterfaceId`、不进容器导出 / 导入表，实例经 `Fs.Vfs.RegisterFileSystem` 登记；`Fs.Vfs` 八槽与这九个字段的对应关系见 4.2。共用基类与 ops 表只维护一套行为：模块内部可以复用 `Src/Storage/` 的纯逻辑（`ResolvePath` 等），ops 只是把虚方法接线到函数指针。M6 落地的三个实现（Ext4 / ISO9660 / FAT32File）的 `Format` / `Caps` / `VolumeIdentity` / 只读边界见 4.6。

### 3.5 统一路径索引

同一套路径语法把所有支持的卷接进同一棵目录树；**卷内名字怎么比较由各实现自己决定**。共用的是索引方式（路径形式、切分、遍历），不是命中规则（比较语义）——把折叠写进共用层，会让 Ext4 或 ISO9660 中的一侧出现「卷上明明有、按路径却读不到」的情况。

**两种路径形态共用同一套分量语法**，区别只在起头与上限：

| 形态 | 起头 | 上限 | 用在哪 |
| --- | --- | --- | --- |
| VFS 绝对路径 | `/` | 1024 字节（`PathRules::kMaxBytes`，含起头分隔符） | VFS 操作入口、布局描述 `KernelPath`（另有 255 字节上限） |
| 清单相对路径 | 不以 `/` 开头 | 255 字节 | 清单的 `Path`、`Modules.list` 的 `module <相对路径>` 条目；相对**集目录** |

- **路径形式（共用）**：`/` 分隔分量，根路径为 `/`；分量非空、不含 `.` 与 `..`，不以 `/` 结尾；单个分量上限 255 字节；非 ASCII 字节不做字符集变换与换行翻译。
- **两种形态互相转换（已定）**：清单相对路径拼成 `<集根>/<相对路径>` 后按 VFS 绝对路径校验，拼接后超限按 `Corrupt` 拒绝（清单已验签不等于路径可用，不静默截断）。VFS 挂载点裁剪：绝对路径去掉挂载点前缀、补一个 `/` 得到**卷内绝对路径**交给文件系统；当前挂载点是 `/`，两者逐字节相同。
- **布局描述的内核路径**是卷内绝对路径（`BootInfo.kernelPath`），按 VFS 绝对路径校验，同时受布局描述的 255 字节上限约束；不得依赖符号链接（组装器保证）。
- **语法与切分（共用）**：`CheckPath` 给出语法判定（`InvalidArgument` / `NameTooLong`），`PathWalker` 逐段交出分量并给出总数；两者都不折叠大小写、不解析符号链接。
- **查找（各实现）**：`FindChild` 在目录里按卷内规则找到子项；实现从共用比较函数里选用——`NameEquals`（逐字节）、`NameEqualsIgnoreCase`（ASCII 折叠），不各写一份。
- **语义报告（共用类型）**：`FileSystemCaps::nameCase` 取 `Sensitive` 或 `Insensitive`，调用方（内核 VFS、Baleen 侧取内核路径的代码）据此决定自己一侧的名字处理，不替实现做折叠。
- **遍历（共用）**：`ResolvePath` 从 `RootId` 出发逐段调用 `FindChild`，把错误码原样上报；根路径直接给出根目录标识。

```cpp
// Storage/Path.hpp
namespace Storage {
    struct PathRules {
        static constexpr char kSeparator = '/';          // 分量分隔符
        static constexpr uint32_t kMaxBytes = 1024;      // 整条路径上限，含起头分隔符，不含结尾零
        static constexpr uint32_t kMaxNameBytes = 255;   // 单个分量上限
    };

    // 卷内名字的比较语义：各实现按卷格式选定，调用方从 FileSystemCaps::nameCase 得知
    enum class NameCase : uint32_t {
        Sensitive,     // 逐字节比较：Ext4 等大小写敏感的格式
        Insensitive,   // ASCII 大小写折叠后比较：ISO9660 与 FAT32
    };

    // 语法校验与逐段遍历
    Error CheckPath(const char* path);
    class PathWalker {
        explicit PathWalker(const char* path);
        Error Check() const;          // 绑定时语法检查的结果
        bool HasName() const;         // 是否还有待处理分量
        const char* Name() const;     // 当前分量起址
        uint32_t NameBytes() const;   // 当前分量字节数
        void Advance();               // 前进到下一分量
        uint32_t Count() const;       // 分量总数
    };

    // 共用的两种名字比较
    bool NameEquals(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes);
    bool NameEqualsIgnoreCase(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes);

    // ASCII 折叠单个字节；非 ASCII 字节原样返回
    constexpr char ToUpperAscii(char byte);
    constexpr char ToLowerAscii(char byte);
}
```

Baleen 侧与内核侧看到的是同一条路径写法与同一套分量切分；ISO9660 卷上的全大写名字与 Ext4 卷上的大小写敏感名字因此可以出现在同一棵树里，由各卷的实现各自命中。

### 3.6 卷格式识别范围

同一格式的位清单由共用头文件 `Packages/Common/Include/Storage/Ext4Features.hpp` 单点定义；**接受集合拆成两个常量，不再声称 Baleen 与内核接受同一集合**：

| 集合 | 语义 | 使用方 |
| --- | --- | --- |
| `kBaleenAccepted` | Baleen 只读最小实现接受的 `INCOMPAT` 集合（原 `kIncompatAccepted` 的取值） | Baleen `Ext4.cpp`；**本阶段内核 `Ext4.osm` 只读挂载也按它判定** |
| `kKernelAccepted` | 内核完整实现的最终接受集合，**超集**：`kKernelAccepted` 先并入 `kIncompatRecover`，其余日志回放 / 写入相关位按阶段 3 需要增补 | 阶段 3 起内核 Ext4 的读写与回放路径；**本阶段只读挂载不得按它接受卷** |

规则：

1. **单一头文件、两个集合**：两侧都 include 同一份头文件，不复制位清单、不由宿主工具另生成第二份声明；接受集合的取值来自同一文件，但**判定用的集合按实现阶段分**，不再要求两侧接受集合相同。
2. **子集关系由编译期断言固定**：`(kBaleenAccepted & ~kKernelAccepted) == 0`——引导器能读的卷，内核实现必须也能读；反向不成立。
3. **未知 `INCOMPAT` 位一律拒绝**，不猜语义；密钥 / 加密、内联数据、大目录、大小写折叠、MMP、`META_BG` 等在范围外的位明确拒绝并报告；解析前提位（`FILETYPE` / `EXTENTS`）在挂载阶段核对，缺失即拒绝，判定不散到解析目录项或 extent 的最后一步。
4. **位被认识 ≠ 本阶段可挂载**：本阶段只读、不回放日志。`kKernelAccepted` 里的 `RECOVER` 只表示内核完整实现最终认识该位；置 `RECOVER` 的卷或未干净卸载的卷，在本阶段 `Mount` 的**卷状态核对**阶段仍被拒绝（日志回放属阶段 3 以后）。集合决定「位是否可解析」，卷状态决定「这次能不能只读挂载」。
5. `RO_COMPAT` 两侧同口径：只拒绝 `BIGALLOC`（改变 extent 单位）；`COMMIT` 位只影响写路径。
6. **实现落点（已落地）**：`kIncompatAccepted` 保留为 `kBaleenAccepted` 的等价别名；判定函数为 `IncompatSupported(flags, accepted = kBaleenAccepted)`——默认参数就是本阶段只读集合，Baleen 与内核模块都按默认或显式 `kBaleenAccepted` 调用；`kKernelAccepted` 只在阶段 3 的写 / 回放路径显式传入。新增位必须同时改头文件与本节的集合关系说明，删位等于不兼容改动，按 [ABI 兼容与外部契约](ABI兼容与外部契约.md) 走新版本。
7. 具体位值以头文件为准，本文不复述位表。

## 四、内核侧落地口径

内核侧由 `Vfs.osm` 一个模块提供三个单例导出：`Block.Registry`、`Fs.Vfs`、`Boot.Volume`。驱动与文件系统模块**不导出**这些单例：驱动向 `Drv.Loader` 登记 `DriverDesc`（含 `BlockDeviceOps` 与 context）并 `Bind` 进 `Dev.Model`；文件系统向 `Fs.Vfs` 登记 `FileSystemOps`；**`Block.Registry.Publish` 的唯一调用者是 Vfs 模块内部的块设备层**（4.1）。与[内核模块容器](../内核模块容器.md)第七节「提供者唯一」一致——同一 `(InterfaceId, Version)` 只有一个提供者，设备与文件系统实例不是接口实例，不导出多实例接口。

本节五个小节依次定：块设备层、VFS、卷标识归属、错误码映射、系统卷挂载。

### 4.1 块设备层（`Block.Registry`）

**边界：** 登记块设备、按句柄取 ops、创建子设备视图、引用计数。**不解析卷格式、不解释布局描述、不猜设备、不做缓存与重试。**

**跨模块事实以生成头为准。** `Block.Registry` 的 4 个槽由生成头 `Slots.hpp` 定义：`Publish(DeviceHandle, const BlockDeviceOps*, void* context, uint32_t flags)`、`SubDevice(DeviceHandle parent, uint64_t begin, uint64_t bytes, uint32_t flags, DeviceHandle* child)`、`Acquire(DeviceHandle, const BlockDeviceOps**, void**)`、`Release(DeviceHandle)`，全部返回 `Storage::Error`；句柄类型 `DeviceHandle` 是 u32，**0 表示无效**。

| 概念 | 规则 |
| --- | --- |
| 设备句柄 | 真实设备的句柄来自 `Dev.Model` 的事实（`DeviceFacts.deviceHandle` / `BindingFacts.deviceHandle`），块设备层不另建编号；子设备句柄由 `SubDevice` 在同一命名空间分配、非 0。未登记句柄 → `NotFound`，句柄 0 → `InvalidArgument` |
| 诊断标识 | 登记接口不带设备名称；诊断标识由设备句柄与 ops 事实（块大小、容量）+ 设备模型事实拼出，只用于日志，不是身份、不参与路径解析与卷标识核对 |
| 登记 | `Publish(handle, ops, context, flags)`：`flags` 本阶段必须为 0（非零 `InvalidArgument`）；同一句柄只能登记一次（重复 `InvalidArgument`，不覆盖先登记的）；ops 与 context 只存指针、不复制；设备与句柄同内核寿命（M5 不注销、不移除、不热替换） |
| 取用 | `Acquire(handle, &ops, &context)` 返回 ops 与 context 并 +1 引用；`Release(handle)` −1。未登记 `NotFound`；重复或没有对应 `Acquire` 的 `Release` 返回 `InvalidArgument`。引用计数只保证有效期内句柄与 ops 不被回收，**不代替** Objects 的资源归属记录（归属仍按[模块与驱动装载](../模块与驱动装载.md)登记） |
| 子设备 | `SubDevice(parent, begin, bytes, flags, &child)`：语义与换算规则同 3.3；`flags` 本阶段必须为 0（非零 `InvalidArgument`）；起点按父块大小对齐、长度非零、区间不越界（含 64 位加法溢出），否则 `InvalidArgument`；子设备块大小 / 可写 / 刷新原语随父；父在子使用期间有效；可嵌套；不验证区间连续性 |
| 单提供者 | 接口只有 Vfs 一个提供者；每台设备是登记表里的一条，不是接口实例；子设备视图也不是接口实例，不导出多实例块设备接口 |
| 固定容量 | 设备登记表与子设备表的容量是模块构建期常量（数值登记在 Vfs 模块 README），耗尽返回 `NoSpace`；不动态扩容、不覆盖在用条目 |

**子设备视图的回收（M5 起的边界）。** 真实设备句柄由 `Dev.Model` 持有、与内核同寿命；子设备视图由 `SubDevice` 创建，`Release` 只递减引用计数，**M5 起没有注销 / 回收接口**——子设备表项在本次启动内一直占用，直到模块 `Abort` / 重新 `Init` 的 `Reset` 才清空。因此容量上界必须按「根设备数 + 探测产生的子视图数」预留（设备表 32 已覆盖 M5 的探测规模，见下）；忘记 `Release` 会表现为引用计数不归零，而不是表项回收。

**固定容量的具体数值（M5 实现单点）**：设备表（根设备 + 子设备合计）32、文件系统登记表 8、挂载表 4、文件引用表 32；数值定义在 `Packages/KernelModules/Vfs/Include/VfsCore.hpp`，模块 README 同步说明。子设备句柄从高位段分配（基址 `0x80000000`），与根设备句柄不会冲突；句柄空间耗尽同样按 `NoSpace` 处理。

**`BlockDeviceOps` 字段需求**（生成头 `Types.hpp`；首字段是能力位而不是函数指针，布局 56 字节，随 `DriveSDKVersion` 冻结；所有回调首参是登记时的 `context`，实例状态在 `context` 里）：

| 字段 | 语义 |
| --- | --- |
| `capabilities`（u32） | 能力位：`bit0` = `kBlockCapabilityFlushPrimitive`，对应共用接口的 `HasFlushPrimitive()`。未置位时 `Flush` 允许为空，`Block.Registry` 按「无缓存」处理、不调用；没有这个位，调用方无法区分「刷新成功」与「设备没缓存所以成功」 |
| `BlockBytes(context)`（u32） | 逻辑块大小；规则同 3.2（2 的幂、不小于 512） |
| `CapacityBytes(context)`（u64） | 容量，字节 |
| `Writable(context)`（u32） | 设备是否可写：0 只读、非 0 可写；M5 起挂载一律只读（M6 仍只读），写路径阶段 3 启用 |
| `Read(context, offset, buffer, bytes)` | 读 `[offset, offset + bytes)`；对齐 / 范围 / 指针非法返回 `InvalidArgument`，底层失败 `Io` |
| `Write(context, offset, buffer, bytes)` | 写；只读返回 `ReadOnly` |
| `Flush(context)` | 刷新写缓存；无缓存返回 `Ok`，设备错误 `Io` |

**登记入口与调用者（规格口径）。** **`Block.Registry.Publish` 的唯一调用者是 Vfs 模块内部的块设备层**：Vfs `Init` 时从 `Dev.Model` 的绑定事实（`Enumerate` / `Binding`）取出设备句柄、`BlockDeviceOps` 与 context，逐台登记进块设备注册表。**驱动不导入 `Block.Registry`、不调用 `Publish`、不依赖 Vfs**：驱动在 `Init` 里构造 `DriverDesc`（`ops` + `context` + `start` / `stop`）经 `Drv.Loader.RegisterDriver` / `Bind` 登记进 `Dev.Model`，由内核编排调用 `Drv.Loader.Start`。初始化序仍为 `DevModel → DriverLoader → 启动驱动 → Vfs → Ext4 / ISO9660 / FAT32File`（引导闭包预装顺序不变，内核在模块 `Init` 之间编排 Start）。三条硬约束：① 设备必须在 `Boot.Volume` 调用之前登记；② 同一设备句柄只登记一次；③ 任何导入 `Block.Registry` / `Fs.Vfs` 的模块都必须在该提供者 `Init` 之后初始化（依赖序保证）。

**登记校验（实现必须拒绝的情况）**：`Publish` 要求 `handle != 0`、`ops != nullptr`、`flags == 0`、同一句柄未登记；`BlockBytes` / `CapacityBytes` / `Writable` / `Read` / `Write` 必须非空，`Flush` 仅在 `capabilities.bit0` 置位时才要求非空；逻辑块大小必须合法（2 的幂且不小于 512）。任一不满足返回 `InvalidArgument`，表满返回 `NoSpace`。

**设备引用与挂载。** 挂载期间设备由 VFS 保持被引用：`Mount` 前 `Acquire`、`Unmount` 完成后 `Release`。文件系统模块的 `Mount` 收到 `const BlockDeviceOps*` 与设备 `context`（生成头 `FileSystemOps` 的第二、三参数），只借用这两个指针：不得在引用释放后使用，也不得释放 VFS 持有的引用；若实现自己另行 `Acquire`（例如要独立持有子设备视图），必须配对 `Release`。

**同一实例跨启动期与运行期**：Vfs `Init` 按 `Dev.Model` 绑定事实登记一次（此时驱动已完成 `Init` / `Bind`，其 `Start` 由内核编排）；系统卷挂载（阶段 1）与运行期模块装载（阶段 2）以及之后的运行期都使用同一句柄、同一驱动实例。阶段 3 启用中断 / 异步时只切换完成通知方式，**不重新绑定、不换实例、不重复登记**（见[启动介质与文件系统](启动介质与文件系统.md)第五节）。

**错误**（注册槽与 ops 槽都返回 `Storage::Error`）：参数非法 / 句柄无效 / 未登记 / 重复登记 / 只读目标写 / 底层失败按 3.1 的语义取值；容量耗尽 `NoSpace`；不静默、不重试、不降级为空设备。

### 4.2 VFS（`Fs.Vfs`）

**边界：** 文件系统注册表、挂载表、统一路径解析、文件引用、目录遍历与读取；**不实现任何卷格式、不做页缓存（本阶段）、不睡眠、不异步**。

**跨模块事实以生成头为准。** `Fs.Vfs` 的 8 个槽由生成头 `Slots.hpp` 定义：`RegisterFileSystem` / `Mount` / `Unmount` / `Stat` / `Open` / `Read` / `Close` / `ReadDirectory`，全部返回 `Storage::Error`；`FileSystemId` 是 u32（0 无效），文件引用类型 `FileReference` 是**不透明的 u64，不是对象句柄表项**。

**`Fs.Vfs` 八槽与 `FileSystemOps` 九字段的对应关系**（`FileSystemOps` 见 3.4 与生成头 `Types.hpp`）：

| `Fs.Vfs` 槽 | 对 `FileSystemOps` 的调用 | 语义 |
| --- | --- | --- |
| `RegisterFileSystem(ops, context, &fsId)` | 无（登记 ops 与 context，校验 9 个字段非空、格式名非空且不重复） | 返回 `FileSystemId`；登记表固定容量，耗尽 `NoSpace` |
| `Mount(fsId, deviceHandle, mountPoint, mode)` | 先 `Acquire(deviceHandle)`，再 `Mount(context, deviceOps, deviceContext, mode)` | 成功记录挂载项并保持设备引用；失败 `Release` 并清理残留 |
| `Unmount(mountPoint)` | `Unmount(context)` 后 `Release(deviceHandle)` | 有未关闭引用时拒绝（`InvalidArgument`）并保持挂载 |
| `Stat(path, &info)` | 路径解析 → `Stat(context, 卷内路径, info)` | 不做卷格式解析 |
| `Open(path, &ref)` | 路径解析 → `Open(context, 卷内路径, objectId)` → 分配引用表项 | 返回不透明引用；表满 `NoSpace` |
| `Read(ref, offset, buffer, bytes, &read)` | 查引用 → `Read(context, objectId, offset, buffer, bytes, &read)` | 读取边界见 3.4 |
| `Close(ref)` | 无（释放引用表项） | 无效引用拒绝 |
| `ReadDirectory(ref, &start, ...)` | 查引用 → `ReadDirectory(context, objectId, ...)` | 游标式遍历，规则见 3.4 |

**文件系统注册**：文件系统模块在自己的 `Init` 里登记 `FileSystemOps`；登记时检查 9 个字段非空（`Format` / `Mount` / `Unmount` / `Caps` / `VolumeIdentity` / `Stat` / `Open` / `Read` / `ReadDirectory`），缺项拒绝登记。格式名只用于诊断与重名拒绝，**挂载选择用 `FileSystemId`，不按名字匹配**。登记表固定容量、本阶段不注销（模块不卸载）。

**挂载点与最长匹配**：

- 挂载点是统一路径的绝对路径（3.5）；`Mount` 接收完整挂载点字符串，要求规范形式（不带尾斜杠、不以 `/` 结尾，根 `/` 除外），否则 `InvalidArgument`。
- 路径解析按**分量边界的最长前缀**匹配挂载点：例如挂载表有 `/` 与 `/mnt/data` 时，`/mnt/data/x` 命中 `/mnt/data`，`/mnt/database` 只命中 `/`（不按字节前缀误配）；同长度并列不可能，因为挂载点唯一。
- 命中后把剩余部分拼成**卷内绝对路径**交给 `FileSystemOps`（去掉挂载点前缀、补 `/`；命中 `/` 时原路径即卷内路径）。没有命中返回 `NotFound`。
- 同一挂载点只能挂载一次（重复 `InvalidArgument`）；同一设备本阶段只允许挂载一次；挂载表固定容量，耗尽 `NoSpace`。
- M6 的实际使用：`Boot.Volume` 把系统卷挂到 `/`（系统卷是内核与用户空间的根，见[系统卷布局](系统卷布局.md)），运行期模块装载经 VFS 读 `.osm` 字节；**公开的 `Fs.Vfs.Mount` 槽 v1 只接受 `/` 与只读模式**，其它挂载点返回 `NotSupported`。模块内部另有通用挂载入口（`MountAt`）供系统卷挂载与宿主夹具验证最长匹配，它不是第三套公开语义；其它挂载点与 ISO9660 / FAT32 卷的实际挂载消费者留给后续阶段（七节）。
- `mode` 本阶段只接受只读；读写挂载返回 `NotSupported`。
- **挂载时的格式选择**：公开 `Mount` 由调用方给出 `FileSystemId`；`Boot.Volume` 当前只按格式名 `ext4` 取唯一实现（模块内部 `FindFileSystemByFormat`），**不遍历全部登记项**。ISO9660 / FAT32File 在 M6 已登记（4.6），但不参与系统卷挂载。`Mount` 兼任格式探测：选定的实现 `Mount` 失败即说明不是该格式，调用方按需换下一个候选，VFS 不替调用方重试、不猜测格式。
- 挂载成功后 VFS 保持设备引用（`Acquire`）；`Unmount` 成功后 `Release`。存在未关闭文件引用时 `Unmount` 拒绝并保持挂载。

**文件引用**：`FileReference` 是 VFS 自有的不透明 u64、**不是对象句柄**；值 0 无效，引用表项绑定「挂载项 + 文件系统 context + 对象标识」。引用在 `Close` 前有效；`Close(0)`、已关闭或未知引用返回 `InvalidArgument`；引用表固定容量，耗尽 `NoSpace`。引用不跨卷；本阶段没有「按引用反查路径」的操作，也不把引用导出给其它模块。

**固定容量表的边界**（设备登记表、子设备表、文件系统登记表、挂载表、文件引用表）：

- 容量是 Vfs 模块的构建期常量：设备 32（根 + 子设备）、文件系统 8、挂载 4、文件引用 32（定义在 `VfsCore.hpp`）；**不动态扩容**。
- 表满时返回 `Storage::Error::NoSpace`（按生成头映射给模块面调用方），**不得覆盖、搬迁或静默丢弃**在用条目。
- 条目只在 `Release` / `Unmount` / `Close` 后归还；失败路径必须归还已占用的条目（注册与挂载都可撤销）。
- 已释放的编号可以复用，但调用方不得再持有已释放的句柄或引用；子设备表项在 M5 不回收（4.1）。

**操作语义**（`Fs.Vfs` 槽返回 `Storage::Error`）：

| 操作 | 成功 | 失败 |
| --- | --- | --- |
| `Stat(path, &info)` | 返回对象事实（目录 `bytes` 为 0、`directory` 为真） | 语法非法 / 超限 / 无挂载点命中 / 不存在 / 中间项非目录，按 3.1 的语义取值 |
| `Open(path, &ref)` | 返回引用；路径指向目录也成功，由调用方按 `Stat` 判定 | 同上；引用表满 |
| `Read(ref, offset, buffer, bytes, &read)` | 至多读到文件尾，`read` 为实际字节数；`offset` 任意、跨文件尾按 3.4 的读取边界 | 引用无效；目录读取；`bytes` 为 0 或缓冲为空 |
| `Close(ref)` | 释放引用 | 无效或已关闭引用 |
| `ReadDirectory(ref, &start, name, nameBytes, &nameLength, &info)` | 交出下一项（不含 `.` 与 `..`），`start` 由调用方从 0 开始并原样传回；结束返回 `NotFound` | 引用无效 / 非目录；名字缓冲放不下（不推进游标） |

**目录遍历是 v1 面的一部分**：M5 验证要求「挂载系统卷并列出根目录」，内核核心只导入 `Fs.Vfs`，因此契约层 10.2 在 v1 里补入 `ReadDirectory` 槽（槽 7）；只靠 `FileSystemOps.ReadDirectory` 不够——那只是模块内部实现，跨模块的列出动作必须经 VFS。

**路径解析边界（穷举拒绝项）：**

- 只接受绝对路径（`/` 起头）；相对路径、空串、空指针拒绝。
- 分量非空、不含 `.` 与 `..`、不以 `/` 结尾（根路径除外）；整条路径上限 1024 字节、单个分量上限 255 字节，超限报 `NameTooLong`。
- 不做大小写折叠（名字命中由卷实现按 `nameCase` 负责）、不做字符集转换与换行翻译、不解析符号链接、不接受反斜杠分隔符。
- **只按最长挂载点前缀落到一个卷**：不在卷之间拼接或回退；本阶段实际只挂 `/`，没有跨卷路径。
- **不猜**：路径按语法与实现规则判定，找不到就是 `NotFound`；不按前缀模糊匹配、不按大小写退让、不按「最后一个匹配」替代。

**清单相对路径的拼接**：阶段 2 装载运行期模块时，把清单的相对路径拼成 `<集根>/<相对路径>` 再交给 VFS（3.5）；拼接后超限按 `Corrupt` 拒绝。

**M6 读路径边界**：本阶段 VFS 仍只提供只读路径——不接受读写挂载，没有写 / 创建 / 删除 / 改名 / 截断操作，不做日志回放，不做页缓存；所有调用同步、有界、单线程（内核入口与闭包 `Init` 顺序执行）。M6 新增的是**格式实现与消费者**：ISO9660 / FAT32File 真实只读解析已登记进 `Fs.Vfs`（4.6），运行期模块装载经 VFS 读 `.osm`（`Stat` / `Open` / `Read` / `Close`）；内核当前没有把 ISO9660 / FAT32 挂到非 `/` 挂载点的消费者。写入、日志回放、异步与工作队列属阶段 3 以后（七节）。

**调用时机**：Vfs 模块 `Init` 建立注册表、挂载表与引用表（可撤销），并作为唯一 `Publish` 调用者按 `Dev.Model` 绑定事实登记块设备（4.1）；它不挂任何卷。文件系统由各自模块 `Init` 登记（M6 为 Ext4 / ISO9660 / FAT32File，4.6）；**闭包全部 `Init` 成功后**，内核核心调用 `Boot.Volume.MountSystemVolume`（4.5），成功后系统卷挂在 `/`，阶段 2 的运行期模块才可以通过 VFS 读卷；在此之前装载器不得经 VFS 读盘。

**并发与缓存**：本阶段不加锁、不睡眠、不做页缓存、不做预估读；每次 `Read` 直达文件系统。运行期模块装载与后续读取复用同一挂载与同一设备实例。

### 4.3 卷标识查询归属

**结论：卷标识由文件系统实现提供，块设备层只透传候选设备；与 `BootInfo.volumeUuid` 的核对责任在 VFS（`Boot.Volume`），内核核心只负责调用与失败编号。**

| 角色 | 做什么 | 不做什么 |
| --- | --- | --- |
| 文件系统模块（`Ext4.osm` 等） | 经 `FileSystemOps.VolumeIdentity`（槽 4）写 16 字节卷标识：Ext4 取 superblock 的 `s_uuid`；ISO9660 取 PVD Volume Identifier 前 16 字节补零；FAT32 取 BPB Volume ID（4 字节）+ Volume Label（11 字节）补零（4.6） | 不比对、不解释「这是不是系统卷」 |
| 块设备层（`Block.Registry`） | 给出设备句柄与读写原语 | 不解析 superblock、不提供「卷标识」概念、不认识卷格式 |
| VFS（`Fs.Vfs` / 本模块） | 挂载后向已挂载的文件系统取标识，交给同一模块的 `Boot.Volume` 比对 | 不解释标识字节、不维护第二份标识 |
| `Boot.Volume` | 与 `BootInfo.volumeUuid` 逐字节核对，决定挂载哪台设备（唯一候选，见 4.5） | 不解析卷格式，不自行读 superblock |
| 内核核心 | 闭包全部 `Init` 后调用 `MountSystemVolume`；按返回码选 `E-KRN` 编号并打印标记 | 不直接读卷标识、不参与候选选择 |
| Baleen 侧 | 同一归属：Ext4 只读实现给出 `s_uuid`（`Ext4FileSystem::Uuid()`），其块设备适配器不解释卷格式 | 不把卷标识塞进块设备层 |

槽位与签名以生成头为准（`FileSystemOps` 槽 4 `VolumeIdentity(context, uint8_t*)` 写 16 字节），**取值语义由本文定**；块设备的 ops 表不出现任何卷标识字段。

语义：16 字节**原字节**，与布局描述 `VolumeUuid` 的表示一致，不做字节序重排、不做十六进制字符串比较；挂载成功后有效；未挂载 / 已卸载 / 调用顺序错误返回 `InvalidArgument`；格式本身没有卷标识概念时返回 `NotSupported`。**ISO9660 与 FAT32 的标识口径已在 M6 定稿**（4.6）：两者都给出 16 字节标识，但**都不参与系统卷身份核对**——`Boot.Volume` 只按 Ext4 的 `s_uuid` 核对，这两个标识只用于诊断与归属。**全零不是有效身份**：`BootInfo.volumeUuid` 全零表示引导器未提供（见 [Baleen 引导器](../Baleen/README.md) 6.1）；文件系统返回全零也按「无标识」处理，`Boot.Volume` 不得把它当成匹配。

**为什么不在块设备层**：块设备层不认识卷格式。把 superblock 解析挪进块设备层，会让驱动依赖某种文件系统的字节布局，也会让「同一设备跨启动期与运行期」的设备实例承担与设备无关的卷语义。卷标识的唯一事实源是卷内结构，`BootInfo` 里的值只是引导器转抄的线索（4.5）。

### 4.4 错误码映射与诊断

**存储面只有一套错误码，块设备层 / 文件系统 / VFS 的失败按同一套语义返回：**

| 错误码 | 块设备层何时返回 | 文件系统 / VFS 何时返回 |
| --- | --- | --- |
| `InvalidArgument` | 偏移 / 长度未按逻辑块对齐、零长度、空指针、区间越界、句柄为 0、重复登记、子视图起点未对齐、无配对的 `Release` | 路径语法非法或挂载点非规范、`bytes` 为 0、无效文件引用、未挂载状态、重复挂载、写只读挂载、`VolumeIdentity` 调用顺序错误 |
| `NotFound` | 未登记句柄 | 路径或对象不存在、目录遍历结束、**没有唯一系统卷候选**（4.5） |
| `NotDirectory` | — | 路径中间项不是目录 |
| `IsDirectory` | — | 对目录做文件读取 |
| `NotSupported` | 实现未覆盖的读 | 未知或拒绝的卷特性、读写挂载、格式无卷标识能力 |
| `Corrupt` | — | 卷结构非法、字段自相矛盾、清单相对路径拼出的绝对路径超限 |
| `Io` | 底层读 / 写 / 刷新失败 | 由设备失败透传或汇总 |
| `ReadOnly` | 对只读设备写 | 写只读挂载 |
| `NameTooLong` | — | 整条路径或单个分量超限、目录项名字缓冲放不下 |
| `NoSpace` | 设备 / 子设备表满 | 文件系统登记表 / 挂载表 / 文件引用表容量耗尽 |

**错误码分面**：分面与 `KernelErrorFromStorage` 映射见 3.1。`Block.Registry` / `Fs.Vfs` / `Boot.Volume` 的槽都返回 `Storage::Error`，不返回 `KernelError`；内核核心直接消费 `Storage::Error` 并在自己的调用点选 `E-KRN` 编号，不需要再走映射表（映射表是给模块入口 / `Core.*` 面的）。模块内部不得定义第三套跨边界错误码，也不得就地改写既有取值语义。

**诊断**：存储层与 `.osm` 模块**不写 `E-KRN` 行、不自行等待或复位**；`Boot.Volume` 失败时静默返回 `Storage::Error`（成功时打印 `system volume mounted:` OK 行，失败不打印该行）。内核核心在调用点判定失败后按返回码与阶段选号：M5 已分配 `E-KRN-70`–`76`（70 文件系统 / 卷接口不可用、71 挂载失败、72 身份不符、73 清单路径不可用、74 打开失败、75 读取失败、76 摘要不符）并沿用至 M6；M6 追加 `E-KRN-77`（光盘档 ISO9660 第二来源与 `BootInfo` 不一致，FATAL）与 `E-KRN-78`（第二来源不可用，WARN），另由内核核心在运行期模块装载段选用 `E-KRN-80`–`83`（见[模块修订与更新](../模块修订与更新.md)第五节），都不在 VFS 槽内部选号。所有 FATAL 报一次 `FATAL` + 屏幕 + 等待 + 复位；detail 带阶段、返回码的 `ErrorMessage` 文本与候选数 / 卷标识摘要。被跳过的单个候选只输出普通诊断行（不带编号），不逐候选刷编号。编号的具体文本以[内核诊断编号](../内核诊断编号.md)与 `Diagnostics.hpp` 登记为准。

### 4.5 `Boot.Volume`：系统卷核对与挂载

**跨模块事实**：`Boot.Volume` v1 只有一个槽 `MountSystemVolume(const Boot::Info* info, SystemVolumeFacts* facts)`，返回 `Storage::Error`（生成头 `Slots.hpp`）。输入是引导器交来的只读 `BootInfo`，输出是 `SystemVolumeFacts`；不接收挂载点参数——系统卷固定挂到 `/`（[系统卷布局](系统卷布局.md)）。

**输入事实（逐项用途）：**

| 输入 | 当前用途 |
| --- | --- |
| `volumeUuid[16]` | 身份核对：与卷内 `VolumeIdentity` 的 16 字节逐字节比较；全零 = 引导器未提供 → 无法核对，失败 |
| `volumeOffset` / `volumeBytes` | 系统卷在启动介质上的字节区间：用它在候选设备上开子设备视图；两个都 0 或只给一个 → 无法定位，失败 |
| `mediumBytes` | 容量上限：非 0 时要求 `volumeOffset + volumeBytes <= mediumBytes`；0 = 引导器不知道，跳过 |
| `sectorBytes` | 只作诊断 / 交叉提示，**不作对齐判据**（对齐用驱动报告的逻辑块大小） |
| `deviceKind` / `devicePath` / `devicePathBytes` | 本阶段只记录到诊断。M6 的 `Boot.Volume` 与块设备注册表之间仍没有「设备事实 → 启动线索」的映射（那需要 `Dev.Model` 绑定事实，接线见「暂缓项」），因此候选集合是**全部已登记块设备**；线索不得用于「按顺序取第一个」 |
| `kernelSetBuildId[32]` | 原样转抄进 `facts.buildId`；全零 = 未提供 |
| `kernelPath` / `layoutOffset` | 只作诊断与阶段 2 用途：M6 仍不重读布局描述（见「暂缓项」），也不在挂载阶段校验路径形态（选版阶段已核对）；`kernelPath` 用于内核核心推导集根、读清单与装载运行期模块 |
| 候选设备与 ops | 本模块块设备注册表的内部登记（4.1），不需要跨模块导入 |
| 文件系统注册表 | 本模块 `Fs.Vfs` 的内部登记表（4.2）；M6 登记 Ext4 / ISO9660 / FAT32File 三项，系统卷挂载只按 `ext4` 取实现（4.6） |

**规则：**

1. **线索只缩小范围，身份由卷标识核对。** M6 的「缩小范围」仍未接线，候选取全部已登记块设备——这不违反「不猜」：每台设备都必须通过卷标识验证，只是不跳过验证直接选设备。
2. **候选设备是整介质的块设备。** `volumeOffset` 是介质绝对字节偏移，系统卷用偏移视图切出；`Boot.Volume` 不解析分区表、不把分区当独立候选（分区与 ISO 内镜像都由偏移视图表达）。
3. **对齐判据用驱动报告的逻辑块大小**（3.2 / 3.3）；`BootInfo.sectorBytes` 不一致只报诊断，不作判据。
4. **`Boot.Volume` 不解析卷格式。** 挂载与卷标识来自文件系统模块的 `FileSystemOps`；它只负责候选遍历、身份比对与唯一性判定。
5. **唯一候选才成功。** 恰好一台设备通过「可定位 + 可只读挂载 + 卷标识相等」才算定位成功；零台或多台都失败（`NotFound`），不按设备顺序挑第一个、不在多台之间取舍。
6. **失败不降级。** 返回失败后由内核核心报 `FATAL` + 屏幕 + 等待 + 复位，不带着未挂载继续，也不「先启动再从别的卷补」。

**判定顺序（逐候选探测、最后只挂一个）：**

1. 取块设备注册表里的全部已登记**根设备**句柄（子设备不是候选）；为空 → `NotFound`（detail `no block device registered`）。
2. 逐候选（顺序无关，结论必须与顺序无关）：
   1. 取 ops 与设备事实；逻辑块大小或容量事实非法 → 跳过（输出普通诊断行，不带编号）。
   2. 位置核对：`volumeOffset`、`volumeBytes` 都非 0；`volumeOffset` 按设备逻辑块对齐；`volumeOffset < capacity` 且 `volumeBytes <= capacity - volumeOffset`（减法判定）；`mediumBytes` 非 0 时同样检查介质容量上限。任一项不满足 → 跳过。
   3. 在候选设备上开系统卷子设备视图（`begin = volumeOffset`、`bytes = volumeBytes`）；失败 → 跳过。
   4. 只读挂载到 `/`（M6 的 `Boot.Volume` 只按格式名 `ext4` 取实现，ISO9660 / FAT32 虽已登记但不参与系统卷挂载；VFS 内部用通用挂载入口，公开 `Mount` 槽 v1 也只接受 `/`）。格式不匹配或卷不可接受 → 清理残留、跳过该候选。
   5. 取卷标识（`FileSystemOps.VolumeIdentity`，经本模块 VFS 内部接口）；失败或全零 → 卸载、跳过。
   6. 与 `BootInfo.volumeUuid` 逐字节比较：不等 → 卸载、跳过；相等 → 记为一个匹配项并**立即卸载**（扫描期间不保留 `/` 挂载，避免占用挂载点；也不允许多个候选同时挂在 `/`）。子设备表项在 M6 仍不回收（4.1），容量上界按「根设备数 + 探测视图数」预留。
3. 扫描结束：
   - 恰好 1 个匹配 → **复用探测时创建的子设备句柄**重新只读挂载到 `/`，复核卷标识，写入 `facts`，输出成功标记并返回 `Ok`。
   - 0 个匹配 → `NotFound`，detail `no system volume candidate`。
   - 多个匹配 → `NotFound`，detail `multiple system volume candidates: <n>`（身份事实自相矛盾，不挑一个）。
4. 失败时 `facts` 保持模块清零后的状态（调用方不得读）；成功时填 `bytes` = `sizeof(SystemVolumeFacts)`、`volumeUuid` 与 `buildId`。

**`SystemVolumeFacts` v1 字段（生成头 `Types.hpp`，52 字节、首字段长度、尾部可追加）：**

| 字段 | 语义 |
| --- | --- |
| `bytes` | 结构长度，版本协商字段：模块入口先清零整个结构，成功时写 `sizeof(SystemVolumeFacts)`；调用方**不需要**预填，按模块写入的长度有界读取 |
| `volumeUuid[16]` | 已核对的卷标识，原字节，与 `BootInfo.volumeUuid` 逐字节相等 |
| `buildId[32]` | 原样转抄自 `BootInfo.kernelSetBuildId`；全零表示未提供，只作诊断与归属 |

**不把设备句柄 / 挂载名写进 v1 facts**：v1 只承诺这三项；设备句柄与挂载名由 Vfs 内部记录（挂载项里已是设备句柄 + 挂载点），外部消费方不需要。若后续确要暴露，按可扩展结构「首字段长度 + 尾部追加」发新版本，不改既有偏移。

**成功标记（由 `Boot.Volume` 自己打印）**：唯一候选重新挂载并复核通过后，模块用 `Core.Log` 的 OK 等级打印一行 `[ OK ] system volume mounted: uuid=<32 位小写十六进制>`（`uuid` 取 `facts.volumeUuid` 的 32 个十六进制字符；**当前不输出 `bytes=` 字段**，T5.8 断言按 `system volume mounted:` 子串匹配）。模块仍不写 `E-KRN` 行；失败时静默返回错误，由内核核心汇总报 FATAL。

**失败返回码（模块面）**：`NotFound`——BootInfo 卷标识全零 / 区段事实不可用 / 全部候选不命中 / 命中多个；`InvalidArgument`——模块未初始化、`facts` 为空、`BootInfo` 为空；`Io`——唯一候选重新挂载失败；卷标识查询的错误原样透传。内核核心按返回码与阶段选 `E-KRN-70`–`76`（T5.9 已登记：接口不可用 / 挂载失败 / 身份不符 / 清单路径 / 打开 / 读取 / 摘要不符）。

**内核调用时机**：闭包 13 个模块全部 `Init` 成功之后、运行期模块装载之前，由内核核心调用。调用前必须满足：块设备已由 Vfs `Init` 按 `Dev.Model` 绑定事实登记（4.1）、Ext4 已 `RegisterFileSystem`（4.2）、Vfs 注册表已建立；调用成功后系统卷挂在 `/`，阶段 2 才可以通过 VFS 读卷。挂载失败按 4.4 报 `FATAL`，不继续装载运行期模块。

**挂载之后的清单读取（T5.8 链路，VFS 只提供操作）**：内核核心按 `BootInfo.kernelPath` 与 `kernelSetBuildId` 推出集根（[内核架构与自举闭包](../内核架构与自举闭包.md) 2.3），拼出集内 `Manifest.mft` 的绝对路径，经 `Fs.Vfs.Open` → `Read`（前 32 字节）→ `Close` 读取并打印摘要标记，随后在光盘档做 ISO9660 第二来源核对（`E-KRN-77` / `E-KRN-78`）并装载运行期模块。读取链路与标记格式见[启动介质与文件系统](启动介质与文件系统.md)第三节。`Boot.Volume` 本身不读清单（集根推导与清单读取不属于「挂哪个卷」）。

**暂缓项（M6 仍未实现，登记在七节未定表）**：

- **布局描述重读**：[Baleen 引导器](../Baleen/README.md) 6.1 描述的是「内核按 `DeviceKind` 找到候选后在 `LayoutOffset` 处读布局描述交叉核对」。M6 仍不重读布局描述：身份锚是卷内 `s_uuid` 与 `BootInfo` 转抄事实；布局描述重读作为加强项（第二来源）归 M6 收尾 / M7，不阻塞系统卷主路径。
- **线索缩小候选**：`DeviceKind` → 设备句柄的映射需要 `Dev.Model` 绑定事实，M6 也未接入；候选范围仍为全量，接口与唯一性判定不变，归 M6 收尾 / M7。

**与内核入口 WARN 的关系**：`BootInfo` 启动来源字段为 0 或彼此不一致时，内核入口按 `E-KRN-20`–`23` 播报 `WARN` 不停机（入口判「字段自洽」）；挂载阶段对「身份不可核对」是硬失败（`FATAL`，判「身份可核对」）。两者不矛盾：入口不因缺字段停机，不等于允许在内核里猜系统卷——猜错设备会写坏别的盘。

**ISO 与光盘形态**：两种介质走同一条路径（候选设备 + 布局区段 + 偏移视图），**不按 `media` 分支**。光盘上的系统卷是 ISO9660 卷内连续存放的 Ext4 镜像（ISO 内文件名固定为 `/SystemVolume.img`，见[启动介质与文件系统](启动介质与文件系统.md)第二节），布局描述给出该区段的起点与长度；单区段连续性由镜像组装器保证，读取方只核对边界与对齐。M6 的 ISO9660 模块能按名字定位并读取 `/SystemVolume.img`（名字折叠规则见 4.6）；**第二来源核对已在 M6 由内核核心实现**（光盘档最小解析：主卷描述符 `CD001` + 根目录项 extent LBA × 2048 得到区段，与 `BootInfo` 比对；一致打印 `iso9660: system volume file=` 稳定行，不一致 `E-KRN-77` 停机、不可用只 `WARN E-KRN-78`）。这一步发生在挂载成功之后，不改变“ISO9660 不成为系统卷挂载格式”的结论；主路径仍只有偏移视图。对齐要求见[启动介质与文件系统](启动介质与文件系统.md)第四节：区段起点必须按介质逻辑块大小对齐，4Kn 上为 4096。

**与 Baleen 的关系**：与 [Baleen 引导器](../Baleen/README.md)第五节、6.1 是同一条规则——线索缩小范围、卷标识核对、无法唯一定位就报告失败。内核拿到的是引导器转抄的事实，必须自己再核对一次；不因为「引导器已经核对过」而跳过。

### 4.6 已落地的文件系统实现（M6）

**Ext4、ISO9660、FAT32File 三个只读实现都在 M6 落地并登记进 `Fs.Vfs`；系统卷仍只按 Ext4 挂载，另两个格式的标识与能力槽只用于诊断、归属与后续阶段的挂载消费者。**

| 项 | Ext4（M5 落地） | ISO9660（M6） | FAT32File（M6） |
| --- | --- | --- | --- |
| `Format()` | `ext4` | `iso9660` | `fat32` |
| `Caps()` | writable=false、directoryListing=true、symlinks=false、nameCase=Sensitive | writable=false、directoryListing=true、symlinks=false，nameCase=Insensitive | writable=false、directoryListing=true、symlinks=false，nameCase=Insensitive |
| `VolumeIdentity` | superblock `s_uuid`（16 字节原字节） | PVD Volume Identifier（32 字节空格填充文本）去尾部空格后取前 16 字节、其余补零；未挂载 `InvalidArgument`，标识为空 `NotSupported` | BPB Volume ID（偏移 `0x43`，4 字节原字节）+ Volume Label（偏移 `0x47`，11 字节去尾部空格）按 4+11 写入、其余补零；未挂载 `InvalidArgument`，序号全零且卷标空 `NotSupported` |
| 参与系统卷身份核对 | **是**（唯一身份锚） | 否 | 否 |
| `Mount` 限制 | 只读；按 `kBaleenAccepted` 判 INCOMPAT；卷状态核对拒绝 `RECOVER` / 未干净卸载 | 只读；逻辑块必须是 2048，否则 `NotSupported`；读写挂载 `NotSupported` | 只读；只接受 FAT32 几何（`rootEntries` 为 0、`fatSize16` 为 0、`FsType` 为 `FAT32`、簇不超过 4 KiB），否则 `NotSupported`；读写挂载 `NotSupported` |
| 名字比较 | 逐字节（`Sensitive`） | Rock Ridge NM 名与 8.3 标识符两套都按 ASCII 折叠；比较前去掉 `;N` 版本后缀；`/SystemVolume.img` 与 `/SYSTEMVO.IMG;1` 命中同一项 | LFN 名与 8.3 短名两套都按 ASCII 折叠；LFN 含非 ASCII 码元或校验和不符时退回 8.3；`/LongNameFile.bin` 与 `/LONGNA~1.BIN` 命中同一项 |
| 目录遍历 | 交出目录项（不含 `.` 与 `..`）；游标由调用方从 0 起、原样传回 | 交出 Rock Ridge 显示名（无 NM 时交出 ISO 标识符去版本形式）；`.` 与 `..` 不交出；遍历有块上界 | 交出 LFN 显示名（无合法 LFN 时交出 8.3 名）；`.` 与 `..` 不交出 |
| 明确拒绝 | 未知 `INCOMPAT` 位；`RECOVER` / 未干净卸载的卷（`NotSupported` / 卷状态失败） | Joliet、多区段文件（目录记录 flags bit7）、SL 符号链接跟随与关联 / 保护属性、扩展属性记录；双端序不一致或 `CD001` 不符按 `Corrupt` | FAT12/16 形态、非 FAT32 卷（`NotSupported`）；坏簇标记、自环、越界链、短链按 `Corrupt` |

- **登记与诊断**：三个模块都在自己的 `Init` 里向 `Fs.Vfs` 登记 `FileSystemOps`；登记成功不打印带格式前缀的日志行（成功由装载器统一打印 `module init ok:`），失败只打印不带编号的普通日志（如 `[ ISO9660 ] the Fs.Vfs import isn't bound`、`[ FAT32File ] registering the fat32 file system failed`）。宿主夹具 `CheckIso9660`（21 例）与 `CheckFat32`（20 例）覆盖上表的断言。
- **身份核对的边界**：`Boot.Volume` 只按格式名 `ext4` 取文件系统、只比较 Ext4 的 `s_uuid`；ISO9660 / FAT32 的 `VolumeIdentity` 虽已定稿，**不参与** `BootInfo.volumeUuid` 的核对，也不作为系统卷候选判据（4.3 / 4.5）。
- **M6 没有的消费者**：除系统卷外，内核还没有把 ISO9660 / FAT32 挂到任何挂载点（公开 `Mount` 槽 v1 只接受 `/`，且由 `Boot.Volume` 以 Ext4 为格式）。光盘上的 `/SystemVolume.img` 有两处只读能力：ISO9660 模块可按名字定位读取（经 VFS 挂载后才可用），内核核心在光盘档另做一份最小解析的第二来源核对（4.5）；两者都不把 ISO9660 变成系统卷挂载格式。
- **运行期模块读取**：运行期 `.osm` 位于系统卷（Ext4）上，经 VFS `Stat` / `Open` / `Read` / `Close` 读出，不经过 ISO9660 / FAT32；装载口径见[模块修订与更新](../模块修订与更新.md)第五节。

## 五、阶段划分与验证

| 阶段 | 交付物 | 验证方式 |
| --- | --- | --- |
| S1 接口与共用件（已落地） | `Packages/Common/Include/Storage/` 的接口头与 `Src/Storage/` 的实现：`Path`、`BlockDevice`、`SubDevice`、`FileSystem` 与 `ErrorMessage`；`Packages/Common/Makefile` 收录对象 | `Tools/Bin/CheckStorage`：统一路径与名字比较、假块设备上的偏移视图边界与对齐、刷新原语能力、错误文本；同一份源码在 32 位与 64 位两套选项下编译并链接通过，32 位侧不牵入 64 位除法辅助函数 |
| S2 Ext4 只读（Baleen 侧，已落地） | 目标侧最小只读实现：superblock、特性清单、块组描述符、inode、extent、目录项、按路径读文件；BaleenCore 的块设备适配器；卷标识 `Uuid()` | `Tools/Bin/CheckExt4`：真实 Ext4 卷夹具上的按路径读取与逐字节比对；故障注入：坏 superblock、未知与拒绝的 `INCOMPAT`、越界块号、错误卷偏移、读盘失败；块设备适配器的窗口中转与对齐约束 |
| S3 BaleenCore 接入（已落地） | 按 `BaleenLayout.bin` 的路径读内核镜像并校验载荷头，卷标识核对 | 与 S2 同一套镜像夹具；QEMU / Bochs 启动与故障注入 |
| S4 内核侧（M5 落地、M6 补齐） | `Vfs.osm`：`Block.Registry` / `Fs.Vfs` / `Boot.Volume`（4.1–4.5）；`Ext4.osm` 只读（3.6 的集合拆分）；M6：ISO9660 / FAT32File 只读实现与卷标识（4.6）、NVMe / USB 真实驱动接入、运行期 `.osm` 装载与同集两修订选择/回退；阶段 3：写入、回放、异步 | 宿主：`CheckStorage` / `CheckExt4` 语义复用 + Vfs 宿主夹具（假 `BlockDeviceOps` / `FileSystemOps`、候选序列、错误码断言、挂载点最长匹配、唯一候选与歧义拒绝）+ `CheckRevision`（M5 起）+ M6 新增 `CheckNvme` / `CheckUsbCore` / `CheckXhci` / `CheckUsbHub` / `CheckUsbStorage` / `CheckIso9660` / `CheckFat32`；启动：M6 主线五档（BIOS / UEFI × HDD / ISO + UEFI NVMe 4Kn）串口依次出现 `boot closure preloaded: count=13`、13 条 `module init ok:`、`system volume mounted: uuid=…`、`system manifest read: bytes=… digest=…`（光盘两档另要求 `iso9660: system volume file=…`）、`runtime modules: loaded=0 failed=0 optional=0`；运行期夹具三档（必需失败 `E-KRN-81` + 复位循环 / 可选失败 `WARN E-KRN-83` 不重启 / 可选成功 `runtime module init ok`）与两修订三档（`Rev=2` / `Rev=1` / 回退 `Rev=1`）；失败注入：无候选 / 卷标识不符 / 对齐不符 / 卷特性拒绝 / 只读介质，编号落在 `E-KRN-70`–`76` |

**S4 的接口面在 M5 收敛，M6 只补实现面**：块设备 ops 与子设备、句柄与回收、VFS 八槽与最长匹配、文件引用、路径解析边界、卷标识归属、`Boot.Volume` 的唯一候选判定、错误码分面、Ext4 特性集合拆分，全部在 M5 固定；M6 的 ISO9660 / FAT32File 增加格式实现、卷标识口径与只读边界（4.6），NVMe / USB 驱动接入 `Dev.Model → Vfs` 链路，运行期模块经 VFS 读卷（第五节），这些都不改 M5 已定的接口语义（八槽、`BlockDeviceOps` 56 字节、`FileSystemOps` 72 字节、唯一候选判定）。第二来源核对的**内核接线**未落地，不改变本节验证口径。

**S1 不依赖 BaleenCore 的其余部分。** BaleenCore 的布局描述格式、选版与签名策略与本抽象解耦：文件系统只回答「按路径取文件」，不解释 `BaleenLayout.bin`，也不参与选版。

**宿主侧参考实现不承诺与目标侧共用。** S2 的目标侧实现按引导环境约束写；如果组装器或验证需要独立的参考解析，那是宿主侧的另一份实现（与 MakeImage / TestInstaller 的现有做法一致），两者靠同一套镜像夹具与摘要比对交叉验证，而不是靠共享代码。

## 六、与现有规格的关系

- [启动介质与文件系统](启动介质与文件系统.md)：介质与访问链、必备文件系统、读写与阶段切换策略；本文只定义接口层与内核侧挂载口径，不改写其策略；两份文档对系统卷挂载的判定顺序以本文 4.5 为准。
- [Baleen 引导器](../Baleen/README.md)：卷定位流程与最小只读实现的能力边界；本文的接口按那套边界设计，内核侧的定位规则与第五节、6.1 同源。
- [Baleen 布局描述](../Baleen/布局描述.md)：`VolumeUuid` / `VolumeOffset` / `VolumeBytes` / `KernelPath` 的字节来源与校验顺序；4.5 只消费这些转抄事实，M6 仍不重读布局描述（重读作为后续第二来源，见七节）。
- [模块修订与更新](../模块修订与更新.md)：运行期 `.osm` 的识别、装载时机与失败编号（`E-KRN-80`–`83`）；运行期模块的字节经 VFS 从系统卷读出，装载编排与编号不由 VFS 负责（4.2 / 第五节）。
- [系统卷布局](系统卷布局.md)：系统卷是内核与用户空间的根，挂载点固定 `/`；集根与 `BuildId` 目录规则由此而来。
- 生成头 `Packages/Common/Out/Contract/{Slots,Types}.hpp`：块设备 / 文件系统 ops、三个闭包单例槽位与 `SystemVolumeFacts` 的字段事实；本文 4.x 的接口形状与错误码分面以它为准。
- [内核架构与自举闭包](../内核架构与自举闭包.md)：VFS 与块设备层、Ext4 / ISO9660 / FAT32 的引导闭包归属；本文是它们的接口与内核侧口径。
- [内核启动与固件边界](../内核启动与固件边界.md)：`BootInfo` 字段核对、启动来源 `WARN` 与设备线索；4.5 与它的分工见该文第一节。
- [内核模块容器](../内核模块容器.md)：接口 ID / 槽位 / 版本、提供者唯一与装载步骤；本文不重复定义容器字节，只定 ops 语义。
- [模块契约层](../模块契约层.md)：接口 ID / 稳定名 / 槽位与版本 / `KernelAbiVersion` / `DriveSDKVersion` / 入口 ABI / `BlockDeviceOps` / `FileSystemOps` 布局与生成物；接口面与错误码分面以它为准，与本文的分工见 1.1。
- [模块与驱动装载](../模块与驱动装载.md)：模块 `Init` / `Activate` 契约、归属登记与失败处理；块设备与文件系统在 `Init` 中登记，`Activate` 属阶段 3。
- [内核诊断编号](../内核诊断编号.md)：`70–79` 号段归属文件系统 / VFS，M5 已在 `Diagnostics.hpp` 分配 `70`–`76`（接口不可用 / 挂载失败 / 身份不符 / 清单路径 / 打开 / 读取 / 摘要不符）；运行期模块装载的 `80`–`83` 在 M6 分配、由内核核心在 `LoadRuntimeModules` 选用，不在 VFS 槽内部选号，具体文本以该规格登记为准。
- [ABI 兼容与外部契约](ABI兼容与外部契约.md)：错误码与接口版本的兼容规则。
- [输出通道与Print](../输出通道与Print.md)：错误文本经 Print 输出时的标签约定；`FATAL` 行使用 `ErrorMessage` 的文本。

## 七、已定与未定

**已定项**（覆盖 M5 的 Vfs / Ext4 接口面与 M6 补入的格式实现、运行期读取面）：

| 主题 | 结论 |
| --- | --- |
| 块设备层 | 句柄 `DeviceHandle`（u32，0 无效）：真实设备句柄来自 `Dev.Model` 的绑定事实、不另编号，子设备句柄由 `SubDevice` 在同一命名空间的高位段分配；`BlockDeviceOps` 首字段 `capabilities`（bit0 = 刷新原语位）+ 六个回调（56 字节）；子设备按区间对齐、块大小随父、可嵌套；单提供者、不导出多实例接口；设备不注销、不热替换；子设备表项 M5 起不回收（`Release` 只减引用）；同一实例跨启动期与运行期（4.1） |
| VFS | v1 八槽 = 注册 / 挂载 / 卸载 / `Stat` / `Open` / `Read` / `Close` / 目录遍历；挂载点按**分量最长前缀**匹配、交给文件系统的是卷内绝对路径；文件引用是 VFS 自有不透明 `u64`（不是对象句柄）；固定容量表耗尽返回 `NoSpace`、不扩容不覆盖；只挂 `/`、只读、单线程、无缓存（4.2） |
| 卷标识 | Ext4 `s_uuid` 由文件系统模块的 `VolumeIdentity` 槽提供（16 字节原字节）；块设备层只透传候选；**核对责任在 Vfs 的 `Boot.Volume`**；未挂载 / 顺序错误返回 `InvalidArgument`，全零不是身份；ISO9660 / FAT32 的标识口径见 4.6，**都不参与系统卷核对**（4.3） |
| 系统卷挂载 | 候选 = 全部已登记根设备（M6 仍未按 `DeviceKind` 过滤，接线归 M6 收尾 / M7）；逐候选「位置核对 → 偏移视图 → 只读挂载 → 卷标识比对」并立即卸载，最后复用匹配候选的视图只挂一个；0 个或多个匹配 → `NotFound` + 固定英文 detail；facts = `bytes` + `volumeUuid` + `buildId`（模块清零后写入）；调用时机 = 闭包全部 `Init` 后；成功标记由 `Boot.Volume` 自己用 `Core.Log` 的 OK 行打印（4.5） |
| 文件系统实现（M6） | 三个只读实现都登记进 `Fs.Vfs`：Ext4（`ext4`，`Sensitive`）、ISO9660（`iso9660`，`Insensitive`，固定 2048 逻辑块，拒绝 Joliet / 多区段 / SL / 属性记录）、FAT32File（`fat32`，`Insensitive`，仅 FAT32 几何，拒绝坏链）；三者 Caps 均为 writable=false / directoryListing=true / symlinks=false；ISO9660 与 FAT32 的 `VolumeIdentity` 已定稿但不参与系统卷核对（4.6） |
| 特性集合 | 单点头文件里两个集合：`kBaleenAccepted` ⊆ `kKernelAccepted`（编译期断言）；`kKernelAccepted` 先并入 `RECOVER`、是阶段 3 的预留超集，**本阶段只读两侧都按 `kBaleenAccepted` 判定**；置 `RECOVER` / 未干净卸载的卷在 Mount 卷状态核对仍被拒绝（3.6） |
| 读取边界 | 至多读到文件尾、`read` 报实际字节数、`offset` 任意；零长度与目录读报明确错误（3.4） |
| 错误码分面 | 存储面（ops 与 `Block.Registry` / `Fs.Vfs` / `Boot.Volume`）只有 `Storage::Error`，模块面只有 `KernelError`，`KernelErrorFromStorage` 单点映射；模块不写 `E-KRN` 行、不改写既有取值；运行期模块装载的 `80`–`83` 由内核核心选号（3.1 / 4.4） |
| M6 读路径边界 | 只读：无写、无创建 / 删除 / 改名、无日志回放、无页缓存、无异步；M6 新增 ISO9660 / FAT32 只读实现与运行期模块装载读取，仍无写 / 回放 / 异步（4.2） |
| 运行期读取面 | 运行期 `.osm` 经 VFS `Stat` / `Open` / `Read` / `Close` 从系统卷（Ext4）读出；必须在系统卷挂载成功之后；VFS 只提供读取操作，运行期识别、上限、摘要核对与失败编号在[模块修订与更新](../模块修订与更新.md)第五节与内核核心 |
| ISO9660 第二来源（M6） | 光盘档在挂载与读清单之后，由内核核心以最小解析（PVD `CD001` + 根目录项）推出 `/SystemVolume.img` 区段并与 `BootInfo.volumeOffset` / `volumeBytes` 比对；稳定行 `iso9660: system volume file=…`，不一致 `E-KRN-77` 停机、不可用 `WARN E-KRN-78`；不是挂载前置（4.5） |
| 共用与分列 | 接口声明、纯逻辑实现、特性清单共用；块设备与卷解析不共用；内核 VFS 在模块内（1.1） |

**未定项**（都不影响已定的 Vfs / Ext4 / ISO9660 / FAT32 只读接口面，写明归属阶段）：

| 项 | 待定内容 | 归属阶段 |
| --- | --- | --- |
| 写入与日志回放 | 写操作、按 `kKernelAccepted` 的完整判定、刷新原语在写路径上的使用、分配与校验和 | 阶段 3 以后 |
| 缓存、并发与异步 | VFS 页缓存、锁、异步读、工作队列；中断模式下的完成通知 | 阶段 3 |
| 多挂载点的实际使用 | 匹配算法已定；除 `/` 之外挂载点的消费方与跨卷路径语义 | 用户空间阶段 |
| 布局描述重读（第二来源） | 按 `BootInfo.layoutOffset` 重读布局描述并与 `BootInfo` 的 `volumeUuid` / `volumeOffset` / `volumeBytes` 交叉核对；M6 仍未落地 | M6 收尾 / M7 |
| `DeviceKind` 线索缩小候选 | 用 `Dev.Model` 绑定事实把 `DeviceKind` / `DevicePath` 映射到设备句柄；M6 候选仍为全量 | M6 收尾 / M7 |
| 设备移除与热替换 | 设备句柄注销、驱动重绑定（子设备视图回收已定） | 阶段 3 以后（模块不卸载、不热替换） |
| ISO9660 / FAT32 的内核挂载消费者 | 两格式的 `VolumeIdentity` 与 Caps 已定稿（4.6）；是否把 ISO9660 / FAT32 挂到 `/` 以外的挂载点、FAT32 交换卷读写、ISO9660 光盘文件访问 | 用户空间阶段 |
| ISO9660 第二来源的实现归属 | 内核核心已在 M6 用最小解析实现（PVD + 根目录项，不依赖 ISO9660 模块的 `FileSystemOps`）；是否收编到模块 / 接口（避免核心保留最小解析）、是否需要镜像文件 extent 事实（接口新版本追加） | M7 / 后续 |
| `SystemVolumeFacts` 扩展 | v1 只有三项；是否需要随 facts 暴露设备句柄 / 挂载名（新版本尾部追加） | M6 / 后续 |
| 运行期模块与用户态对 VFS 的访问面 | 是否把路径解析 / 文件引用（或新接口版本）导出给运行期模块与 AppSDK | 用户空间阶段 |
| 安装与恢复介质的卷挂载策略 | FAT32 / ISO9660 卷的挂载点、交换卷写入与安装恢复流程 | 用户空间阶段 |

## 八、外部依据

- [Kernel documentation: Ext4 Disk Layout](https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html)：superblock、特性位、extent 树与目录项的字段依据。
- [ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/)（ISO9660）与 [ECMA-107](https://ecma-international.org/publications-and-standards/standards/ecma-107/)（El Torito）：光盘卷与引导记录的结构依据。
- Microsoft FAT32 File System Specification（fatgen103，Microsoft Extensible Firmware Initiative）：FAT32 卷结构依据。

具体字段解读与实现取舍在实现阶段按对应标准与实测记录补充，本文不预写。
