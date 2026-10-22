/* main.cpp
    CheckUsbStorage：UsbStorage 模块（USB 大容量存储 BOT/SCSI）的宿主侧检查

    复用 Packages/KernelModules/UsbStorage/Src/Module.cpp 的目标侧源码，不复制实现；
    宿主替身注入伪 Usb.Host（ControlTransfer/BulkTransfer/RegisterClass）、伪 Dev.Model v2
    （Register/Claim/Binding/Release）与伪 Core.* 导入槽；伪 BOT 设备按 CBW/CSW 字节表
    回应 INQUIRY / READ CAPACITY(10) / READ(10)，可配置 512/4096 块、CSW 状态与短传输
    每个用例打印 [ OK ] 或 [ FAIL ]，最后一行打印计数；失败返回 1
*/

#include <ModuleAbi.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // 用例总数
    constexpr unsigned kCaseCount = 15;
    // 已跑 / 失败计数
    unsigned g_ran = 0;
    unsigned g_failed = 0;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 小端 / 大端读写
    void PutLe16(std::uint8_t* field, std::uint16_t value) { field[0] = static_cast<std::uint8_t>(value); field[1] = static_cast<std::uint8_t>(value >> 8); }
    std::uint32_t Le32(const std::uint8_t* field) { return static_cast<std::uint32_t>(field[0]) | (static_cast<std::uint32_t>(field[1]) << 8) | (static_cast<std::uint32_t>(field[2]) << 16) | (static_cast<std::uint32_t>(field[3]) << 24); }
    void PutLe32(std::uint8_t* field, std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) field[i] = static_cast<std::uint8_t>(value >> (8 * i)); }
    std::uint32_t Be32(const std::uint8_t* field) { return (static_cast<std::uint32_t>(field[0]) << 24) | (static_cast<std::uint32_t>(field[1]) << 16) | (static_cast<std::uint32_t>(field[2]) << 8) | field[3]; }
    void PutBe32(std::uint8_t* field, std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) field[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i)); }

    // 伪 BOT 设备状态：块几何、命令计数与最近一次 CBW
    struct FakeBot {
        std::uint32_t blockBytes = 512;   // 块大小
        std::uint32_t blockCount = 2048;  // 块数
        std::uint8_t cswStatus = 0;       // CSW 状态：0 成功、1 失败
        bool badCswSignature = false;     // CSW 签名破坏
        unsigned cbwCount = 0;            // 收到的 CBW 数
        unsigned readCommands = 0;        // READ(10) 数
        std::uint8_t lastCbw[31] = {};    // 最近一次 CBW
        std::uint32_t lastLba = 0;        // 最近一次 READ(10) 的 LBA
        std::uint16_t lastBlocks = 0;     // 最近一次 READ(10) 的块数
        bool pendingData = false;         // 是否有待读的数据阶段
        std::uint32_t producedBytes = 0;   // 数据阶段已产出字节数
        bool dataIn = true;               // 数据阶段方向
        std::uint32_t pendingTag = 0;     // 当前命令标签
        std::uint32_t pendingBytes = 0;   // 数据阶段字节数
        std::uint32_t pendingLba = 0;     // 数据阶段起始 LBA
        std::uint32_t pendingBlocks = 0;  // 数据阶段块数
    };
    FakeBot g_bot;

    // 伪盘内容：按 LBA/字节产生可核对图案
    std::uint8_t DiskByte(std::uint32_t lba, std::uint32_t offset) {
        return static_cast<std::uint8_t>((lba * 31 + offset * 7 + 5) & 0xFF);
    }

    // 伪 Usb.Host 捕获的类驱动登记
    struct FakeHost {
        std::uint32_t classCode = 0;         // 登记的接口类码
        const UsbClassOps* classOps = nullptr; // 类驱动回调表
        void* classContext = nullptr;        // 类驱动上下文
        ClassHandle classHandle = 7;         // 返回的类句柄
    };
    FakeHost g_host;

    // 伪 Dev.Model v2 状态
    struct FakeModel {
        DeviceFacts facts = {};      // 登记的设备事实
        DeviceHandle handle = 100;   // 分配的块设备句柄
        ClaimHandle claim = 200;     // 认领句柄
        BindingFacts binding = {};   // 最近一次绑定事实
        bool registered = false;     // 是否已登记
        bool claimed = false;        // 是否已认领
        bool bound = false;          // 是否已绑定
        bool released = false;       // 是否已 Release
        KernelError bindingResult = KernelError::kOk; // Binding 返回值注入
    };
    FakeModel g_model;

    // 伪日志
    struct FakeLog {
        char tag[16] = {};
        char text[256] = {};
        unsigned lines = 0;
    };
    FakeLog g_log;

    // 伪 Core.Log：记录一行
    void LogLine(const char* tag, const char* text) {
        ++g_log.lines;
        std::strncpy(g_log.tag, tag != nullptr ? tag : "", sizeof(g_log.tag) - 1);
        std::strncpy(g_log.text, text != nullptr ? text : "", sizeof(g_log.text) - 1);
    }
    // 伪 Core.Log：写文本
    void LogWrite(const char* text) { (void)text; }
    // 伪 Core.Mem：清零
    void MemZero(void* destination, std::uint64_t bytes) { if (destination != nullptr && bytes != 0) std::memset(destination, 0, static_cast<std::size_t>(bytes)); }
    // 伪 Core.Mem：拷贝
    void MemCopy(void* destination, const void* source, std::uint64_t bytes) { if (destination != nullptr && source != nullptr && bytes != 0) std::memcpy(destination, source, static_cast<std::size_t>(bytes)); }
    // 伪 Core.Mem：比较
    std::int32_t MemCompare(const void* left, const void* right, std::uint64_t bytes) { return (left != nullptr && right != nullptr) ? static_cast<std::int32_t>(std::memcmp(left, right, static_cast<std::size_t>(bytes))) : 0; }
    // 伪 Core.Page：页大小
    std::uint32_t PageBytes() { return 4096; }

    // 伪 Usb.Host：RegisterClass 捕获类驱动
    KernelError HostRegisterClass(std::uint32_t classCode, const UsbClassOps* ops, void* context, ClassHandle* handle) {
        if (ops == nullptr || handle == nullptr) return KernelError::kInvalidArgument;
        g_host.classCode = classCode;
        g_host.classOps = ops;
        g_host.classContext = context;
        *handle = g_host.classHandle;
        return KernelError::kOk;
    }

    // 伪 Usb.Host：控制传输（只实现 GET_DESCRIPTOR 配置描述符）
    KernelError HostControlTransfer(UsbDeviceHandle device, const UsbSetupPacket* packet, void* buffer, std::uint32_t bytes, std::uint32_t* actual) {
        (void)device;
        if (packet == nullptr || buffer == nullptr || actual == nullptr) return KernelError::kInvalidArgument;
        *actual = 0;
        if (packet->requestType != 0x80 || packet->request != 0x06 || packet->value != 0x0200) return KernelError::kNotSupported;
        std::uint8_t* out = static_cast<std::uint8_t*>(buffer);
        const std::uint32_t configBytes = 9 + 9 + 7 + 7;
        if (bytes < configBytes) return KernelError::kInvalidArgument;
        std::memset(out, 0, configBytes);
        out[0] = 9;          // bLength
        out[1] = 2;          // bDescriptorType = configuration
        PutLe16(out + 2, static_cast<std::uint16_t>(configBytes));
        out[4] = 1;          // bNumInterfaces
        out[5] = 1;          // bConfigurationValue
        // 接口描述符：类 0x08 / 子类 0x06 / 协议 0x50
        std::uint8_t* iface = out + 9;
        iface[0] = 9;
        iface[1] = 4;
        iface[2] = 0;        // bInterfaceNumber
        iface[4] = 2;        // bNumEndpoints
        iface[5] = 0x08;
        iface[6] = 0x06;
        iface[7] = 0x50;
        // Bulk IN 端点 0x81
        std::uint8_t* in = iface + 9;
        in[0] = 7;
        in[1] = 5;
        in[2] = 0x81;
        in[3] = 2;           // bulk
        PutLe16(in + 4, 512);
        // Bulk OUT 端点 0x02
        std::uint8_t* outEp = in + 7;
        outEp[0] = 7;
        outEp[1] = 5;
        outEp[2] = 0x02;
        outEp[3] = 2;
        PutLe16(outEp + 4, 512);
        *actual = configBytes;
        return KernelError::kOk;
    }

    // 伪 Usb.Host：批量传输，按 BOT 状态机处理 CBW / 数据阶段 / CSW
    KernelError HostBulkTransfer(UsbDeviceHandle device, std::uint32_t endpoint, void* buffer, std::uint32_t bytes, std::uint32_t* actual) {
        (void)device;
        if (buffer == nullptr || actual == nullptr) return KernelError::kInvalidArgument;
        *actual = 0;
        if (bytes == 31) {
            // CBW：记录并解析命令，准备数据阶段
            std::uint8_t* cbw = static_cast<std::uint8_t*>(buffer);
            if (Le32(cbw + 0) != 0x43425355u || cbw[14] == 0) return KernelError::kIo;
            ++g_bot.cbwCount;
            std::memcpy(g_bot.lastCbw, cbw, 31);
            g_bot.pendingTag = Le32(cbw + 4);
            g_bot.pendingBytes = Le32(cbw + 8);
            g_bot.producedBytes = 0;
            g_bot.dataIn = (cbw[12] & 0x80) != 0;
            const std::uint8_t* command = cbw + 15;
            if (command[0] == 0x12) {
                g_bot.pendingLba = 0;
                g_bot.pendingBlocks = 0;
            } else if (command[0] == 0x25) {
                g_bot.pendingLba = 0;
                g_bot.pendingBlocks = 0;
            } else if (command[0] == 0x28) {
                g_bot.pendingLba = Be32(command + 2);
                g_bot.pendingBlocks = static_cast<std::uint16_t>((command[7] << 8) | command[8]);
                ++g_bot.readCommands;
                g_bot.lastLba = g_bot.pendingLba;
                g_bot.lastBlocks = static_cast<std::uint16_t>(g_bot.pendingBlocks);
            } else {
                return KernelError::kIo;
            }
            g_bot.pendingData = true;
            *actual = 31;
            return KernelError::kOk;
        }
        if (g_bot.pendingData && (endpoint & 0x80) != 0) {
            const std::uint8_t* command = g_bot.lastCbw + 15;
            std::uint8_t* out = static_cast<std::uint8_t*>(buffer);
            const std::uint32_t remaining = g_bot.pendingBytes - g_bot.producedBytes;
            std::uint32_t count = bytes < remaining ? bytes : remaining;
            if (command[0] == 0x12) {
                for (std::uint32_t i = 0; i < count; ++i) out[i] = static_cast<std::uint8_t>(0x40 + i);
            } else if (command[0] == 0x25) {
                if (bytes < 8) return KernelError::kInvalidArgument;
                PutBe32(out + 0, g_bot.blockCount - 1);
                PutBe32(out + 4, g_bot.blockBytes);
                count = 8;
            } else {
                const std::uint32_t startIndex = g_bot.producedBytes;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const std::uint32_t absolute = startIndex + i;
                    const std::uint32_t lba = g_bot.pendingLba + absolute / g_bot.blockBytes;
                    const std::uint32_t offset = absolute % g_bot.blockBytes;
                    out[i] = DiskByte(lba, offset);
                }
            }
            g_bot.producedBytes += count;
            if (g_bot.producedBytes >= g_bot.pendingBytes) g_bot.pendingData = false;
            *actual = count;
            return KernelError::kOk;
        }
        if (bytes == 13) {
            // CSW
            std::uint8_t* csw = static_cast<std::uint8_t*>(buffer);
            std::memset(csw, 0, 13);
            PutLe32(csw + 0, g_bot.badCswSignature ? 0x41414141u : 0x53425355u);
            PutLe32(csw + 4, g_bot.pendingTag);
            PutLe32(csw + 8, 0);
            csw[12] = g_bot.cswStatus;
            *actual = 13;
            return KernelError::kOk;
        }
        return KernelError::kIo;
    }


    // 伪 Dev.Model v2：Register
    KernelError ModelRegister(const DeviceFacts* facts, std::uint32_t* handle) {
        if (facts == nullptr || handle == nullptr) return KernelError::kInvalidArgument;
        g_model.facts = *facts;
        g_model.registered = true;
        *handle = g_model.handle;
        return KernelError::kOk;
    }
    // 伪 Dev.Model v2：Claim
    KernelError ModelClaim(std::uint32_t device, std::uint32_t flags, std::uint32_t* claim) {
        (void)flags;
        if (claim == nullptr || device != g_model.handle) return KernelError::kInvalidArgument;
        g_model.claimed = true;
        *claim = g_model.claim;
        return KernelError::kOk;
    }
    // 伪 Dev.Model v2：Binding（查询零初始化；登记模式写事实；清除模式清绑定）
    KernelError ModelBinding(std::uint32_t device, BindingFacts* facts) {
        if (facts == nullptr || device != g_model.handle) return KernelError::kInvalidArgument;
        if (g_model.bindingResult != KernelError::kOk) return g_model.bindingResult;
        if (facts->bytes == 0) return g_model.bound ? KernelError::kOk : KernelError::kNotFound;
        if (facts->state == 0) {
            g_model.bound = false;
            g_model.binding = BindingFacts{};
            return KernelError::kOk;
        }
        g_model.binding = *facts;
        g_model.bound = true;
        return KernelError::kOk;
    }
    // 伪 Dev.Model v2：Release
    KernelError ModelRelease(std::uint32_t claim) {
        if (claim != g_model.claim) return KernelError::kInvalidArgument;
        g_model.claimed = false;
        g_model.released = true;
        return KernelError::kOk;
    }

    // 初始化模块：填导入回填区并调用 _Module_Init；返回 Init 结果
    KernelError InitModule() {
        g_bot = FakeBot{};
        g_host = FakeHost{};
        g_model = FakeModel{};
        g_log = FakeLog{};
        _Contract_Import_UsbHost_v1.AddController = nullptr;
        _Contract_Import_UsbHost_v1.RegisterClass = &HostRegisterClass;
        _Contract_Import_UsbHost_v1.ControlTransfer = &HostControlTransfer;
        _Contract_Import_UsbHost_v1.BulkTransfer = &HostBulkTransfer;
        _Contract_Import_DevModel_v2.Register = &ModelRegister;
        _Contract_Import_DevModel_v2.Claim = &ModelClaim;
        _Contract_Import_DevModel_v2.Binding = &ModelBinding;
        _Contract_Import_DevModel_v2.Release = &ModelRelease;
        _Contract_Import_CoreLog_v1.Line = &LogLine;
        _Contract_Import_CoreLog_v1.Write = &LogWrite;
        _Contract_Import_CoreMem_v1.Zero = &MemZero;
        _Contract_Import_CoreMem_v1.Copy = &MemCopy;
        _Contract_Import_CoreMem_v1.Compare = &MemCompare;
        _Contract_Import_CorePage_v1.PageBytes = &PageBytes;
        KernelAbiContext context{};
        context.bytes = static_cast<std::uint32_t>(sizeof(KernelAbiContext));
        context.version = 1;
        context.moduleId = kModUsbStorage;
        return _Module_Init(&context);
    }

    // 伪 USB 设备事实：类 08/06/50，普通厂商
    UsbDeviceFacts StorageFacts() {
        UsbDeviceFacts facts{};
        facts.bytes = static_cast<std::uint32_t>(sizeof(UsbDeviceFacts));
        facts.deviceHandle = 3;
        facts.controllerHandle = 1;
        facts.classCode = 0x08;
        facts.subclass = 0x06;
        facts.protocol = 0x50;
        facts.vendorId = 0x46F4;
        facts.productId = 0x0001;
        facts.speed = 3;
        facts.port = 2;
        return facts;
    }

    // 触发类驱动 Bind：返回 Bind 结果
    KernelError BindStorage() {
        UsbDeviceFacts facts = StorageFacts();
        return g_host.classOps->Bind(g_host.classContext, &facts, g_host.classHandle);
    }
    // 一条用例
    struct Case { std::string name; std::function<void()> body; };

    // 组装用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        cases.push_back({ "Init without a device logs and succeeds", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(g_host.classCode == 0x08, "class 0x08 wasn't registered");
            Require(std::strcmp(g_log.text, "usbstorage: no device") == 0, "no-device marker is missing");
        } });
        cases.push_back({ "Probe claims only the BOT interface", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            std::uint32_t claimed = 0;
            UsbDeviceFacts facts = StorageFacts();
            Require(g_host.classOps->Probe(g_host.classContext, &facts, &claimed) == KernelError::kOk && claimed == 1, "BOT interface wasn't claimed");
            facts.protocol = 0x62;
            Require(g_host.classOps->Probe(g_host.classContext, &facts, &claimed) == KernelError::kOk && claimed == 0, "non-BOT protocol was claimed");
        } });
        cases.push_back({ "Bind probes the device and registers the model", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            Require(g_model.registered && g_model.claimed && g_model.bound, "Dev.Model registration/claim/binding is incomplete");
            Require(g_model.facts.busKind == 2 && g_model.facts.classCode == 0x080650, "registered DeviceFacts are wrong");
            Require(g_model.binding.state == 2 && g_model.binding.ops != nullptr && g_model.binding.context != nullptr, "binding facts don't carry ops/context");
            Require(g_model.binding.driverModule == kModUsbStorage, "binding module id is wrong");
            Require(std::strncmp(g_log.text, "usbstorage: device bound block=0x200", 35) == 0, "bound marker is wrong");
        } });
        cases.push_back({ "INQUIRY CBW encoding is correct", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            Require(Le32(g_bot.lastCbw + 0) == 0x43425355u, "CBW signature is wrong");
            Require((g_bot.lastCbw[12] & 0x80) != 0, "INQUIRY direction isn't IN");
            Require(g_bot.lastCbw[14] == 10, "last command isn't a 10-byte SCSI command");
        } });
        cases.push_back({ "READ CAPACITY(10) yields 512-byte geometry", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_bot.blockBytes = 512;
            g_bot.blockCount = 2048;
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            const BlockDeviceOps* ops = g_model.binding.ops;
            Require(ops != nullptr && ops->BlockBytes(g_model.binding.context) == 512, "block size isn't 512");
            Require(ops->CapacityBytes(g_model.binding.context) == 2048ull * 512ull, "capacity is wrong");
            Require(std::strncmp(g_log.text, "usbstorage: device bound block=0x200 sectors=2048", 40) == 0, "marker geometry is wrong");
        } });
        cases.push_back({ "READ CAPACITY(10) yields 4096-byte geometry", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_bot.blockBytes = 4096;
            g_bot.blockCount = 1024;
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            const BlockDeviceOps* ops = g_model.binding.ops;
            Require(ops->BlockBytes(g_model.binding.context) == 4096, "block size isn't 4096");
            Require(ops->CapacityBytes(g_model.binding.context) == 1024ull * 4096ull, "4Kn capacity is wrong");
        } });
        cases.push_back({ "READ(10) returns the disk contents", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_bot.blockBytes = 512;
            g_bot.blockCount = 2048;
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            const BlockDeviceOps* ops = g_model.binding.ops;
            std::vector<std::uint8_t> buffer(4 * 512, 0);
            Require(ops->Read(g_model.binding.context, 2 * 512, buffer.data(), 4 * 512) == Storage::Error::Ok, "Read failed");
            for (std::uint32_t block = 0; block < 4; ++block) {
                for (std::uint32_t offset = 0; offset < 512; ++offset) {
                    Require(buffer[block * 512 + offset] == DiskByte(2 + block, offset), "read bytes are wrong");
                }
            }
            Require(g_bot.lastLba == 2 && g_bot.lastBlocks == 4, "READ(10) LBA/length is wrong");
        } });
        cases.push_back({ "Read rejects unaligned or out-of-range requests", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            const BlockDeviceOps* ops = g_model.binding.ops;
            std::uint8_t buffer[1024] = {};
            Require(ops->Read(g_model.binding.context, 1, buffer, 512) == Storage::Error::InvalidArgument, "unaligned offset was accepted");
            Require(ops->Read(g_model.binding.context, 0, buffer, 3) == Storage::Error::InvalidArgument, "unaligned length was accepted");
            Require(ops->Read(g_model.binding.context, 2048ull * 512ull, buffer, 512) == Storage::Error::InvalidArgument, "out-of-range read was accepted");
            Require(ops->Read(g_model.binding.context, 0, nullptr, 512) == Storage::Error::InvalidArgument, "null buffer was accepted");
        } });
        cases.push_back({ "Read splits large requests into several commands", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_bot.blockBytes = 512;
            g_bot.blockCount = 2048;
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            const BlockDeviceOps* ops = g_model.binding.ops;
            std::vector<std::uint8_t> buffer(40 * 512, 0);
            const unsigned before = g_bot.readCommands;
            Require(ops->Read(g_model.binding.context, 0, buffer.data(), 40 * 512) == Storage::Error::Ok, "large read failed");
            Require(g_bot.readCommands - before == 3, "large read wasn't split into three commands");
        } });
        cases.push_back({ "CSW failure status fails the bind", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_bot.cswStatus = 1;
            Require(BindStorage() == KernelError::kIo, "CSW failure wasn't reported as Io");
        } });
        cases.push_back({ "bad CSW signature fails the bind", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_bot.badCswSignature = true;
            Require(BindStorage() == KernelError::kCorrupt, "bad CSW signature wasn't Corrupt");
        } });
        cases.push_back({ "Init is repeatable and Bind still works", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(InitModule() == KernelError::kOk, "re-Init failed");
            g_bot.blockBytes = 512;
            Require(BindStorage() == KernelError::kOk, "Bind failed");
        } });
        cases.push_back({ "binding failure is mapped and the claim is released", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_model.bindingResult = KernelError::kBusy;
            Require(BindStorage() == KernelError::kBusy, "Binding failure wasn't propagated");
            Require(g_model.released, "claim wasn't released after a binding failure");
        } });
        cases.push_back({ "Abort clears the binding and releases the claim", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(BindStorage() == KernelError::kOk, "Bind failed");
            Require(_Module_Abort(nullptr) == KernelError::kOk, "Abort failed");
            Require(!g_model.bound || !g_model.claimed, "Abort didn't clear the model state");
        } });
        cases.push_back({ "Probe rejects non-storage interfaces", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            std::uint32_t claimed = 1;
            UsbDeviceFacts facts = StorageFacts();
            facts.classCode = 0x09;
            Require(g_host.classOps->Probe(g_host.classContext, &facts, &claimed) == KernelError::kOk && claimed == 0, "hub interface was claimed");
        } });
        return cases;
    }
}

// 入口：跑用例并汇总
int main() {
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckUsbStorage: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckUsbStorage: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
