# 文件系统抽象与 VFS

本文定义 Baleen 与内核共用的存储抽象：块设备、偏移视图、文件系统与路径解析的接口边界、错误与内存模型，以及两侧实现的共用与分列规则。

**状态：草案。** 接口定义与共用实现位于 `Packages/Common/Include/Storage/` 与 `Packages/Common/Src/Storage/`，取舍理由见第三节；阶段划分见第五节，已定项与未定项见第七节。

策略与范围（支持哪些介质与文件系统、读写边界）见[启动介质与文件系统](启动介质与文件系统.md)；VFS 与块设备层在内核中的归属见[内核架构与自举闭包](../内核架构与自举闭包.md)第二节；内核侧模块名与输出名称见[内核模块与驱动清单](../内核模块与驱动清单.md)。

## 一、共用口径与归属

[Baleen 引导器](../Baleen/README.md)第二节已经定下口径：**引导器与内核各自持有文件系统代码，两者不需要共享代码，但可识别的卷格式范围必须一致。** 本文按这条口径划分共用内容：

| 内容 | 共用 | 位置 |
| --- | --- | --- |
| 接口声明（块设备、偏移视图、文件系统、错误码） | 共用一份定义，不复制第二份 | `Packages/Common/Include/Storage/` |
| 错误诊断文本、无设备依赖的纯逻辑适配器 | 共用实现 | `Packages/Common/Src/Storage/` |
| 卷格式识别范围（Ext4 特性位接受 / 拒绝清单） | 共用一份清单，两侧实现引用 | `Packages/Common/Include/Storage/` |
| 块设备实现（BIOS 读盘、AHCI、NVMe、USB、固件块协议） | 不共用 | 各阶段自行实现 |
| 文件系统解析（Ext4 只读最小实现、内核完整读写实现） | 不共用 | Baleen 各阶段 / 内核 |
| 内核 VFS（挂载表、跨卷路径、文件引用与句柄、缓存） | 内核内部，不进共用包 | `Packages/LikesProgramOS` |

`Packages/Common` 的定位是“Baleen 各阶段与内核共用”，Print 前端与 `Platform` 原语同在该包；存储抽象沿用同一位置与“声明、实现分列”的组织方式。

**为什么接口共用而实现不共用：** Baleen 的 Ext4 读取是只装载镜像服务的最小只读实现（能力边界见 [Baleen 引导器](../Baleen/README.md)第五节），内核的 Ext4 是含日志回放与写入的完整实现；两者运行环境（32 位保护模式、无运行库 与 长模式、有内存管理）也不同。共用解析代码会把只读路径与完整实现绑死，但两侧各自实现同一格式时，特性接受范围必须由同一份清单约束，否则会出现“引导器读得了、内核读不了”或反向的布局。

## 二、分层

自下而上四层，前两层是本文定义并共用的部分，后两层按侧实现：

| 层 | 解决什么 | 谁实现 |
| --- | --- | --- |
| 块设备 `BlockDevice` | 固定大小逻辑块的随机访问；报告逻辑块大小与容量；只读 / 可写 | 各阶段：Baleen Core 走交权块的读盘服务，UEFI 走固件块协议，内核走启动设备驱动 |
| 偏移视图 `SubDevice` | 把一段字节区间映射为独立块设备：分区视图、光盘内的 Ext4 镜像文件 | 共用纯逻辑实现 |
| 文件系统 `FileSystem` | 识别并挂载卷、按路径查找、读取（内核侧另有写入） | Baleen 只读最小实现；内核完整实现 |
| 内核 VFS | 挂载表、跨卷路径、文件引用与句柄、缓存与同步 | 内核内部 |

**偏移视图是光盘路径的关键。** [启动介质与文件系统](启动介质与文件系统.md)第二节规定：光盘上的系统卷是 ISO9660 卷内的 Ext4 镜像文件，内核与引导器通过块设备偏移视图访问它；采用单区段实现时，镜像必须连续，装载时验证，不支持的布局明确拒绝。`SubDevice` 就是这条视图，`Open` 时检查对齐与边界，连续性的验证由调用方（镜像组装器与挂载方）按文件 extent 事实完成。

