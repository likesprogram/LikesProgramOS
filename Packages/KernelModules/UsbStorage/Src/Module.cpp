/* Module.cpp
    UsbStorage 模块：USB 大容量存储类驱动，逻辑名与版本保持 1.0.0

    经 Usb.Host.RegisterClass(0x08) 登记类驱动；UsbCore 枚举到接口类 0x08/子类 0x06/
    协议 0x50 的设备后回调 Probe/Bind。Bind 读配置描述符找 Bulk IN/OUT 端点，
    走 BOT（CBW/CSW）发 SCSI INQUIRY 与 READ CAPACITY(10) 拿块大小/块数，
    构造模块内静态 BlockDeviceOps（只读、无刷新原语），再用 Dev.Model v2 Register 登记
    USB 设备、Claim 认领、Binding 登记/更新模式写 state=Started(2)+ops/context；
    Vfs Init 之后按绑定事实 Publish，本模块不导入 Block.Registry、不自己 Publish
    无设备时打 `usbstorage: no device` 并成功返回；绑定成功打稳定标识行
*/

#include <ModuleAbi.hpp>

#include <stdint.h>

namespace {
    // 类码：大容量存储 SCSI 透明命令集
    constexpr uint32_t kStorageClassCode = 0x08;
    constexpr uint32_t kStorageSubclassScsi = 0x06;
    constexpr uint32_t kStorageProtocolBot = 0x50;
    // USB 标准请求与描述符类型
    constexpr uint8_t kRequestGetDescriptor = 0x06;
    constexpr uint8_t kDescriptorTypeConfiguration = 0x02;
    constexpr uint8_t kDescriptorTypeInterface = 0x04;
    constexpr uint8_t kDescriptorTypeEndpoint = 0x05;
    constexpr uint8_t kRequestTypeDeviceIn = 0x80;
    // 端点属性：批量传输
    constexpr uint8_t kEndpointAttributeBulk = 2;
    // BOT 签名与状态
    constexpr uint32_t kCbwSignature = 0x43425355u;   // "USBC"
    constexpr uint32_t kCswSignature = 0x53425355u;   // "USBS"
    constexpr uint32_t kCbwBytes = 31;
    constexpr uint32_t kCswBytes = 13;
    constexpr uint8_t kCswStatusOk = 0;
    // SCSI 命令操作码
    constexpr uint8_t kScsiInquiry = 0x12;
    constexpr uint8_t kScsiReadCapacity10 = 0x25;
    constexpr uint8_t kScsiRead10 = 0x28;
    // 单次批量传输上限与 SCSI 单命令最多读取的块数（分包由驱动按短传输推进）
    constexpr uint32_t kMaxTransferBytes = 4096;
    constexpr uint32_t kMaxBlocksPerCommand = 16;
    // INQUIRY 标准数据长度
    constexpr uint32_t kInquiryBytes = 36;
    // 配置描述符读取上限
    constexpr uint32_t kMaxConfigBytes = 256;
    // 绑定状态：Started(2) 表示 ops/context 有效（与 DriverLoader/Dev.Model 约定一致）
    constexpr uint32_t kBindingStateStarted = 2;

    // 模块状态：USB 设备、端点、卷几何与寄存器登记结果
    struct State {
        uint32_t initialized;        // Init 是否完成
        uint32_t bound;              // 是否已绑定设备
        ClassHandle classHandle;     // UsbCore 类句柄
        UsbDeviceHandle usbDevice;   // UsbCore 设备句柄
        uint32_t bulkIn;             // Bulk IN 端点地址
        uint32_t bulkOut;            // Bulk OUT 端点地址
        uint32_t maxPacket;          // Bulk 端点最大包长
        uint32_t blockBytes;         // 逻辑块大小（512 或 4096）
        uint32_t blockCount;         // 块数
        uint64_t capacityBytes;      // 容量（字节）
        uint32_t tag;                // BOT 标签计数
        DeviceHandle modelDevice;    // Dev.Model 分配的块设备句柄
        ClaimHandle claim;           // Dev.Model 认领句柄
    };
    State g_state;

