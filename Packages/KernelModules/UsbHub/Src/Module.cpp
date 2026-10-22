/* Module.cpp
    UsbHub 模块：USB hub 类驱动，逻辑名与版本保持 1.0.0

    经 Usb.Host.RegisterClass(0x09) 登记类驱动；UsbCore 枚举到 hub 后回调 Probe/Bind：
    Probe 校验接口类 0x09 与子类 0x00/0x01；Bind 用 ControlTransfer 读 Hub Class Descriptor
    （GET_DESCRIPTOR type 0x29），逐端口 SET_PORT_FEATURE(PORT_POWER)，再 GET_PORT_STATUS
    统计连接端口并对有 C_PORT_CONNECTION 变化的端口 CLEAR_PORT_FEATURE
    本模块不导出接口、不导入 Block.Registry；下游端口设备的枚举需要 UsbCore 提供 hub 子设备
    支持（本契约版本只有根端口），当前只做 hub 自身描述符 / 端口电源 / 连接变化处理，
    下游设备枚举路径作为后续扩展点
*/

#include <ModuleAbi.hpp>

#include <stdint.h>

namespace {
    // 类码与子类：USB hub 类规范
    constexpr uint32_t kHubClassCode = 0x09;
    constexpr uint32_t kHubSubclass = 0x00;
    constexpr uint32_t kHubSubclassMultiTt = 0x01;
    // USB 标准请求与描述符类型
    constexpr uint8_t kRequestGetDescriptor = 0x06;
    constexpr uint8_t kRequestGetStatus = 0x00;
    constexpr uint8_t kRequestClearFeature = 0x01;
    constexpr uint8_t kRequestSetFeature = 0x03;
    constexpr uint8_t kDescriptorTypeHub = 0x29;
    // bmRequestType 取值：设备到主机 / 主机到设备，标准 / 类，接收者设备 / 其它
    constexpr uint8_t kRequestTypeDeviceIn = 0x80;
    constexpr uint8_t kRequestTypeClassOut = 0x23;
    constexpr uint8_t kRequestTypeClassIn = 0xA3;
    // 端口特性：PORT_POWER 与 C_PORT_CONNECTION
    constexpr uint16_t kPortFeaturePower = 8;
    constexpr uint16_t kPortFeatureConnectionChange = 16;
    // 端口状态位：当前连接与连接变化
    constexpr uint32_t kPortStatusConnected = 1u << 0;
    constexpr uint32_t kPortStatusConnectionChanged = 1u << 16;
    // hub 描述符最小长度与端口数上限（USB 2.0 允许 1..255，本模块限制合理上界）
    constexpr uint32_t kHubDescriptorMinBytes = 7;
    constexpr uint32_t kMaxHubPorts = 15;

    // 模块状态：hub 绑定事实与端口统计
    struct State {
        uint32_t initialized;        // Init 是否完成
        uint32_t bound;              // 是否已绑定 hub 设备
        ClassHandle classHandle;     // RegisterClass 返回的类句柄
        UsbDeviceHandle device;      // hub 设备句柄
        uint32_t portCount;          // 下游端口数
        uint32_t connectedPorts;     // 当前连接的端口数
    };
    State g_state;

    // 文本缓冲：日志行拼接用（不动态分配）
    char g_text[128];

    // 以 32 位十进制追加到文本缓冲，返回新的游标
    uint32_t AppendDecimal(char* text, uint32_t cursor, uint32_t value) {
        char digits[10];
        uint32_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (count > 0) text[cursor++] = digits[--count];
        return cursor;
    }


    // 输出一整行日志：文本以 NUL 结尾
    void LogLine(const char* tag, char* text, uint32_t cursor) {
        text[cursor] = '\0';
        _Contract_Import_CoreLog_v1.Line(tag, text);
    }

    // 输出重定位后的固定文本行
    void LogFixed(const char* tag, const char* text) { _Contract_Import_CoreLog_v1.Line(tag, text); }