**VFS 不在本文定义。** 挂载树、跨卷路径解析、符号链接跟随、文件引用计数与页缓存属于内核内部契约，由内核规格单独定义；它建立在 `FileSystem` 抽象之上，不改变本文接口。Baleen 侧不需要 VFS：它只有“在一个卷里按一条路径取一个文件”的需求，由 `Stat` / `Open` / `Read` 直接完成。

## 三、接口草案

以下签名是共用接口的骨架。共同的硬规则：

- 不依赖 C++ 运行库：无异常、无 RTTI、不动态分配内存、不使用虚析构（实例不由基类指针销毁，理由与 `Print::OutTarget` 相同）
- 接口方法都提供默认实现，不用纯虚函数（`= 0`）：引导镜像可能不带 C++ 运行库，纯虚函数会让链接引入 `__cxa_pure_virtual`；代价是漏写覆盖变成运行期的 `NotSupported` 而不是编译期错误，由存储层的实现数量可控
- 所有缓冲由调用方提供；接口不返回需要释放的对象
- 参数非法与不支持的情况必须显式返回错误码，不静默截断、不猜测

### 3.1 错误模型

```cpp
// Storage/Error.hpp
namespace Storage {
    // 操作结果；除 Ok 外都表示失败，诊断文本由 ErrorMessage 给出
    enum class Error : int32_t {
        Ok = 0,            // 成功
        NotFound,          // 路径或对象不存在
        NotDirectory,      // 路径中间项不是目录
        IsDirectory,       // 目标要求常规文件，实际是目录
        NotSupported,      // 卷特性或操作不受支持，含未知的 Ext4 INCOMPAT 位
        Corrupt,           // 结构非法：魔数、校验、范围、环路
        Io,                // 底层读写失败
        ReadOnly,          // 目标只读
        NoSpace,           // 空间不足
        NameTooLong,       // 路径或单个名字超出实现上限
        InvalidArgument,   // 参数非法：空指针、零长度、未对齐、越界
    };

    // 错误码的诊断文本；共用实现，Baleen 阶段用于 FATAL 行，内核用于日志
    const char* ErrorMessage(Error error);
}
```

错误码是稳定编号：复用既有取值或改语义必须按[ABI 兼容与外部契约](ABI兼容与外部契约.md)第二节走新版本，不就地改写。

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
    };
}
```

接口方法都有默认实现（未覆盖时返回 `NotSupported`、`ReadOnly` 或 0），不用纯虚的理由见第三节开头。读与写取非 const：读盘会改动设备的内部状态（BIOS 会话、缓存、DMA 寄存器），把它写成 const 只是逻辑只读的伪装，实际实现会绕开类型系统。

Baleen Core 的实现把 `Read` 映射到交权块里的 `readSectors`，并在内部完成“高位缓冲 → 低 1MiB 弹跳窗口 → 拷贝”的中转：`readSectors` 只接受低 1MiB 目的地，这个约束不上升为接口语义，文件系统层看到的是普通字节偏移读。

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
    };
}
```

`Open` 只做对齐与边界（含 64 位加法的溢出）检查，用位与与减法判定，32 位目标上不牵入 64 位除法。

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

- **`Mount` 兼任格式探测**：失败即说明这个块设备不是本实现认识的卷；调用方换下一个候选设备或下一个文件系统，不需要单独的探测接口。
- **查找与比较是实现的职责**：`FindChild` 在一层目录里按卷内规则找子项——Ext4 逐字节比较，ISO9660 与 FAT32 按 ASCII 折叠比较；基类不预设任何一侧的语义。
- **共用遍历**：`ResolvePath` 用 `RootId` 与 `FindChild` 把统一路径逐段下降到目标；实现想要别的路径语义（如跟随符号链接）时可以覆盖 `Open` 与 `Stat`。
- **`Stat` 与 `Open` 的组合关系**：默认的 `Stat` 走 `Open` 再 `StatId`；能力位 `directoryListing` 为 false 的实现不必实现 `ReadDirectory`。
- **符号链接**：Baleen 依赖组装器的保证（布局描述给出的路径不得依赖符号链接，见 [Baleen 引导器](../Baleen/README.md)第五节），`symlinks` 能力位为 false 的实现遇到符号链接返回 `NotSupported`，不静默跟随。
- **实例存储由提供方持有**：Baleen 阶段用静态存储就地构造；内核侧由内存层分配。接口本身不含工厂与生命周期管理，也不经基类指针销毁实例（无虚析构）。