    // 静态传输缓冲：不动态分配；CBW/CSW、配置描述符与 INQUIRY/容量响应
    uint8_t g_cbw[kCbwBytes];
    uint8_t g_csw[kCswBytes];
    uint8_t g_config[kMaxConfigBytes];

    // 小端读写
    uint16_t GetLe16(const uint8_t* field) { return static_cast<uint16_t>(field[0] | (field[1] << 8)); }
    uint32_t GetLe32(const uint8_t* field) {
        return static_cast<uint32_t>(field[0]) | (static_cast<uint32_t>(field[1]) << 8)
            | (static_cast<uint32_t>(field[2]) << 16) | (static_cast<uint32_t>(field[3]) << 24);
    }
    uint32_t GetBe32(const uint8_t* field) {
        return (static_cast<uint32_t>(field[0]) << 24) | (static_cast<uint32_t>(field[1]) << 16)
            | (static_cast<uint32_t>(field[2]) << 8) | static_cast<uint32_t>(field[3]);
    }
    void PutLe32(uint8_t* field, uint32_t value) {
        for (uint32_t index = 0; index < 4; ++index) field[index] = static_cast<uint8_t>(value >> (8 * index));
    }
    void PutBe32(uint8_t* field, uint32_t value) {
        for (uint32_t index = 0; index < 4; ++index) field[index] = static_cast<uint8_t>(value >> (24 - 8 * index));
    }