    // 控制传输：设备到主机方向的类请求
    Storage::Error TransferIn(UsbDeviceHandle device, uint8_t requestType, uint8_t request, uint16_t value,
        uint16_t index, void* buffer, uint32_t bytes, uint32_t* actual) {
        UsbSetupPacket setup{};
        setup.requestType = requestType;
        setup.request = request;
        setup.value = value;
        setup.index = index;
        setup.length = static_cast<uint16_t>(bytes);
        const KernelError error = _Contract_Import_UsbHost_v1.ControlTransfer(device, &setup, buffer, bytes, actual);
        if (error == KernelError::kOk) return Storage::Error::Ok;
        if (error == KernelError::kNotSupported) return Storage::Error::NotSupported;
        if (error == KernelError::kInvalidArgument) return Storage::Error::InvalidArgument;
        return Storage::Error::Io;
    }

    // 控制传输：无数据阶段的主机到设备类请求
    Storage::Error TransferOut(UsbDeviceHandle device, uint8_t requestType, uint8_t request, uint16_t value, uint16_t index) {
        UsbSetupPacket setup{};
        setup.requestType = requestType;
        setup.request = request;
        setup.value = value;
        setup.index = index;
        setup.length = 0;
        uint32_t actual = 0;
        const KernelError error = _Contract_Import_UsbHost_v1.ControlTransfer(device, &setup, nullptr, 0, &actual);
        return error == KernelError::kOk ? Storage::Error::Ok : Storage::Error::Io;
    }

    // 类驱动 Probe：类 0x09 且子类 0x00/0x01 的接口由本驱动接管
    KernelError HubProbe(void* context, const UsbDeviceFacts* facts, uint32_t* claimed) {
        (void)context;
        if (facts == nullptr || claimed == nullptr) return KernelError::kInvalidArgument;
        *claimed = 0;
        if (facts->classCode != kHubClassCode) return KernelError::kOk;
        if (facts->subclass != kHubSubclass && facts->subclass != kHubSubclassMultiTt) return KernelError::kOk;
        *claimed = 1;
        return KernelError::kOk;
    }

    // 类驱动 Bind：读 hub 描述符、给端口上电、统计连接端口并清除连接变化位
    KernelError HubBind(void* context, const UsbDeviceFacts* facts, uint32_t classHandle) {
        (void)context;
        if (facts == nullptr) return KernelError::kInvalidArgument;
        if (facts->deviceHandle == 0) return KernelError::kInvalidArgument;
        uint8_t hubDescriptor[kHubDescriptorMinBytes] = {};
        uint32_t actual = 0;
        const Storage::Error descriptorError = TransferIn(facts->deviceHandle, kRequestTypeDeviceIn, kRequestGetDescriptor,
            static_cast<uint16_t>(kDescriptorTypeHub << 8), 0, hubDescriptor, kHubDescriptorMinBytes, &actual);
        if (descriptorError != Storage::Error::Ok || actual < kHubDescriptorMinBytes) {
            LogFixed("WARN", "usbhub: hub descriptor read failed");
            return KernelError::kCorrupt;
        }
        if (hubDescriptor[1] != kDescriptorTypeHub) return KernelError::kCorrupt;
        uint32_t portCount = hubDescriptor[2];
        if (portCount == 0) return KernelError::kCorrupt;
        if (portCount > kMaxHubPorts) portCount = kMaxHubPorts;
        g_state.classHandle = classHandle;
        g_state.device = facts->deviceHandle;
        g_state.portCount = portCount;
        g_state.connectedPorts = 0;
        // 逐端口上电：SET_PORT_FEATURE(PORT_POWER)
        for (uint32_t port = 1; port <= portCount; ++port) {
            if (TransferOut(facts->deviceHandle, kRequestTypeClassOut, kRequestSetFeature, kPortFeaturePower,
                    static_cast<uint16_t>(port)) != Storage::Error::Ok) {
                return KernelError::kIo;
            }
        }
        // 逐端口读状态：统计连接，并对有变化的端口清除 C_PORT_CONNECTION
        for (uint32_t port = 1; port <= portCount; ++port) {
            uint8_t status[4] = {};
            if (TransferIn(facts->deviceHandle, kRequestTypeClassIn, kRequestGetStatus, 0, static_cast<uint16_t>(port),
                    status, 4, &actual) != Storage::Error::Ok || actual < 4) {
                return KernelError::kIo;
            }
            const uint32_t bits = static_cast<uint32_t>(status[0]) | (static_cast<uint32_t>(status[1]) << 8)
                | (static_cast<uint32_t>(status[2]) << 16) | (static_cast<uint32_t>(status[3]) << 24);
            if ((bits & kPortStatusConnected) == 0) continue;
            ++g_state.connectedPorts;
            if ((bits & kPortStatusConnectionChanged) == 0) continue;
            if (TransferOut(facts->deviceHandle, kRequestTypeClassOut, kRequestClearFeature,
                    kPortFeatureConnectionChange, static_cast<uint16_t>(port)) != Storage::Error::Ok) {
                return KernelError::kIo;
            }
        }
        g_state.bound = 1;
        uint32_t cursor = 0;
        static constexpr char kPrefix[] = "usbhub: hub bound ports=";
        for (uint32_t index = 0; kPrefix[index] != '\0'; ++index) g_text[cursor++] = kPrefix[index];
        cursor = AppendDecimal(g_text, cursor, g_state.portCount);
        static constexpr char kMiddle[] = " connected=";
        for (uint32_t index = 0; kMiddle[index] != '\0'; ++index) g_text[cursor++] = kMiddle[index];
        cursor = AppendDecimal(g_text, cursor, g_state.connectedPorts);
        LogLine("OK", g_text, cursor);
        return KernelError::kOk;
    }