### 3.5 统一路径索引

同一套路径语法把所有支持的卷接进同一棵目录树；**卷内名字怎么比较由各实现自己决定**。共用的是索引方式（路径形式、切分、遍历），不是命中规则（比较语义）——把折叠写进共用层，会让 Ext4 或 ISO9660 中的一侧出现“卷上明明有、按路径却读不到”的情况。

- **路径形式（共用）**：以 `/` 起头的绝对路径，`/` 分隔分量，根路径为 `/`；分量非空、不含 `.` 与 `..`，不以 `/` 结尾；整条路径上限 1024 字节、单个分量上限 255 字节；非 ASCII 字节不做字符集变换与换行翻译。
- **语法与切分（共用）**：`CheckPath` 给出语法判定（`InvalidArgument` / `NameTooLong`），`PathWalker` 逐段交出分量并给出总数；两者都不折叠大小写、不解析符号链接。
- **查找（各实现）**：`FindChild` 在目录里按卷内规则找到子项；实现从共用比较函数里选用——`NameEquals`（逐字节）、`NameEqualsIgnoreCase`（ASCII 折叠），不各写一份。
- **语义报告（共用类型）**：`FileSystemCaps::nameCase` 取 `Sensitive` 或 `Insensitive`，调用方（内核 VFS、Baleen 侧取内核路径的代码）据此决定自己一侧的名字处理，不替实现做折叠。
- **遍历（共用）**：`ResolvePath` 从 `RootId` 出发逐段调用 `FindChild`，把错误码原样上报；根路径直接给出根目录标识。

```cpp
// Storage/Path.hpp
namespace Storage {
    struct PathRules {
        static constexpr char kSeparator = '/';          // 分量分隔符
        static constexpr uint32_t kMaxBytes = 1024;      // 整条路径上限，含起头分隔符
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
}
```

Baleen 侧与内核侧看到的是同一条路径写法与同一套分量切分；ISO9660 卷上的全大写名字与 Ext4 卷上的大小写敏感名字因此可以出现在同一棵树里，由各卷的实现各自命中。

### 3.6 卷格式识别范围

同一格式的接受 / 拒绝清单由共用头文件单点定义，例如 Ext4 的 `INCOMPAT` / `RO_COMPAT` 位接受表与拒绝表。两侧实现引用同一份清单，并用同一套宿主侧检查验证：

- 未知的 `INCOMPAT` 位一律拒绝，不猜测语义
- 密钥 / 加密、内联数据等在范围外的特性明确拒绝并报告
- Baleen 侧只读实现的拒绝集合是内核完整实现的子集：引导器能读的卷，内核必须也能读

清单的取值在实现阶段按当时的磁盘格式分配填出，本文不预写具体位。

## 四、运行环境约束

| 约束 | 由来与做法 |
| --- | --- |
| 无异常、无 RTTI、无运行库 | Baleen 各阶段用 `-ffreestanding -fno-exceptions -fno-rtti` 编译；接口不出现 `throw`、`dynamic_cast` |
| 不动态分配 | 引导期无堆；缓冲与实例存储都由调用方提供 |
| 32 位目标不牵入 64 位除法 | 逻辑块大小是 2 的幂，块号换算用移位；接口签名保留 64 位偏移，实现内部按需收窄 |
| 单实例与静态存储 | Baleen 侧文件系统与块设备都是静态实例，不做并发；内核侧在同一接口上实现多卷实例与并发规则 |
| 只读优先 | Baleen 全部路径只读；内核早期同样只读，写入在[启动介质与文件系统](启动介质与文件系统.md)第五节的条件满足后启用 |

## 五、阶段划分与验证