    // 追加十进制 / 十六进制文本，返回新游标
    uint32_t AppendDecimal(char* text, uint32_t cursor, uint64_t value) {
        char digits[20];
        uint32_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (count > 0) text[cursor++] = digits[--count];
        return cursor;
    }
    uint32_t AppendHex(char* text, uint32_t cursor, uint64_t value) {
        static constexpr char kDigits[] = "0123456789abcdef";
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const uint32_t nibble = static_cast<uint32_t>((value >> shift) & 0x0F);
            if (!started && nibble == 0 && shift != 0) continue;
            started = true;
            text[cursor++] = kDigits[nibble];
        }
        return cursor;
    }
    void AppendText(char* text, uint32_t& cursor, const char* source) {
        for (uint32_t index = 0; source[index] != '\0'; ++index) text[cursor++] = source[index];
    }

    // 输出固定文本行
    void LogFixed(const char* tag, const char* text) { _Contract_Import_CoreLog_v1.Line(tag, text); }

    // 控制传输：设备到主机
    KernelError ControlIn(UsbDeviceHandle device, uint16_t value, uint16_t index, void* buffer, uint32_t bytes, uint32_t* actual) {
        UsbSetupPacket setup{};
        setup.requestType = kRequestTypeDeviceIn;
        setup.request = kRequestGetDescriptor;
        setup.value = value;
        setup.index = index;
        setup.length = static_cast<uint16_t>(bytes);
        return _Contract_Import_UsbHost_v1.ControlTransfer(device, &setup, buffer, bytes, actual);
    }

    // 批量传输：把短传输视作成功推进（控制器可能按最大包长分包）
    KernelError BulkTransferProgress(UsbDeviceHandle device, uint32_t endpoint, void* buffer, uint32_t bytes, uint32_t* actual) {
        if (actual == nullptr) return KernelError::kInvalidArgument;
        uint32_t done = 0;
        while (done < bytes) {
            const uint32_t request = (bytes - done) < kMaxTransferBytes ? (bytes - done) : kMaxTransferBytes;
            uint32_t step = 0;
            const KernelError error = _Contract_Import_UsbHost_v1.BulkTransfer(device, endpoint,
                static_cast<uint8_t*>(buffer) + done, request, &step);
            if (error != KernelError::kOk) return error;
            if (step == 0 || step > request) return KernelError::kIo;
            done += step;
        }
        *actual = done;
        return KernelError::kOk;
    }

    // 构造并发送 CBW，返回本命令的 BOT 标签
    KernelError SendCbw(uint32_t dataBytes, bool directionIn, const uint8_t* command, uint8_t commandBytes, uint32_t& tag) {
        PutLe32(g_cbw + 0, kCbwSignature);
        tag = ++g_state.tag;
        PutLe32(g_cbw + 4, tag);
        PutLe32(g_cbw + 8, dataBytes);
        g_cbw[12] = directionIn ? 0x80 : 0x00;
        g_cbw[13] = 0;
        g_cbw[14] = commandBytes;
        for (uint32_t index = 0; index < kCbwBytes; ++index) {
            if (index >= 15 && index - 15 < commandBytes) g_cbw[index] = command[index - 15];
            else if (index >= 15) g_cbw[index] = 0;
        }
        uint32_t actual = 0;
        const KernelError error = BulkTransferProgress(g_state.usbDevice, g_state.bulkOut, g_cbw, kCbwBytes, &actual);
        if (error != KernelError::kOk) return error;
        return actual == kCbwBytes ? KernelError::kOk : KernelError::kIo;
    }

    // 接收并校验 CSW：签名、标签与状态必须成立，残留字节数不得超过数据阶段长度
    KernelError ReceiveCsw(uint32_t tag, uint32_t dataBytes) {
        uint32_t actual = 0;
        const KernelError error = BulkTransferProgress(g_state.usbDevice, g_state.bulkIn, g_csw, kCswBytes, &actual);
        if (error != KernelError::kOk) return error;
        if (actual != kCswBytes) return KernelError::kIo;
        if (GetLe32(g_csw + 0) != kCswSignature) return KernelError::kCorrupt;
        if (GetLe32(g_csw + 4) != tag) return KernelError::kCorrupt;
        const uint32_t residue = GetLe32(g_csw + 8);
        if (residue > dataBytes) return KernelError::kCorrupt;
        if (g_csw[12] != kCswStatusOk) return KernelError::kIo;
        return KernelError::kOk;
    }

    // 执行一条 SCSI 命令：CBW → 可选数据阶段 → CSW
    KernelError ExecuteScsi(const uint8_t* command, uint8_t commandBytes, void* data, uint32_t dataBytes, bool dataIn) {
        uint32_t tag = 0;
        KernelError error = SendCbw(dataBytes, dataIn, command, commandBytes, tag);
        if (error != KernelError::kOk) return error;
        if (dataBytes > 0 && data != nullptr) {
            uint32_t actual = 0;
            error = BulkTransferProgress(g_state.usbDevice, dataIn ? g_state.bulkIn : g_state.bulkOut, data, dataBytes, &actual);
            if (error != KernelError::kOk) return error;
            if (actual != dataBytes) return KernelError::kIo;
        }
        return ReceiveCsw(tag, dataBytes);
    }

    // SCSI INQUIRY：确认设备响应 BOT 命令集
    KernelError Inquiry() {
        uint8_t command[6] = {kScsiInquiry, 0, 0, 0, static_cast<uint8_t>(kInquiryBytes), 0};
        uint8_t response[kInquiryBytes] = {};
        return ExecuteScsi(command, 6, response, kInquiryBytes, true);
    }

    // SCSI READ CAPACITY(10)：返回块数与逻辑块大小
    KernelError ReadCapacity(uint32_t& blockCount, uint32_t& blockBytes) {
        uint8_t command[10] = {kScsiReadCapacity10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t response[8] = {};
        const KernelError error = ExecuteScsi(command, 10, response, 8, true);
        if (error != KernelError::kOk) return error;
        blockCount = GetBe32(response + 0);
        blockBytes = GetBe32(response + 4);
        if (blockBytes != 512 && blockBytes != 4096) return KernelError::kNotSupported;
        if (blockCount == 0xFFFFFFFFu) return KernelError::kNotSupported;
        ++blockCount;
        return KernelError::kOk;
    }

    // 发 READ(10) 读一段数据；lba 以块为单位，dataBytes 由块数派生
    KernelError ReadBlocks(uint32_t lba, uint32_t blocks, void* data) {
        uint8_t command[10] = {kScsiRead10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        PutBe32(command + 2, lba);
        command[7] = static_cast<uint8_t>(blocks >> 8);
        command[8] = static_cast<uint8_t>(blocks);
        return ExecuteScsi(command, 10, data, blocks * g_state.blockBytes, true);
    }

    // —— BlockDeviceOps 回调：只读块设备，上下文是模块状态 ——

    // 逻辑块大小
    uint32_t DeviceBlockBytes(void* context) { return static_cast<State*>(context)->blockBytes; }

    // 设备容量，字节
    uint64_t DeviceCapacityBytes(void* context) { return static_cast<State*>(context)->capacityBytes; }

    // 只读设备：Writable 恒 0
    uint32_t DeviceWritable(void* context) {
        (void)context;
        return 0;
    }

    // 读 [offset, offset+bytes)：按块对齐、边界与分包校验后逐命令读
    Storage::Error DeviceRead(void* context, uint64_t offset, void* buffer, uint32_t bytes) {
        State& state = *static_cast<State*>(context);
        if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
        if (state.blockBytes == 0) return Storage::Error::InvalidArgument;
        if ((offset % state.blockBytes) != 0 || (bytes % state.blockBytes) != 0) return Storage::Error::InvalidArgument;
        if (offset > state.capacityBytes || bytes > state.capacityBytes - offset) return Storage::Error::InvalidArgument;
        uint8_t* cursor = static_cast<uint8_t*>(buffer);
        uint64_t lba = offset / state.blockBytes;
        uint32_t remaining = bytes / state.blockBytes;
        while (remaining > 0) {
            const uint32_t blocks = remaining < kMaxBlocksPerCommand ? remaining : kMaxBlocksPerCommand;
            const KernelError error = ReadBlocks(static_cast<uint32_t>(lba), blocks, cursor);
            if (error != KernelError::kOk) return Storage::Error::Io;
            cursor += blocks * state.blockBytes;
            lba += blocks;
            remaining -= blocks;
        }
        return Storage::Error::Ok;
    }

    // 只读设备：写返回 ReadOnly
    Storage::Error DeviceWrite(void* context, uint64_t offset, const void* buffer, uint32_t bytes) {
        (void)context;
        (void)offset;
        (void)buffer;
        (void)bytes;
        return Storage::Error::ReadOnly;
    }

    // 无写缓存：刷新返回 Ok
    Storage::Error DeviceFlush(void* context) {
        (void)context;
        return Storage::Error::Ok;
    }

    // 模块内静态块设备调用表：capabilities = 0（没有真实刷新原语）
    const BlockDeviceOps kBlockOps = {0, &DeviceBlockBytes, &DeviceCapacityBytes, &DeviceWritable, &DeviceRead,
        &DeviceWrite, &DeviceFlush};

    // 从配置描述符找 Bulk IN/OUT 端点与最大包长
    KernelError ParseEndpoints() {
        uint32_t actual = 0;
        KernelError error = ControlIn(g_state.usbDevice, static_cast<uint16_t>(kDescriptorTypeConfiguration << 8), 0,
            g_config, kMaxConfigBytes, &actual);
        if (error != KernelError::kOk) return error;
        if (actual < 9 || g_config[1] != kDescriptorTypeConfiguration) return KernelError::kCorrupt;
        const uint32_t totalBytes = GetLe16(g_config + 2);
        const uint32_t limit = totalBytes < actual ? totalBytes : actual;
        uint32_t offset = 0;
        g_state.bulkIn = 0;
        g_state.bulkOut = 0;
        g_state.maxPacket = 0;
        while (offset + 2 <= limit) {
            const uint8_t length = g_config[offset];
            const uint8_t type = g_config[offset + 1];
            if (length < 2 || offset + length > limit) return KernelError::kCorrupt;
            if (type == kDescriptorTypeEndpoint && length >= 7) {
                const uint8_t address = g_config[offset + 2];
                const uint8_t attributes = g_config[offset + 3];
                const uint16_t maxPacket = GetLe16(g_config + offset + 4);
                if ((attributes & 0x03) == kEndpointAttributeBulk && maxPacket != 0) {
                    if ((address & 0x80) != 0) {
                        g_state.bulkIn = address;
                        g_state.maxPacket = maxPacket;
                    } else {
                        g_state.bulkOut = address;
                    }
                }
            }
            offset += length;
        }
        if (g_state.bulkIn == 0 || g_state.bulkOut == 0) return KernelError::kNotSupported;
        return KernelError::kOk;
    }

    // 向 Dev.Model 登记 USB 设备、认领并把 Started 绑定事实写回（Vfs Init 会 Publish）
    KernelError RegisterDevice(const UsbDeviceFacts* facts) {
        DeviceFacts device{};
        device.bytes = static_cast<uint32_t>(sizeof(DeviceFacts));
        device.deviceHandle = 0;
        device.busKind = 2;             // USB
        device.classCode = 0x080650u;   // 类 0x08 / 子类 0x06 / 协议 0x50
        device.vendorId = facts->vendorId;
        device.deviceId = facts->productId;
        // 总线地址自选编码：控制器高 32 位、端口次 16 位、UsbCore 设备句柄低 16 位
        device.busAddress = (static_cast<uint64_t>(facts->controllerHandle) << 32)
            | (static_cast<uint64_t>(facts->port & 0xFFFF) << 16) | static_cast<uint64_t>(facts->deviceHandle & 0xFFFF);
        DeviceHandle modelDevice = 0;
        KernelError error = _Contract_Import_DevModel_v2.Register(&device, &modelDevice);
        if (error != KernelError::kOk) return error;
        ClaimHandle claim = 0;
        error = _Contract_Import_DevModel_v2.Claim(modelDevice, 0, &claim);
        if (error != KernelError::kOk) return error;
        BindingFacts binding{};
        binding.bytes = static_cast<uint32_t>(sizeof(BindingFacts));
        binding.bindHandle = claim;
        binding.deviceHandle = modelDevice;
        binding.driverModule = kModUsbStorage;
        binding.state = kBindingStateStarted;
        binding.ops = &kBlockOps;
        binding.context = &g_state;
        error = _Contract_Import_DevModel_v2.Binding(modelDevice, &binding);
        if (error != KernelError::kOk) {
            _Contract_Import_DevModel_v2.Release(claim);
            return error;
        }
        g_state.modelDevice = modelDevice;
        g_state.claim = claim;
        return KernelError::kOk;
    }

    // 撤回 Dev.Model 绑定与认领（Abort / Unbind 共用）
    void ReleaseDevice() {
        if (g_state.modelDevice == 0) return;
        BindingFacts binding{};
        binding.bytes = static_cast<uint32_t>(sizeof(BindingFacts));
        binding.deviceHandle = g_state.modelDevice;
        binding.state = 0;
        _Contract_Import_DevModel_v2.Binding(g_state.modelDevice, &binding);
        if (g_state.claim != 0) _Contract_Import_DevModel_v2.Release(g_state.claim);
        g_state.modelDevice = 0;
        g_state.claim = 0;
    }

    // 类驱动 Probe：类 0x08 / 子类 SCSI / 协议 BOT 的接口由本驱动接管
    KernelError StorageProbe(void* context, const UsbDeviceFacts* facts, uint32_t* claimed) {
        (void)context;
        if (facts == nullptr || claimed == nullptr) return KernelError::kInvalidArgument;
        *claimed = 0;
        if (facts->classCode != kStorageClassCode) return KernelError::kOk;
        if (facts->subclass != kStorageSubclassScsi || facts->protocol != kStorageProtocolBot) return KernelError::kOk;
        *claimed = 1;
        return KernelError::kOk;
    }

    // 类驱动 Bind：端点解析 → BOT 探测 → 静态块设备表 → Dev.Model 登记/认领/绑定
    KernelError StorageBind(void* context, const UsbDeviceFacts* facts, uint32_t classHandle) {
        (void)context;
        if (facts == nullptr || facts->deviceHandle == 0) return KernelError::kInvalidArgument;
        g_state.classHandle = classHandle;
        g_state.usbDevice = facts->deviceHandle;
        KernelError error = ParseEndpoints();
        if (error != KernelError::kOk) return error;
        error = Inquiry();
        if (error != KernelError::kOk) return error;
        uint32_t blockCount = 0;
        uint32_t blockBytes = 0;
        error = ReadCapacity(blockCount, blockBytes);
        if (error != KernelError::kOk) return error;
        if (blockCount == 0) return KernelError::kCorrupt;
        const uint64_t capacity = static_cast<uint64_t>(blockCount) * blockBytes;
        if (capacity / blockBytes != blockCount) return KernelError::kCorrupt;
        g_state.blockBytes = blockBytes;
        g_state.blockCount = blockCount;
        g_state.capacityBytes = capacity;
        error = RegisterDevice(facts);
        if (error != KernelError::kOk) return error;
        g_state.bound = 1;
        char text[128];
        uint32_t cursor = 0;
        AppendText(text, cursor, "usbstorage: device bound block=0x");
        cursor = AppendHex(text, cursor, blockBytes);
        AppendText(text, cursor, " sectors=");
        cursor = AppendDecimal(text, cursor, blockCount);
        text[cursor] = '\0';
        LogFixed("OK", text);
        return KernelError::kOk;
    }

    // 类驱动 Unbind：撤回绑定事实与认领
    void StorageUnbind(void* context, uint32_t deviceHandle) {
        (void)context;
        if (g_state.bound == 0 || g_state.usbDevice != deviceHandle) return;
        ReleaseDevice();
        g_state = State{};
        LogFixed("INFO", "usbstorage: device unbound");
    }

    // 类驱动回调表
    const UsbClassOps kStorageClassOps = {&StorageProbe, &StorageBind, &StorageUnbind};
}

// 导入回填区：Usb.Host 前 4 槽、Dev.Model v2 七槽、Core.Log / Core.Mem / Core.Page
LP_CONTRACT_IMPORT(UsbHost, 1, UsbHostSlotsV1) = {};
LP_CONTRACT_IMPORT(DevModel, 2, DevModelSlotsV2) = {};
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMem, 1, CoreMemSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePage, 1, CorePageSlotsV1) = {};