    // 类驱动 Unbind：清空 hub 绑定事实
    void HubUnbind(void* context, uint32_t deviceHandle) {
        (void)context;
        if (g_state.bound == 0 || g_state.device != deviceHandle) return;
        g_state.bound = 0;
        LogFixed("INFO", "usbhub: hub unbound");
    }

    // 类驱动回调表：非 const 指针要求由 UsbClassOps 持有，表中只放模块内函数地址
    const UsbClassOps kHubClassOps = {&HubProbe, &HubBind, &HubUnbind};
}

// 导入回填区：Usb.Host 前 4 槽（AddController/RegisterClass/ControlTransfer/BulkTransfer）
LP_CONTRACT_IMPORT(UsbHost, 1, UsbHostSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMem, 1, CoreMemSlotsV1) = {};

// 契约声明：不导出任何接口（类驱动经 Usb.Host.RegisterClass 注册，不导出单例）
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < static_cast<uint32_t>(sizeof(KernelAbiContext))) return KernelError::kInvalidArgument;
    if (!(context->moduleId == kModUsbHub)) return KernelError::kInvalidArgument;
    if (_Contract_Import_UsbHost_v1.RegisterClass == nullptr || _Contract_Import_UsbHost_v1.ControlTransfer == nullptr
        || _Contract_Import_CoreLog_v1.Line == nullptr) {
        return KernelError::kNotSupported;
    }
    g_state = State{};
    ClassHandle classHandle = 0;
    const KernelError error = _Contract_Import_UsbHost_v1.RegisterClass(kHubClassCode, &kHubClassOps, &g_state, &classHandle);
    if (error != KernelError::kOk) return error;
    g_state.classHandle = classHandle;
    g_state.initialized = 1;
    // RegisterClass 会对已枚举未绑定设备立即重扫；没有 hub 设备时打固定行并成功返回
    if (g_state.bound == 0) LogFixed("INFO", "usbhub: no hub device");
    return KernelError::kOk;
}

// Abort：撤回 hub 绑定状态（类驱动登记当前不可注销，只清模块状态）
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    g_state = State{};
    return KernelError::kOk;
}