| 阶段 | 交付物 | 验证方式 |
| --- | --- | --- |
| S1 接口与共用件 | `Packages/Common/Include/Storage/` 的接口头与 `Src/Storage/` 的实现：`Path`、`BlockDevice`、`SubDevice`、`FileSystem` 与 `ErrorMessage`；`Packages/Common/Makefile` 收录新对象 | 宿主侧检查：统一路径与名字比较的用例、假块设备上的偏移视图边界与对齐用例、错误文本；32 位与 64 位两套编译选项下均编译并链接通过 |
| S2 Ext4 只读实现（Baleen 侧） | 目标侧最小只读实现：superblock、特性清单、块组描述符、inode、extent、目录项、按路径读文件；Baleen Core 的块设备适配器 | 真实 Ext4 镜像上的路径读取与摘要比对；故障注入：坏 superblock、未知 `INCOMPAT`、不存在的路径、越界 extent、非连续镜像 |
| S3 Baleen Core 接入 | 按 `BaleenLayout.bin` 的路径读内核镜像并校验载荷头（[Baleen 引导器](../Baleen/README.md)第五节） | 与 S2 同一套镜像夹具；QEMU / Bochs 启动与故障注入 |
| S4 内核侧实现 | 内核 VFS、Ext4 完整实现（含日志回放与写入）、ISO9660、FAT32File、启动设备驱动之上的块设备 | 内核规格另行定义，不因本草案提前承诺 |

**S1 不依赖 Baleen Core 的其余部分。** Baleen Core 的布局描述格式、选版与签名策略与本抽象解耦：文件系统只回答“按路径取文件”，不解释 `BaleenLayout.bin`，也不参与选版。

**宿主侧参考实现不承诺与目标侧共用。** S2 的目标侧实现按引导环境约束写；如果组装器或验证需要独立的参考解析，那是宿主侧的另一份实现（与 MakeImage / TestInstaller 的现有做法一致），两者靠同一套镜像夹具与摘要比对交叉验证，而不是靠共享代码。

## 六、与现有规格的关系

- [启动介质与文件系统](启动介质与文件系统.md)：介质与访问链、必备文件系统、读写与阶段切换策略；本文只定义接口层，不改写其策略。
- [Baleen 引导器](../Baleen/README.md)：卷定位流程与最小只读实现的能力边界；本文的接口按那套边界设计。
- [内核架构与自举闭包](../内核架构与自举闭包.md)：VFS 与块设备层、Ext4 / ISO9660 / FAT32 的内置归属；本文是它们的接口草案。
- [ABI 兼容与外部契约](ABI兼容与外部契约.md)：接口表尺寸、版本与错误码的兼容规则适用。
- [输出通道与Print](../输出通道与Print.md)：错误文本经 Print 输出时的标签约定；`FATAL` 行使用 `ErrorMessage` 的文本。

## 七、已定与未定

已定项：目录遍历取游标式（`ReadDirectory` 的 `start` 取值由实现定义）；接口只描述操作，实例生命周期完全由调用方（Baleen 侧静态存储、内核侧内存层）；接口方法用默认实现而非纯虚函数；路径的比较语义由各实现经 `nameCase` 报告，共用层不做折叠；`Mount` 与偏移视图取设备的非 const 引用。

未定项：

| 项 | 待定内容 |
| --- | --- |
| 卷标识查询 | 是否需要显式的卷标识（Ext4 UUID / 标签）查询接口，以及它属于 `FileSystem` 还是挂载方 |
| 读取边界 | `Read` 跨文件尾的部分读取语义；暂定“至多读到文件尾、超出部分以实际长度回报” |
| VFS 分界 | 挂载表与跨卷路径最终由内核 VFS 承担；Baleen 侧的“单卷路径”是否需要独立的最简封套 |
| 特性清单位置 | Ext4 接受 / 拒绝清单放共用头文件，还是由宿主工具从单点定义生成两份声明；清单本身在 S2 按磁盘格式填出 |
| 验证工具形态 | S1 / S2 的宿主侧检查工具命名与归属（`Tools/Build/CheckStorage` 或并入既有工具） |
| 内核侧构建 | 内核的编译选项与链接方式；同一份源码须在 32 位与 64 位下编译验证、不引入 64 位除法辅助函数，接入内核构建的形态未定 |

## 八、外部依据

- [Kernel documentation: Ext4 Disk Layout](https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html)：superblock、特性位、extent 树与目录项的字段依据。
- [ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/)（ISO9660）与 [ECMA-107](https://ecma-international.org/publications-and-standards/standards/ecma-107/)（El Torito）：光盘卷与引导记录的结构依据。
- Microsoft FAT32 File System Specification（fatgen103，Microsoft Extensible Firmware Initiative）：FAT32 卷结构依据。

具体字段解读与实现取舍在实现阶段按对应标准与实测记录补充，本文不预写。