// 契约声明：不导出任何接口（类驱动经 Usb.Host.RegisterClass 注册，块设备走 Dev.Model 绑定）
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < static_cast<uint32_t>(sizeof(KernelAbiContext))) return KernelError::kInvalidArgument;
    if (!(context->moduleId == kModUsbStorage)) return KernelError::kInvalidArgument;
    if (_Contract_Import_UsbHost_v1.RegisterClass == nullptr || _Contract_Import_UsbHost_v1.ControlTransfer == nullptr
        || _Contract_Import_UsbHost_v1.BulkTransfer == nullptr
        || _Contract_Import_DevModel_v2.Register == nullptr || _Contract_Import_DevModel_v2.Claim == nullptr
        || _Contract_Import_DevModel_v2.Binding == nullptr || _Contract_Import_DevModel_v2.Release == nullptr
        || _Contract_Import_CoreLog_v1.Line == nullptr) {
        return KernelError::kNotSupported;
    }
    g_state = State{};
    ClassHandle classHandle = 0;
    const KernelError error = _Contract_Import_UsbHost_v1.RegisterClass(kStorageClassCode, &kStorageClassOps, &g_state, &classHandle);
    if (error != KernelError::kOk) return error;
    g_state.classHandle = classHandle;
    g_state.initialized = 1;
    // RegisterClass 会对已枚举未绑定设备立即重扫；没有设备时打固定行并成功返回
    if (g_state.bound == 0) LogFixed("INFO", "usbstorage: no device");
    return KernelError::kOk;
}

// Abort：撤回 Dev.Model 绑定/认领并清状态
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    ReleaseDevice();
    g_state = State{};
    return KernelError::kOk;
}
