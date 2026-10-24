/* main.cpp
    CheckVfs：Vfs 模块（Block.Registry / Fs.Vfs / Boot.Volume）的宿主侧检查，手工执行

    检查对象是 Packages/KernelModules/Vfs/Src 的目标侧源码：用 vpath 按相对路径直接编进本工具，
    不复制实现；宿主替身提供伪 BlockDeviceOps、伪 FileSystemOps、伪 Core.* 导入槽与内存缓冲。
    伪块设备按共用块设备语义校验对齐 / 范围 / 指针；伪文件系统实现挂载探测、卷标识、
    路径查找、读取与目录遍历，可注入错误码用于检查透传
    覆盖：Publish / SubDevice / Acquire / Release、挂载点最长匹配、句柄 Open / Read / Close、
    ReadDirectory、错误码映射、MountSystemVolume 唯一候选与歧义拒绝
    每个用例打印 [ OK ] 或 [ FAIL ]；用例数固定，失败返回 1；最后一行打印用例计数
*/

#include <ModuleAbi.hpp>

#include <VfsCore.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // 用例总数：与用例表长度核对，防止用例被误删
    constexpr unsigned kCaseCount = 52;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) {
        if (!value) Fail(message);
    }

    // 伪日志：记录最近一次 Line 的标签与文本
    struct FakeLog {
        char tag[16] = {};        // 最近一次标签
        char text[256] = {};      // 最近一次文本
        unsigned lines = 0;       // Line 调用次数
    };
    FakeLog g_log;

    // 伪日志写文本：记录到 text，不做换行
    void FakeLogWrite(const char* text) {
        if (text == nullptr) return;
        std::strncpy(g_log.text, text, sizeof(g_log.text) - 1);
    }

    // 伪日志写一行：记录标签与文本
    void FakeLogLine(const char* tag, const char* text) {
        ++g_log.lines;
        std::strncpy(g_log.tag, tag != nullptr ? tag : "", sizeof(g_log.tag) - 1);
        std::strncpy(g_log.text, text != nullptr ? text : "", sizeof(g_log.text) - 1);
    }

    // 伪 Core.Mem：拷贝
    void FakeMemCopy(void* destination, const void* source, uint64_t bytes) {
        if (destination == nullptr || source == nullptr || bytes == 0) return;
        std::memcpy(destination, source, static_cast<std::size_t>(bytes));
    }

    // 伪 Core.Mem：清零
    void FakeMemZero(void* destination, uint64_t bytes) {
        if (destination == nullptr || bytes == 0) return;
        std::memset(destination, 0, static_cast<std::size_t>(bytes));
    }

    // 伪 Core.Mem：比较
    int32_t FakeMemCompare(const void* left, const void* right, uint64_t bytes) {
        if (left == nullptr || right == nullptr) return 0;
        return static_cast<int32_t>(std::memcmp(left, right, static_cast<std::size_t>(bytes)));
    }

    // 伪 Core.Page：页大小
    uint32_t FakePageBytes() { return 4096; }

    // 伪 BootInfo：Boot.Volume 的输入事实
    Boot::Info g_bootInfo;


    // 伪 Dev.Model 的绑定事实：Vfs Init 按它把已启动设备经 Block.Registry.Publish 登记
    struct HarnessBinding {
        DeviceHandle handle;            // 设备句柄
        const BlockDeviceOps* ops;      // 驱动 ops（仅 state == Started 时有效）
        void* context;                  // 驱动上下文（仅 state == Started 时有效）
        std::uint32_t state;            // BindingFacts.state：0 未绑定、1 只绑定、2 已启动
    };

    // 当前用例的绑定事实
    HarnessBinding g_bindings[VfsModule::kMaxDevices];
    // 已登记绑定事实条数
    std::uint32_t g_bindingCount = 0;

    // 伪 Dev.Model：枚举设备句柄，游标从 0 起，结束返回 kNotFound
    KernelError FakeDevModelEnumerate(std::uint32_t* cursor, DeviceFacts* facts) {
        if (cursor == nullptr || facts == nullptr || facts->bytes < sizeof(DeviceFacts)) return KernelError::kInvalidArgument;
        while (*cursor < g_bindingCount) {
            const HarnessBinding& binding = g_bindings[(*cursor)++];
            facts->bytes = static_cast<std::uint32_t>(sizeof(DeviceFacts));
            facts->deviceHandle = binding.handle;
            facts->busKind = 0;
            facts->classCode = 0;
            facts->vendorId = 0;
            facts->deviceId = 0;
            facts->busAddress = 0;
            return KernelError::kOk;
        }
        return KernelError::kNotFound;
    }

    // 伪 Dev.Model：零初始化（bytes == 0）是查询，未绑定返回 kNotFound；
    // 非查询路径（bytes 非零）按 DevModel 的登记/清除分支返回 kOk 但不回填事实
    KernelError FakeDevModelBinding(DeviceHandle handle, BindingFacts* facts) {
        if (facts == nullptr) return KernelError::kInvalidArgument;
        if (facts->bytes != 0) return KernelError::kOk;
        for (std::uint32_t index = 0; index < g_bindingCount; ++index) {
            const HarnessBinding& binding = g_bindings[index];
            if (binding.handle != handle) continue;
            if (binding.state == 0) return KernelError::kNotFound;
            facts->bytes = static_cast<std::uint32_t>(sizeof(BindingFacts));
            facts->bindHandle = 1;
            facts->deviceHandle = binding.handle;
            facts->driverModule = kModAtaPio;
            facts->state = binding.state;
            facts->ops = binding.state == VfsModule::kBindingStateStarted ? binding.ops : nullptr;
            facts->context = binding.state == VfsModule::kBindingStateStarted ? binding.context : nullptr;
            return KernelError::kOk;
        }
        return KernelError::kNotFound;
    }

    // 伪 Dev.Model 绑定查询的坏实现：总返回 kOk 但不回填事实（检查 Vfs 不信任裸 kOk）
    KernelError BrokenDevModelBinding(DeviceHandle handle, BindingFacts* facts) {
        (void)handle;
        (void)facts;
        return KernelError::kOk;
    }

    // 伪 Core.Boot：返回 BootInfo 只读视图
    const Boot::Info* FakeBootInfo() { return &g_bootInfo; }

    // 伪块设备：内存缓冲 + 可配置块大小 / 写能力 / 刷新原语 / 错误注入
    struct FakeBlock {
        std::vector<std::uint8_t> bytes;      // 设备内容
        std::uint32_t blockBytes = 512;       // 逻辑块大小
        bool writable = false;                // 是否可写
        bool flushPrimitive = false;          // 是否有刷新原语
        Storage::Error readResult = Storage::Error::Ok;   // 读错误注入
        Storage::Error writeResult = Storage::Error::Ok;  // 写错误注入
        unsigned reads = 0;                   // 读回调次数
        unsigned writes = 0;                  // 写回调次数
        unsigned flushes = 0;                 // 刷新回调次数
        // 依次返回的块大小覆盖值（0 表示用 blockBytes）：模拟探测与切窗口之间的块大小事实变化
        std::uint32_t blockBytesSequence[3] = {};
        // 块大小查询次数：按次取 blockBytesSequence
        unsigned blockBytesCalls = 0;
        // 依次返回的容量覆盖值（0 表示用实际容量）：模拟探测与切窗口之间的容量事实变化
        std::uint64_t capacitySequence[2] = {};
        // 容量查询次数：按次取 capacitySequence
        unsigned capacityCalls = 0;
    };

    // 伪块设备 ops：块大小（blockBytesSequence 非 0 时按查询次数覆盖）
    uint32_t FakeBlockBytes(void* context) {
        FakeBlock& block = *static_cast<FakeBlock*>(context);
        const unsigned call = block.blockBytesCalls++;
        if (call < 3 && block.blockBytesSequence[call] != 0) return block.blockBytesSequence[call];
        return block.blockBytes;
    }

    // 伪块设备 ops：容量（capacitySequence 非 0 时按查询次数覆盖）
    uint64_t FakeCapacityBytes(void* context) {
        FakeBlock& block = *static_cast<FakeBlock*>(context);
        const unsigned call = block.capacityCalls++;
        if (call < 2 && block.capacitySequence[call] != 0) return block.capacitySequence[call];
        return block.bytes.size();
    }

    // 伪块设备 ops：可写
    uint32_t FakeWritable(void* context) { return static_cast<FakeBlock*>(context)->writable ? 1 : 0; }

    // 伪块设备 ops：读，按共用块设备语义校验对齐 / 范围 / 指针
    Storage::Error FakeRead(void* context, uint64_t offset, void* buffer, std::uint32_t bytes) {
        FakeBlock& block = *static_cast<FakeBlock*>(context);
        ++block.reads;
        if (block.readResult != Storage::Error::Ok) return block.readResult;
        if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
        if (block.blockBytes == 0 || (block.blockBytes & (block.blockBytes - 1)) != 0) return Storage::Error::InvalidArgument;
        if ((offset % block.blockBytes) != 0 || (bytes % block.blockBytes) != 0) return Storage::Error::InvalidArgument;
        if (offset > block.bytes.size() || bytes > block.bytes.size() - offset) return Storage::Error::InvalidArgument;
        std::memcpy(buffer, block.bytes.data() + offset, bytes);
        return Storage::Error::Ok;
    }

    // 伪块设备 ops：写，只读返回 ReadOnly
    Storage::Error FakeWrite(void* context, uint64_t offset, const void* buffer, std::uint32_t bytes) {
        FakeBlock& block = *static_cast<FakeBlock*>(context);
        ++block.writes;
        if (block.writeResult != Storage::Error::Ok) return block.writeResult;
        if (!block.writable) return Storage::Error::ReadOnly;
        if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
        if ((offset % block.blockBytes) != 0 || (bytes % block.blockBytes) != 0) return Storage::Error::InvalidArgument;
        if (offset > block.bytes.size() || bytes > block.bytes.size() - offset) return Storage::Error::InvalidArgument;
        std::memcpy(block.bytes.data() + offset, buffer, bytes);
        return Storage::Error::Ok;
    }

    // 伪块设备 ops：刷新
    Storage::Error FakeFlush(void* context) {
        FakeBlock& block = *static_cast<FakeBlock*>(context);
        ++block.flushes;
        return Storage::Error::Ok;
    }

    // 构造伪块设备 ops 表：有能力位时带上刷新回调，否则 Flush 为空
    BlockDeviceOps FakeOps(FakeBlock& block) {
        BlockDeviceOps ops{};
        ops.capabilities = block.flushPrimitive ? kBlockCapabilityFlushPrimitive : 0;
        ops.BlockBytes = &FakeBlockBytes;
        ops.CapacityBytes = &FakeCapacityBytes;
        ops.Writable = &FakeWritable;
        ops.Read = &FakeRead;
        ops.Write = &FakeWrite;
        ops.Flush = block.flushPrimitive ? &FakeFlush : nullptr;
        return ops;
    }

    // 伪文件系统：小型固定目录树，可配置格式名、卷标识与错误注入
    struct FakeFs {
        const char* format = "fakefs";              // 格式名
        std::uint8_t uuid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01, 0x02}; // 卷标识
        bool mounted = false;                       // 是否已挂载
        std::uint32_t mountBlockBytes = 0;          // 挂载时看到的逻辑块大小
        Storage::Error mountError = Storage::Error::Ok; // 挂载错误注入
        Storage::Error statError = Storage::Error::Ok;  // Stat 错误注入
        char lastPath[VfsModule::kMaxPathBytes + 1] = {}; // 最近一次收到的卷内路径
        std::uint32_t lastPathBytes = 0;            // 最近一次收到的卷内路径长度
        unsigned mounts = 0;                        // 挂载次数
        unsigned unmounts = 0;                      // 卸载次数
    };

    // 伪文件系统：格式名
    const char* FakeFormat(void* context) { return static_cast<FakeFs*>(context)->format; }

    // 伪文件系统：挂载探测（设备第一个逻辑块必须以 FAKEFS 起头）
    Storage::Error FakeMount(void* context, const BlockDeviceOps* device, void* deviceContext, std::uint32_t mode) {
        FakeFs& fileSystem = *static_cast<FakeFs*>(context);
        if (device == nullptr || deviceContext == nullptr) return Storage::Error::InvalidArgument;
        if (mode != static_cast<std::uint32_t>(Storage::MountMode::ReadOnly)) return Storage::Error::NotSupported;
        if (fileSystem.mountError != Storage::Error::Ok) return fileSystem.mountError;
        if (device->BlockBytes == nullptr || device->Read == nullptr) return Storage::Error::InvalidArgument;
        const std::uint32_t blockBytes = device->BlockBytes(deviceContext);
        if (blockBytes < 512) return Storage::Error::InvalidArgument;
        std::uint8_t buffer[512] = {};
        const Storage::Error error = device->Read(deviceContext, 0, buffer, 512);
        if (error != Storage::Error::Ok) return error;
        static constexpr char kMagic[] = "FAKEFS";
        for (std::uint32_t index = 0; index < 6; ++index) {
            if (buffer[index] != static_cast<std::uint8_t>(kMagic[index])) return Storage::Error::NotSupported;
        }
        fileSystem.mounted = true;
        fileSystem.mountBlockBytes = blockBytes;
        ++fileSystem.mounts;
        return Storage::Error::Ok;
    }

    // 伪文件系统：卸载
    void FakeUnmount(void* context) {
        FakeFs& fileSystem = *static_cast<FakeFs*>(context);
        fileSystem.mounted = false;
        ++fileSystem.unmounts;
    }

    // 伪文件系统：能力位
    void FakeCaps(void* context, Storage::FileSystemCaps* caps) {
        (void)context;
        if (caps == nullptr) return;
        caps->writable = false;
        caps->directoryListing = true;
        caps->symlinks = false;
        caps->nameCase = Storage::NameCase::Sensitive;
    }

    // 伪文件系统：卷标识
    Storage::Error FakeVolumeIdentity(void* context, std::uint8_t* volumeId) {
        FakeFs& fileSystem = *static_cast<FakeFs*>(context);
        if (volumeId == nullptr) return Storage::Error::InvalidArgument;
        if (!fileSystem.mounted) return Storage::Error::InvalidArgument;
        std::memcpy(volumeId, fileSystem.uuid, 16);
        return Storage::Error::Ok;
    }

    // 伪文件系统的路径解析：支持根、/hello.txt、/System、/System/readme
    Storage::Error FakeResolve(FakeFs& fileSystem, const char* path, std::uint64_t& object, Storage::FileInfo& info) {
        if (path == nullptr) return Storage::Error::InvalidArgument;
        if (!fileSystem.mounted) return Storage::Error::InvalidArgument;
        fileSystem.lastPathBytes = static_cast<std::uint32_t>(std::strlen(path));
        if (fileSystem.lastPathBytes >= sizeof(fileSystem.lastPath)) return Storage::Error::NameTooLong;
        std::memcpy(fileSystem.lastPath, path, fileSystem.lastPathBytes);
        fileSystem.lastPath[fileSystem.lastPathBytes] = '\0';
        info = Storage::FileInfo{};
        if (std::strcmp(path, "/") == 0) {
            object = 1;
            info.id = 1;
            info.directory = true;
            return Storage::Error::Ok;
        }
        if (std::strcmp(path, "/hello.txt") == 0) {
            object = 2;
            info.id = 2;
            info.bytes = 5;
            info.directory = false;
            return Storage::Error::Ok;
        }
        if (std::strcmp(path, "/System") == 0) {
            object = 3;
            info.id = 3;
            info.directory = true;
            return Storage::Error::Ok;
        }
        if (std::strcmp(path, "/System/readme") == 0) {
            object = 4;
            info.id = 4;
            info.bytes = 3;
            info.directory = false;
            return Storage::Error::Ok;
        }
        if (std::strncmp(path, "/hello.txt/", 11) == 0) return Storage::Error::NotDirectory;
        if (std::strncmp(path, "/System/", 8) == 0) return Storage::Error::NotFound;
        return Storage::Error::NotFound;
    }

    // 伪文件系统：查询事实
    Storage::Error FakeStat(void* context, const char* path, Storage::FileInfo* info) {
        FakeFs& fileSystem = *static_cast<FakeFs*>(context);
        if (info == nullptr) return Storage::Error::InvalidArgument;
        if (fileSystem.statError != Storage::Error::Ok) return fileSystem.statError;
        std::uint64_t object = 0;
        return FakeResolve(fileSystem, path, object, *info);
    }

    // 伪文件系统：解析对象标识
    Storage::Error FakeOpen(void* context, const char* path, std::uint64_t* object) {
        FakeFs& fileSystem = *static_cast<FakeFs*>(context);
        if (object == nullptr) return Storage::Error::InvalidArgument;
        Storage::FileInfo info{};
        return FakeResolve(fileSystem, path, *object, info);
    }

    // 伪文件系统：按标识读文件
    Storage::Error FakeRead(void* context, std::uint64_t object, std::uint64_t offset, void* buffer,
        std::uint32_t bytes, std::uint32_t* read) {
        FakeFs& fileSystem = *static_cast<FakeFs*>(context);
        if (read == nullptr || buffer == nullptr) return Storage::Error::InvalidArgument;
        *read = 0;
        if (!fileSystem.mounted) return Storage::Error::InvalidArgument;
        if (bytes == 0) return Storage::Error::InvalidArgument;
        if (object == 1 || object == 3) return Storage::Error::IsDirectory;
        if (object != 2 && object != 4) return Storage::Error::InvalidArgument;
        const char* content = object == 2 ? "hello" : "abc";
        const std::uint64_t length = object == 2 ? 5 : 3;
        if (offset >= length) return Storage::Error::Ok;
        std::uint64_t available = length - offset;
        if (available > bytes) available = bytes;
        std::memcpy(buffer, content + offset, static_cast<std::size_t>(available));
        *read = static_cast<std::uint32_t>(available);
        return Storage::Error::Ok;
    }

    // 伪文件系统：游标式目录遍历
    Storage::Error FakeReadDirectory(void* context, std::uint64_t object, std::uint64_t* cursor, char* name,
        std::uint32_t nameBytes, std::uint32_t* nameLength, Storage::FileInfo* info) {
        FakeFs& fileSystem = *static_cast<FakeFs*>(context);
        (void)fileSystem;
        if (cursor == nullptr || name == nullptr || nameLength == nullptr || info == nullptr) {
            return Storage::Error::InvalidArgument;
        }
        *nameLength = 0;
        if (object != 1 && object != 3) return Storage::Error::NotDirectory;
        const char* entryName = nullptr;
        std::uint32_t entryBytes = 0;
        bool entryDirectory = false;
        if (object == 1) {
            if (*cursor == 0) {
                entryName = "hello.txt";
                entryBytes = 9;
            } else if (*cursor == 1) {
                entryName = "System";
                entryBytes = 6;
                entryDirectory = true;
            } else {
                return Storage::Error::NotFound;
            }
        } else {
            if (*cursor != 0) return Storage::Error::NotFound;
            entryName = "readme";
            entryBytes = 6;
        }
        if (entryBytes >= nameBytes) return Storage::Error::NameTooLong;
        std::memcpy(name, entryName, entryBytes);
        name[entryBytes] = '\0';
        *nameLength = entryBytes;
        info->id = 100 + *cursor;
        info->bytes = entryDirectory ? 0 : 5;
        info->directory = entryDirectory;
        ++*cursor;
        return Storage::Error::Ok;
    }

    // 伪文件系统 ops 表：9 个槽
    FileSystemOps FakeFsOps() {
        FileSystemOps ops{};
        ops.Format = &FakeFormat;
        ops.Mount = &FakeMount;
        ops.Unmount = &FakeUnmount;
        ops.Caps = &FakeCaps;
        ops.VolumeIdentity = &FakeVolumeIdentity;
        ops.Stat = &FakeStat;
        ops.Open = &FakeOpen;
        ops.Read = &FakeRead;
        ops.ReadDirectory = &FakeReadDirectory;
        return ops;
    }

    // 调用模块 Init 入口：按当前导入回填区与 Dev.Model 绑定事实登记设备
    KernelError CallInit() {
        KernelAbiContext context{};
        context.bytes = static_cast<std::uint32_t>(sizeof(KernelAbiContext));
        context.version = 1;
        context.kernelAbiVersion = 1;
        context.moduleBase = 0x1000000;
        context.moduleId = kModVfs;
        return _Module_Init(&context);
    }

    // 初始化模块：清空上一用例的绑定事实，填导入回填区（伪 Core.* 与伪 Dev.Model）后 Init
    void InitModule() {
        g_bindingCount = 0;
        g_bootInfo = Boot::Info{};
        g_log = FakeLog{};
        _Contract_Import_CoreBoot_v1.Info = &FakeBootInfo;
        _Contract_Import_CoreLog_v1.Write = &FakeLogWrite;
        _Contract_Import_CoreLog_v1.Line = &FakeLogLine;
        _Contract_Import_CoreLog_v1.Hex = nullptr;
        _Contract_Import_CoreLog_v1.Code = nullptr;
        _Contract_Import_CoreMem_v1.Copy = &FakeMemCopy;
        _Contract_Import_CoreMem_v1.CopyOverlap = nullptr;
        _Contract_Import_CoreMem_v1.Zero = &FakeMemZero;
        _Contract_Import_CoreMem_v1.Compare = &FakeMemCompare;
        _Contract_Import_CorePage_v1.PageBytes = &FakePageBytes;
        _Contract_Import_DevModel_v1.Enumerate = &FakeDevModelEnumerate;
        _Contract_Import_DevModel_v1.Binding = &FakeDevModelBinding;
        Require(CallInit() == KernelError::kOk, "module Init failed in the harness");
    }

    // 造一个已发布设备的夹具：伪块设备 + ops + 句柄
    struct DeviceFixture {
        FakeBlock block;              // 伪块设备状态
        BlockDeviceOps ops{};         // 调用表
        DeviceHandle handle = 0;      // 登记句柄
    };

    // 初始化伪块设备：容量、块大小、内容以 FAKEFS 起头并铺可辨识字节
    void FillBlock(FakeBlock& block, std::uint64_t totalBytes, std::uint32_t blockBytes) {
        block = FakeBlock{};
        block.blockBytes = blockBytes;
        block.bytes.assign(static_cast<std::size_t>(totalBytes), 0);
        static constexpr char kMagic[] = "FAKEFS";
        for (std::uint32_t index = 0; index < 6; ++index) block.bytes[index] = static_cast<std::uint8_t>(kMagic[index]);
        for (std::size_t index = 6; index < block.bytes.size(); ++index) {
            block.bytes[index] = static_cast<std::uint8_t>(index & 0xFF);
        }
    }

    // 登记一个根设备：成功返回句柄
    // 在设备某偏移处写入文件系统魔数：系统卷区段经偏移视图挂载时，魔数必须落在区段起点
    void MarkVolumeStart(FakeBlock& block, std::uint64_t offset) {
        static constexpr char kMagic[] = "FAKEFS";
        if (offset + 6 > block.bytes.size()) Fail("volume start is beyond the device");
        for (std::uint32_t index = 0; index < 6; ++index) block.bytes[offset + index] = static_cast<std::uint8_t>(kMagic[index]);
    }

    // 登记一条 Dev.Model 绑定事实并重跑 Init：设备由 Vfs Init 内部自动 Publish
    void PublishDevice(DeviceFixture& fixture, DeviceHandle handle) {
        fixture.ops = FakeOps(fixture.block);
        fixture.handle = handle;
        Require(g_bindingCount < VfsModule::kMaxDevices, "binding table overflow");
        g_bindings[g_bindingCount++] = HarnessBinding{handle, &fixture.ops, &fixture.block, VfsModule::kBindingStateStarted};
        Require(VfsModule::RegisterDevicesFromBindings() == KernelError::kOk, "publishing bound devices failed");
    }

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // ---- Init 与入口 ----
        cases.push_back({ "Init rejects a null context", [] {
            InitModule();
            Require(_Module_Init(nullptr) == KernelError::kInvalidArgument, "null context was accepted");
        } });
        cases.push_back({ "Init rejects a short context", [] {
            InitModule();
            KernelAbiContext context{};
            context.bytes = 4;
            Require(_Module_Init(&context) == KernelError::kInvalidArgument, "short context was accepted");
        } });
        cases.push_back({ "Init rejects a foreign module identity", [] {
            InitModule();
            KernelAbiContext context{};
            context.bytes = static_cast<std::uint32_t>(sizeof(KernelAbiContext));
            context.moduleId = kModExt4;
            Require(_Module_Init(&context) == KernelError::kInvalidArgument, "foreign module id was accepted");
        } });
        cases.push_back({ "Init rejects missing imports", [] {
            InitModule();
            _Contract_Import_CoreLog_v1.Line = nullptr;
            KernelAbiContext context{};
            context.bytes = static_cast<std::uint32_t>(sizeof(KernelAbiContext));
            context.moduleId = kModVfs;
            Require(_Module_Init(&context) == KernelError::kNotSupported, "missing import was accepted");
        } });
        cases.push_back({ "Abort resets the registries", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::DeviceExists(0x10), "device wasn't published");
            Require(_Module_Abort(nullptr) == KernelError::kOk, "Abort failed");
            Require(!VfsModule::DeviceExists(0x10), "Abort didn't clear the registry");
            InitModule();
        } });
        // ---- Block.Registry ----
        cases.push_back({ "Publish rejects a zero handle", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            device.ops = FakeOps(device.block);
            Require(VfsModule::Publish(0, &device.ops, &device.block, 0) == Storage::Error::InvalidArgument, "zero handle was accepted");
        } });
        cases.push_back({ "Publish rejects a broken ops table", [] {
            InitModule();
            FakeBlock block;
            FillBlock(block, 4096, 512);
            Require(VfsModule::Publish(0x10, nullptr, &block, 0) == Storage::Error::InvalidArgument, "null ops was accepted");
            BlockDeviceOps ops = FakeOps(block);
            ops.Read = nullptr;
            Require(VfsModule::Publish(0x10, &ops, &block, 0) == Storage::Error::InvalidArgument, "missing Read was accepted");
        } });
        cases.push_back({ "Publish rejects nonzero flags", [] {
            InitModule();
            FakeBlock block;
            FillBlock(block, 4096, 512);
            BlockDeviceOps ops = FakeOps(block);
            Require(VfsModule::Publish(0x10, &ops, &block, 1) == Storage::Error::InvalidArgument, "nonzero flags was accepted");
        } });
        cases.push_back({ "Publish rejects an invalid block size", [] {
            InitModule();
            FakeBlock block;
            FillBlock(block, 4096, 1000);
            BlockDeviceOps ops = FakeOps(block);
            Require(VfsModule::Publish(0x10, &ops, &block, 0) == Storage::Error::InvalidArgument, "non power-of-two block size was accepted");
        } });
        cases.push_back({ "Publish rejects a duplicate handle", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            FakeBlock second;
            FillBlock(second, 4096, 512);
            BlockDeviceOps ops = FakeOps(second);
            Require(VfsModule::Publish(0x10, &ops, &second, 0) == Storage::Error::InvalidArgument, "duplicate handle was accepted");
        } });
        cases.push_back({ "Publish requires a flush callback with the capability bit", [] {
            InitModule();
            FakeBlock block;
            FillBlock(block, 4096, 512);
            BlockDeviceOps ops = FakeOps(block);
            ops.capabilities = kBlockCapabilityFlushPrimitive;
            ops.Flush = nullptr;
            Require(VfsModule::Publish(0x10, &ops, &block, 0) == Storage::Error::InvalidArgument, "missing Flush was accepted");
            block.flushPrimitive = false;
            ops = FakeOps(block);
            ops.capabilities = 0;
            ops.Flush = nullptr;
            Require(VfsModule::Publish(0x10, &ops, &block, 0) == Storage::Error::Ok, "no-flush device was rejected");
        } });
        cases.push_back({ "Acquire and Release track the reference count", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            const BlockDeviceOps* ops = nullptr;
            void* context = nullptr;
            Require(VfsModule::Acquire(0x10, &ops, &context) == Storage::Error::Ok, "Acquire failed");
            Require(ops != nullptr && context == &device.block, "Acquire returned wrong ops/context");
            Require(VfsModule::ReferenceCount(0x10) == 1, "reference count isn't 1");
            Require(VfsModule::Release(0x10) == Storage::Error::Ok, "Release failed");
            Require(VfsModule::ReferenceCount(0x10) == 0, "reference count isn't 0");
            Require(VfsModule::Release(0x10) == Storage::Error::InvalidArgument, "underflow Release was accepted");
        } });
        cases.push_back({ "Acquire rejects an unknown handle", [] {
            InitModule();
            const BlockDeviceOps* ops = nullptr;
            void* context = nullptr;
            Require(VfsModule::Acquire(0x77, &ops, &context) == Storage::Error::NotFound, "unknown handle was acquired");
            Require(VfsModule::Release(0x77) == Storage::Error::NotFound, "unknown handle was released");
        } });
        cases.push_back({ "device table exhaustion reports NoSpace", [] {
            InitModule();
            DeviceFixture devices[VfsModule::kMaxDevices];
            for (std::uint32_t index = 0; index < VfsModule::kMaxDevices; ++index) {
                FillBlock(devices[index].block, 4096, 512);
                PublishDevice(devices[index], 0x100 + index);
            }
            FakeBlock overflow;
            FillBlock(overflow, 4096, 512);
            BlockDeviceOps ops = FakeOps(overflow);
            Require(VfsModule::Publish(0x900, &ops, &overflow, 0) == Storage::Error::NoSpace, "device table overflow wasn't rejected");
        } });
        cases.push_back({ "SubDevice exposes an aligned window", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 8192, 512);
            PublishDevice(device, 0x10);
            DeviceHandle child = 0;
            Require(VfsModule::CreateSubDevice(0x10, 1024, 2048, 0, &child) == Storage::Error::Ok, "SubDevice failed");
            const BlockDeviceOps* ops = nullptr;
            void* context = nullptr;
            Require(VfsModule::Acquire(child, &ops, &context) == Storage::Error::Ok, "child Acquire failed");
            Require(ops->BlockBytes(context) == 512, "child block size isn't the parent's");
            Require(ops->CapacityBytes(context) == 2048, "child capacity isn't the window length");
            Require(ops->Writable(context) == 0, "child writable doesn't follow the parent");
            std::uint8_t buffer[512] = {};
            Require(ops->Read(context, 0, buffer, 512) == Storage::Error::Ok, "child read failed");
            Require(buffer[0] == device.block.bytes[1024], "child read doesn't map to the parent offset");
            Require(VfsModule::Release(child) == Storage::Error::Ok, "child Release failed");
        } });
        cases.push_back({ "SubDevice rejects misaligned or empty windows", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 8192, 512);
            PublishDevice(device, 0x10);
            DeviceHandle child = 0;
            Require(VfsModule::CreateSubDevice(0x10, 100, 512, 0, &child) == Storage::Error::InvalidArgument, "misaligned begin was accepted");
            Require(VfsModule::CreateSubDevice(0x10, 512, 0, 0, &child) == Storage::Error::InvalidArgument, "zero length was accepted");
            Require(VfsModule::CreateSubDevice(0x10, 512, 8192, 0, &child) == Storage::Error::InvalidArgument, "out-of-range window was accepted");
            Require(VfsModule::CreateSubDevice(0x10, 0xFFFFFFFFFFFFFF00ull, 1024, 0, &child) == Storage::Error::InvalidArgument, "overflowing window was accepted");
            Require(VfsModule::CreateSubDevice(0x10, 512, 512, 1, &child) == Storage::Error::InvalidArgument, "nonzero flags was accepted");
            Require(VfsModule::CreateSubDevice(0x77, 512, 512, 0, &child) == Storage::Error::NotFound, "unknown parent was accepted");
        } });
        cases.push_back({ "SubDevice windows nest", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 8192, 512);
            PublishDevice(device, 0x10);
            DeviceHandle outer = 0;
            DeviceHandle inner = 0;
            Require(VfsModule::CreateSubDevice(0x10, 1024, 4096, 0, &outer) == Storage::Error::Ok, "outer SubDevice failed");
            Require(VfsModule::CreateSubDevice(outer, 1024, 2048, 0, &inner) == Storage::Error::Ok, "nested SubDevice failed");
            Require(VfsModule::CreateSubDevice(inner, 1600, 256, 0, &inner) == Storage::Error::InvalidArgument, "odd offset was accepted");
            const BlockDeviceOps* ops = nullptr;
            void* context = nullptr;
            Require(VfsModule::Acquire(inner, &ops, &context) == Storage::Error::Ok, "nested Acquire failed");
            Require(ops->CapacityBytes(context) == 2048, "nested capacity is wrong");
            std::uint8_t buffer[512] = {};
            Require(ops->Read(context, 512, buffer, 512) == Storage::Error::Ok, "nested read failed");
            Require(buffer[0] == device.block.bytes[1024 + 1024 + 512], "nested read doesn't map through the parents");
        } });
        // ---- Fs.Vfs 登记与挂载 ----
        cases.push_back({ "RegisterFileSystem rejects broken tables", [] {
            InitModule();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(nullptr, nullptr, &id) == Storage::Error::InvalidArgument, "null ops was accepted");
            FileSystemOps ops = FakeFsOps();
            ops.VolumeIdentity = nullptr;
            Require(VfsModule::RegisterFileSystem(&ops, nullptr, &id) == Storage::Error::InvalidArgument, "missing VolumeIdentity was accepted");
        } });
        cases.push_back({ "RegisterFileSystem returns a nonzero id", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            Require(id != 0, "registration id is zero");
            Require(VfsModule::FindFileSystemByFormat("fakefs", &id) && id != 0, "registered format wasn't found");
            Require(!VfsModule::FindFileSystemByFormat("ext4", &id), "unregistered format was found");
        } });
        cases.push_back({ "Mount rejects unknown file systems and devices", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            Require(VfsModule::Mount(id + 10, 0x10, "/", 0) == Storage::Error::NotFound, "unknown file system was mounted");
            Require(VfsModule::Mount(id, 0x77, "/", 0) == Storage::Error::NotFound, "unknown device was mounted");
        } });
        cases.push_back({ "Mount enforces the v1 mount point and mode", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/mnt", 0) == Storage::Error::NotSupported, "non-root mount point was accepted");
            Require(VfsModule::Mount(id, 0x10, "relative", 0) == Storage::Error::InvalidArgument, "relative mount point was accepted");
            Require(VfsModule::Mount(id, 0x10, "/", 1) == Storage::Error::NotSupported, "read-write mount was accepted");
        } });
        cases.push_back({ "Mount holds and Unmount releases the device", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            Require(VfsModule::MountCount() == 1, "mount table isn't one entry");
            Require(VfsModule::ReferenceCount(0x10) == 1, "mount didn't hold the device");
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::InvalidArgument, "duplicate mount was accepted");
            Require(VfsModule::Unmount("/") == Storage::Error::Ok, "unmount failed");
            Require(VfsModule::MountCount() == 0, "mount table isn't empty after unmount");
            Require(VfsModule::ReferenceCount(0x10) == 0, "unmount didn't release the device");
            Require(fileSystem.unmounts == 1, "file system Unmount wasn't called");
        } });
        cases.push_back({ "Mount failure propagates and leaves no state", [] {
            InitModule();
            FakeFs fileSystem;
            fileSystem.mountError = Storage::Error::NotSupported;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::NotSupported, "mount error wasn't propagated");
            Require(VfsModule::MountCount() == 0, "failed mount left an entry");
            Require(VfsModule::ReferenceCount(0x10) == 0, "failed mount leaked a device reference");
        } });
        // ---- 路径解析与文件操作 ----
        cases.push_back({ "Stat resolves paths through the root mount", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            Storage::FileInfo info{};
            Require(VfsModule::Stat("/", &info) == Storage::Error::Ok && info.directory, "root Stat failed");
            Require(VfsModule::Stat("/hello.txt", &info) == Storage::Error::Ok && info.bytes == 5, "file Stat failed");
            Require(std::strcmp(fileSystem.lastPath, "/hello.txt") == 0, "volume path wasn't passed through");
            Require(VfsModule::Stat("/missing", &info) == Storage::Error::NotFound, "missing path wasn't NotFound");
            Require(VfsModule::Stat("/hello.txt/child", &info) == Storage::Error::NotDirectory, "non-directory intermediate wasn't rejected");
        } });
        cases.push_back({ "Stat rejects invalid and overlong paths", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            std::string path;
            Storage::FileInfo info{};
            Require(VfsModule::Stat("relative", &info) == Storage::Error::InvalidArgument, "relative path was accepted");
            Require(VfsModule::Stat("", &info) == Storage::Error::InvalidArgument, "empty path was accepted");
            Require(VfsModule::Stat("/hello.txt", nullptr) == Storage::Error::InvalidArgument, "null info was accepted");
            for (std::uint32_t index = 0; index < 1100; ++index) path.push_back('a');
            path[0] = '/';
            Require(VfsModule::Stat(path.c_str(), &info) == Storage::Error::NameTooLong, "overlong path wasn't NameTooLong");
        } });
        cases.push_back({ "Open and Close manage file references", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            std::uint64_t reference = 0;
            Require(VfsModule::Open("/hello.txt", &reference) == Storage::Error::Ok, "Open failed");
            Require(reference != 0, "file reference is zero");
            Require(VfsModule::Close(reference) == Storage::Error::Ok, "Close failed");
            Require(VfsModule::Close(reference) == Storage::Error::InvalidArgument, "double Close was accepted");
            Require(VfsModule::Close(0) == Storage::Error::InvalidArgument, "zero reference was accepted");
            Require(VfsModule::Open("/missing", &reference) == Storage::Error::NotFound, "missing Open wasn't NotFound");
        } });
        cases.push_back({ "file reference table exhaustion reports NoSpace", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            for (std::uint32_t index = 0; index < VfsModule::kMaxFiles; ++index) {
                std::uint64_t reference = 0;
                Require(VfsModule::Open("/hello.txt", &reference) == Storage::Error::Ok, "Open failed before the limit");
            }
            std::uint64_t overflow = 0;
            Require(VfsModule::Open("/hello.txt", &overflow) == Storage::Error::NoSpace, "reference table overflow wasn't rejected");
        } });
        cases.push_back({ "Read returns file bytes and the EOF boundary", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            std::uint64_t reference = 0;
            Require(VfsModule::Open("/hello.txt", &reference) == Storage::Error::Ok, "Open failed");
            std::uint8_t buffer[16] = {};
            std::uint32_t read = 0;
            Require(VfsModule::Read(reference, 0, buffer, 5, &read) == Storage::Error::Ok && read == 5, "full read failed");
            Require(std::memcmp(buffer, "hello", 5) == 0, "read bytes are wrong");
            Require(VfsModule::Read(reference, 3, buffer, 8, &read) == Storage::Error::Ok && read == 2, "truncated read failed");
            Require(VfsModule::Read(reference, 5, buffer, 8, &read) == Storage::Error::Ok && read == 0, "EOF read isn't zero");
            Require(VfsModule::Read(reference, 0, buffer, 0, &read) == Storage::Error::InvalidArgument, "zero-length read was accepted");
            Require(VfsModule::Read(0, 0, buffer, 4, &read) == Storage::Error::InvalidArgument, "zero reference was read");
        } });
        cases.push_back({ "Read rejects directory objects with IsDirectory", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            std::uint64_t reference = 0;
            Require(VfsModule::Open("/", &reference) == Storage::Error::Ok, "Open of the root directory failed");
            std::uint8_t buffer[16] = {};
            std::uint32_t read = 0;
            Require(VfsModule::Read(reference, 0, buffer, 4, &read) == Storage::Error::IsDirectory, "directory read wasn't IsDirectory");
        } });
        cases.push_back({ "ReadDirectory walks entries and reports the end", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            std::uint64_t reference = 0;
            Require(VfsModule::Open("/", &reference) == Storage::Error::Ok, "Open of the root directory failed");
            std::uint64_t cursor = 0;
            char name[32] = {};
            std::uint32_t nameLength = 0;
            Storage::FileInfo info{};
            Require(VfsModule::ReadDirectory(reference, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::Ok, "first entry failed");
            Require(nameLength == 9 && std::strcmp(name, "hello.txt") == 0 && !info.directory, "first entry is wrong");
            Require(VfsModule::ReadDirectory(reference, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::Ok, "second entry failed");
            Require(nameLength == 6 && std::strcmp(name, "System") == 0 && info.directory, "second entry is wrong");
            Require(VfsModule::ReadDirectory(reference, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::NotFound, "end of directory wasn't NotFound");
        } });
        cases.push_back({ "ReadDirectory keeps the cursor when the name doesn't fit", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            std::uint64_t reference = 0;
            Require(VfsModule::Open("/", &reference) == Storage::Error::Ok, "Open of the root directory failed");
            std::uint64_t cursor = 0;
            char name[4] = {};
            std::uint32_t nameLength = 0;
            Storage::FileInfo info{};
            Require(VfsModule::ReadDirectory(reference, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::NameTooLong, "short buffer wasn't NameTooLong");
            Require(cursor == 0, "failed listing advanced the cursor");
            char wide[32] = {};
            Require(VfsModule::ReadDirectory(reference, &cursor, wide, sizeof(wide), &nameLength, &info) == Storage::Error::Ok, "retry after the short buffer failed");
            Require(cursor == 1, "retry didn't return the first entry");
        } });
        cases.push_back({ "Unmount is refused while a file reference is open", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            std::uint64_t reference = 0;
            Require(VfsModule::Open("/hello.txt", &reference) == Storage::Error::Ok, "Open failed");
            Require(VfsModule::Unmount("/") == Storage::Error::InvalidArgument, "Unmount with an open reference was accepted");
            Require(VfsModule::Close(reference) == Storage::Error::Ok, "Close failed");
            Require(VfsModule::Unmount("/") == Storage::Error::Ok, "Unmount after Close failed");
            Storage::FileInfo info{};
            Require(VfsModule::Stat("/", &info) == Storage::Error::InvalidArgument, "Stat succeeded after unmount");
        } });
        cases.push_back({ "file system errors are passed through", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::Mount(id, 0x10, "/", 0) == Storage::Error::Ok, "mount failed");
            Storage::FileInfo info{};
            fileSystem.statError = Storage::Error::Corrupt;
            Require(VfsModule::Stat("/hello.txt", &info) == Storage::Error::Corrupt, "Corrupt wasn't passed through");
            fileSystem.statError = Storage::Error::Io;
            Require(VfsModule::Stat("/hello.txt", &info) == Storage::Error::Io, "Io wasn't passed through");
        } });
        // ---- 挂载点最长匹配 ----
        cases.push_back({ "longest mount point match prefers the deeper mount", [] {
            InitModule();
            FakeFs rootFs;
            FakeFs nestedFs;
            const FileSystemOps rootOps = FakeFsOps();
            const FileSystemOps nestedOps = FakeFsOps();
            FileSystemId rootId = 0;
            FileSystemId nestedId = 0;
            Require(VfsModule::RegisterFileSystem(&rootOps, &rootFs, &rootId) == Storage::Error::Ok, "root registration failed");
            Require(VfsModule::RegisterFileSystem(&nestedOps, &nestedFs, &nestedId) == Storage::Error::Ok, "nested registration failed");
            DeviceFixture rootDevice;
            DeviceFixture nestedDevice;
            FillBlock(rootDevice.block, 4096, 512);
            FillBlock(nestedDevice.block, 4096, 512);
            PublishDevice(rootDevice, 0x10);
            PublishDevice(nestedDevice, 0x11);
            Require(VfsModule::MountAt(rootId, 0x10, "/", 0) == Storage::Error::Ok, "root mount failed");
            Require(VfsModule::MountAt(nestedId, 0x11, "/mnt", 0) == Storage::Error::Ok, "nested mount failed");
            Storage::FileInfo info{};
            Require(VfsModule::Stat("/hello.txt", &info) == Storage::Error::Ok, "root path failed");
            Require(std::strcmp(rootFs.lastPath, "/hello.txt") == 0, "root file system got the wrong volume path");
            Require(VfsModule::Stat("/mnt/hello.txt", &info) == Storage::Error::Ok, "nested path resolved to the wrong tree");
            Require(std::strcmp(nestedFs.lastPath, "/hello.txt") == 0, "nested file system got the wrong volume path");
            Require(VfsModule::Stat("/mntx", &info) == Storage::Error::NotFound, "string prefix matched without a component boundary");
            Require(std::strcmp(rootFs.lastPath, "/mntx") == 0, "shallow mount didn't take the string-prefix path");
        } });
        cases.push_back({ "mount point itself maps to the volume root", [] {
            InitModule();
            FakeFs rootFs;
            FakeFs nestedFs;
            const FileSystemOps rootOps = FakeFsOps();
            const FileSystemOps nestedOps = FakeFsOps();
            FileSystemId rootId = 0;
            FileSystemId nestedId = 0;
            Require(VfsModule::RegisterFileSystem(&rootOps, &rootFs, &rootId) == Storage::Error::Ok, "root registration failed");
            Require(VfsModule::RegisterFileSystem(&nestedOps, &nestedFs, &nestedId) == Storage::Error::Ok, "nested registration failed");
            DeviceFixture rootDevice;
            DeviceFixture nestedDevice;
            FillBlock(rootDevice.block, 4096, 512);
            FillBlock(nestedDevice.block, 4096, 512);
            PublishDevice(rootDevice, 0x10);
            PublishDevice(nestedDevice, 0x11);
            Require(VfsModule::MountAt(rootId, 0x10, "/", 0) == Storage::Error::Ok, "root mount failed");
            Require(VfsModule::MountAt(nestedId, 0x11, "/mnt", 0) == Storage::Error::Ok, "nested mount failed");
            Storage::FileInfo info{};
            Require(VfsModule::Stat("/mnt", &info) == Storage::Error::Ok, "mount point itself wasn't resolved");
            Require(std::strcmp(nestedFs.lastPath, "/") == 0, "mount point didn't map to the volume root");
        } });
        cases.push_back({ "generic mount rejects the same device twice", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            Require(VfsModule::MountAt(id, 0x10, "/", 0) == Storage::Error::Ok, "root mount failed");
            Require(VfsModule::MountAt(id, 0x10, "/mnt", 0) == Storage::Error::InvalidArgument, "same device mounted twice");
        } });
        // ---- Boot.Volume ----
        cases.push_back({ "MountSystemVolume validates its inputs", [] {
            InitModule();
            Require(VfsModule::MountSystemVolume(nullptr, nullptr) == Storage::Error::InvalidArgument, "null facts was accepted");
            SystemVolumeFacts facts{};
            _Contract_Import_CoreBoot_v1.Info = nullptr;
            Require(VfsModule::MountSystemVolume(nullptr, &facts) == Storage::Error::InvalidArgument, "null BootInfo was accepted");
            InitModule();
            g_bootInfo.volumeUuid[0] = 0x11;
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            Require(VfsModule::MountSystemVolume(&g_bootInfo, nullptr) == Storage::Error::InvalidArgument, "null facts was accepted");
        } });
        cases.push_back({ "MountSystemVolume rejects an unverifiable identity", [] {
            InitModule();
            SystemVolumeFacts facts{};
            g_bootInfo = Boot::Info{};
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "zero uuid was accepted");
            g_bootInfo.volumeUuid[0] = 0x11;
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "zero volume range was accepted");
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            g_bootInfo.mediumBytes = 0x100;
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "range beyond the medium was accepted");
        } });
        cases.push_back({ "MountSystemVolume fails without ext4 or devices", [] {
            InitModule();
            SystemVolumeFacts facts{};
            g_bootInfo = Boot::Info{};
            g_bootInfo.volumeUuid[0] = 0x11;
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "no devices was accepted");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            MarkVolumeStart(device.block, 0x200);
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "missing ext4 was accepted");
        } });
        cases.push_back({ "MountSystemVolume needs a registered ext4 before probing candidates", [] {
            InitModule();
            FakeFs fileSystem;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            MarkVolumeStart(device.block, 0x200);
            SystemVolumeFacts facts{};
            g_bootInfo = Boot::Info{};
            g_bootInfo.volumeUuid[0] = 0x11;
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            g_bootInfo.mediumBytes = 0;
            // 只登记了 fakefs、缺 ext4：在探测候选之前就返回 NotFound
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "missing ext4 was accepted");
        } });
        // 候选跳过：未对齐与越界候选必须被探测循环跳过，不读盘且不参与匹配，最终只挂载合法候选
        cases.push_back({ "MountSystemVolume skips misaligned and oversized candidates and mounts the valid one", [] {
            InitModule();
            FakeFs fileSystem;
            fileSystem.format = "ext4";
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            // 未对齐候选：探测时报告块大小 4096（切窗口时 512），卷区段起点 0x400 不是 4096 的整数倍 → 跳过
            DeviceFixture misaligned;
            FillBlock(misaligned.block, 0x2000, 512);
            misaligned.block.blockBytesSequence[1] = 4096;
            MarkVolumeStart(misaligned.block, 0x400);
            PublishDevice(misaligned, 0x10);
            // 越界候选：探测时报告容量 0x400（切窗口时用实际容量），卷区段起点已到容量边界 → 跳过
            DeviceFixture oversized;
            FillBlock(oversized.block, 0x2000, 512);
            oversized.block.capacitySequence[0] = 0x400;
            MarkVolumeStart(oversized.block, 0x400);
            PublishDevice(oversized, 0x11);
            // 合法候选：块大小 512、容量 0x2000，区段 0x400..0x800 落在容量内且区段起点有 FAKEFS 魔数
            DeviceFixture valid;
            FillBlock(valid.block, 0x2000, 512);
            MarkVolumeStart(valid.block, 0x400);
            PublishDevice(valid, 0x12);
            g_bootInfo = Boot::Info{};
            std::memcpy(g_bootInfo.volumeUuid, fileSystem.uuid, 16);
            g_bootInfo.volumeOffset = 0x400;
            g_bootInfo.volumeBytes = 0x400;
            SystemVolumeFacts facts{};
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::Ok, "the valid candidate wasn't mounted");
            Require(facts.bytes == sizeof(SystemVolumeFacts), "facts length is wrong");
            Require(std::memcmp(facts.volumeUuid, fileSystem.uuid, 16) == 0, "facts uuid is wrong");
            Require(g_log.lines == 1 && std::strcmp(g_log.tag, "OK") == 0, "success marker wasn't logged");
            Require(VfsModule::MountCount() == 1, "successful mount isn't in the mount table");
            // 被跳过的候选不应发生读盘（跳过发生在切窗口与挂载之前），合法候选必须被实际探测过
            Require(misaligned.block.reads == 0, "the misaligned candidate was probed");
            Require(oversized.block.reads == 0, "the oversized candidate was probed");
            Require(valid.block.reads > 0, "the valid candidate wasn't probed");
            Require(VfsModule::Unmount("/") == Storage::Error::Ok, "unmounting the system volume failed");
            Require(VfsModule::MountCount() == 0, "mount table isn't empty after unmount");
        } });
        cases.push_back({ "MountSystemVolume rejects an identity mismatch", [] {
            InitModule();
            FakeFs fileSystem;
            fileSystem.format = "ext4";
            fileSystem.uuid[0] = 0xAA;
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            MarkVolumeStart(device.block, 0x200);
            SystemVolumeFacts facts{};
            g_bootInfo = Boot::Info{};
            g_bootInfo.volumeUuid[0] = 0x11;
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "identity mismatch was accepted");
            Require(VfsModule::MountCount() == 0, "failed mount left an entry");
            Require(VfsModule::ReferenceCount(0x10) == 0, "failed mount leaked a reference");
        } });
        cases.push_back({ "MountSystemVolume mounts the unique candidate", [] {
            InitModule();
            FakeFs fileSystem;
            fileSystem.format = "ext4";
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            MarkVolumeStart(device.block, 0x200);
            SystemVolumeFacts facts{};
            g_bootInfo = Boot::Info{};
            std::memcpy(g_bootInfo.volumeUuid, fileSystem.uuid, 16);
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            for (std::uint32_t index = 0; index < 32; ++index) g_bootInfo.kernelSetBuildId[index] = static_cast<std::uint8_t>(index + 1);
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::Ok, "unique candidate wasn't mounted");
            Require(facts.bytes == sizeof(SystemVolumeFacts), "facts length is wrong");
            Require(std::memcmp(facts.volumeUuid, fileSystem.uuid, 16) == 0, "facts uuid is wrong");
            Require(std::memcmp(facts.buildId, g_bootInfo.kernelSetBuildId, 32) == 0, "facts buildId is wrong");
            Require(g_log.lines == 1 && std::strcmp(g_log.tag, "OK") == 0, "success marker wasn't logged");
            Require(std::strncmp(g_log.text, "system volume mounted: uuid=", 28) == 0, "success marker text is wrong");
            Require(VfsModule::MountCount() == 1, "successful mount isn't in the mount table");
            Require(VfsModule::ReferenceCount(0x10) == 0, "root device should stay unreferenced: the sub-view is mounted");
            Require(VfsModule::Unmount("/") == Storage::Error::Ok, "unmounting the system volume failed");
        } });
        cases.push_back({ "MountSystemVolume rejects ambiguous candidates", [] {
            InitModule();
            FakeFs fileSystem;
            fileSystem.format = "ext4";
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture first;
            DeviceFixture second;
            FillBlock(first.block, 4096, 512);
            FillBlock(second.block, 4096, 512);
            PublishDevice(first, 0x10);
            MarkVolumeStart(first.block, 0x200);
            PublishDevice(second, 0x11);
            MarkVolumeStart(second.block, 0x200);
            SystemVolumeFacts facts{};
            g_bootInfo = Boot::Info{};
            std::memcpy(g_bootInfo.volumeUuid, fileSystem.uuid, 16);
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            Require(VfsModule::MountSystemVolume(&g_bootInfo, &facts) == Storage::Error::NotFound, "ambiguous candidates were accepted");
            Require(VfsModule::MountCount() == 0, "ambiguous scan left a mount");
        } });
        cases.push_back({ "MountSystemVolume uses the imported BootInfo when none is given", [] {
            InitModule();
            FakeFs fileSystem;
            fileSystem.format = "ext4";
            const FileSystemOps ops = FakeFsOps();
            FileSystemId id = 0;
            Require(VfsModule::RegisterFileSystem(&ops, &fileSystem, &id) == Storage::Error::Ok, "registration failed");
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            PublishDevice(device, 0x10);
            MarkVolumeStart(device.block, 0x200);
            g_bootInfo = Boot::Info{};
            std::memcpy(g_bootInfo.volumeUuid, fileSystem.uuid, 16);
            g_bootInfo.volumeOffset = 0x200;
            g_bootInfo.volumeBytes = 0x400;
            SystemVolumeFacts facts{};
            Require(VfsModule::MountSystemVolume(nullptr, &facts) == Storage::Error::Ok, "BootInfo fallback wasn't used");
            Require(std::memcmp(facts.volumeUuid, fileSystem.uuid, 16) == 0, "fallback facts uuid is wrong");
        } });
        cases.push_back({ "Init registers a Started device from Dev.Model bindings", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            device.ops = FakeOps(device.block);
            g_bindings[g_bindingCount++] = HarnessBinding{0x10, &device.ops, &device.block, VfsModule::kBindingStateStarted};
            Require(CallInit() == KernelError::kOk, "Init with a bound device failed");
            Require(VfsModule::DeviceExists(0x10), "bound device wasn't published");
            const BlockDeviceOps* ops = nullptr;
            void* context = nullptr;
            Require(VfsModule::Acquire(0x10, &ops, &context) == Storage::Error::Ok, "published device couldn't be acquired");
            Require(ops == &device.ops && context == &device.block, "published device facts are wrong");
            Require(VfsModule::Release(0x10) == Storage::Error::Ok, "release failed");
        } });
        cases.push_back({ "Init skips unbound devices", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            device.ops = FakeOps(device.block);
            g_bindings[g_bindingCount++] = HarnessBinding{0x10, &device.ops, &device.block, 0};
            Require(CallInit() == KernelError::kOk, "unbound device made Init fail");
            Require(!VfsModule::DeviceExists(0x10), "unbound device was published");
        } });
        cases.push_back({ "Init rejects invalid binding facts", [] {
            InitModule();
            FakeBlock block;
            FillBlock(block, 4096, 1000);
            BlockDeviceOps ops = FakeOps(block);
            g_bindings[g_bindingCount++] = HarnessBinding{0x10, &ops, &block, VfsModule::kBindingStateStarted};
            Require(CallInit() == KernelError::kCorrupt, "invalid binding facts were accepted");
            Require(!VfsModule::DeviceExists(0x10), "invalid binding left a device entry");
        } });
        cases.push_back({ "Init rejects missing Dev.Model imports", [] {
            InitModule();
            _Contract_Import_DevModel_v1.Binding = nullptr;
            Require(CallInit() == KernelError::kNotSupported, "missing Dev.Model import was accepted");
        } });
        cases.push_back({ "Dev.Model binding query returns facts", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            device.ops = FakeOps(device.block);
            g_bindings[g_bindingCount++] = HarnessBinding{0x10, &device.ops, &device.block, VfsModule::kBindingStateStarted};
            BindingFacts queried{};
            Require(FakeDevModelBinding(0x10, &queried) == KernelError::kOk, "query mode failed");
            Require(queried.bytes == sizeof(BindingFacts), "query mode didn't fill the length");
            Require(queried.state == VfsModule::kBindingStateStarted, "query mode didn't return the started state");
            Require(queried.ops == &device.ops && queried.context == &device.block, "query mode didn't return ops/context");
            BindingFacts nonQuery{};
            nonQuery.bytes = static_cast<std::uint32_t>(sizeof(BindingFacts));
            Require(FakeDevModelBinding(0x10, &nonQuery) == KernelError::kOk, "non-query branch returned an error");
            Require(nonQuery.ops == nullptr, "non-query branch wrongly returned ops");
        } });
        cases.push_back({ "Init rejects a Binding result without facts", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            device.ops = FakeOps(device.block);
            g_bindings[g_bindingCount++] = HarnessBinding{0x10, &device.ops, &device.block, VfsModule::kBindingStateStarted};
            _Contract_Import_DevModel_v1.Binding = &BrokenDevModelBinding;
            Require(CallInit() == KernelError::kCorrupt, "bare kOk without facts was accepted");
            Require(!VfsModule::DeviceExists(0x10), "device without facts was published");
        } });
        cases.push_back({ "Init rejects a Bound device before Start", [] {
            InitModule();
            DeviceFixture device;
            FillBlock(device.block, 4096, 512);
            device.ops = FakeOps(device.block);
            g_bindings[g_bindingCount++] = HarnessBinding{0x10, &device.ops, &device.block, VfsModule::kBindingStateBound};
            Require(CallInit() == KernelError::kCorrupt, "bound-but-not-started device was accepted");
            Require(!VfsModule::DeviceExists(0x10), "bound-but-not-started device was published");
        } });
        return cases;
    }
}

// 入口：跑全部用例并汇总；任一失败返回 1
int main() {
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckVfs: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckVfs: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckVfs: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
