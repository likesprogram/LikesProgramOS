/* main.cpp
    CheckUsbHub：UsbHub 模块（hub 类描述符 / 端口上电 / 连接变化处理）的宿主侧检查
    复用 UsbHub/Src/Module.cpp，伪 Usb.Host 记录控制传输序列
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
    constexpr unsigned kCaseCount = 8;
    // 已跑 / 失败计数
    unsigned g_ran = 0;
    unsigned g_failed = 0;
    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 伪 Usb.Host 记录
    struct FakeHost {
        std::uint32_t classCode = 0;       // 登记的类码
        const UsbClassOps* ops = nullptr;  // 类驱动回调表
        void* context = nullptr;           // 类驱动上下文
        ClassHandle handle = 9;            // 类句柄
        std::uint32_t descriptorBytes = 9; // hub 描述符长度
        std::uint8_t descriptorType = 0x29;// hub 描述符类型
        std::uint32_t ports = 4;           // 端口数
        std::uint32_t portStatus[16] = {}; // 端口状态
        unsigned powerRequests = 0;        // 上电请求数
        unsigned clearRequests = 0;        // 清除请求数
        std::vector<std::uint32_t> cleared; // 清除端口序列
    };
    FakeHost g_host;
    // 伪日志
    struct FakeLog { char tag[16] = {}; char text[256] = {}; };
    FakeLog g_log;
    // 伪 Core.Log 一行
    void LogLine(const char* tag, const char* text) {
        std::strncpy(g_log.tag, tag != nullptr ? tag : "", sizeof(g_log.tag) - 1);
        std::strncpy(g_log.text, text != nullptr ? text : "", sizeof(g_log.text) - 1);
    }
    // 伪 Core.Log 文本
    void LogWrite(const char* text) { (void)text; }
    // 伪 Core.Mem
    void MemZero(void* destination, std::uint64_t bytes) { if (destination != nullptr) std::memset(destination, 0, static_cast<std::size_t>(bytes)); }
    void MemCopy(void* destination, const void* source, std::uint64_t bytes) { if (destination != nullptr && source != nullptr) std::memcpy(destination, source, static_cast<std::size_t>(bytes)); }

    // 伪 Usb.Host：RegisterClass
    KernelError HostRegisterClass(std::uint32_t classCode, const UsbClassOps* ops, void* context, ClassHandle* handle) {
        if (ops == nullptr || handle == nullptr) return KernelError::kInvalidArgument;
        g_host.classCode = classCode;
        g_host.ops = ops;
        g_host.context = context;
        *handle = g_host.handle;
        return KernelError::kOk;
    }
    // 伪 Usb.Host：控制传输
    KernelError HostControlTransfer(UsbDeviceHandle device, const UsbSetupPacket* packet, void* buffer, std::uint32_t bytes, std::uint32_t* actual) {
        (void)device;
        if (packet == nullptr || actual == nullptr) return KernelError::kInvalidArgument;
        if (packet->requestType == 0x80 && packet->request == 0x06 && packet->value == 0x2900) {
            if (buffer == nullptr || bytes < 7) return KernelError::kInvalidArgument;
            const std::uint32_t copy = bytes < g_host.descriptorBytes ? bytes : g_host.descriptorBytes;
            std::uint8_t* out = static_cast<std::uint8_t*>(buffer);
            std::memset(out, 0, copy);
            out[0] = static_cast<std::uint8_t>(g_host.descriptorBytes);
            out[1] = g_host.descriptorType;
            out[2] = static_cast<std::uint8_t>(g_host.ports);
            *actual = copy;
            return KernelError::kOk;
        }
        if (packet->requestType == 0x23 && packet->request == 0x03 && packet->value == 8) { ++g_host.powerRequests; return KernelError::kOk; }
        if (packet->requestType == 0x23 && packet->request == 0x01 && packet->value == 16) { ++g_host.clearRequests; g_host.cleared.push_back(packet->index); return KernelError::kOk; }
        if (packet->requestType == 0xA3 && packet->request == 0x00) {
            if (buffer == nullptr || bytes < 4) return KernelError::kInvalidArgument;
            std::uint8_t* out = static_cast<std::uint8_t*>(buffer);
            const std::uint32_t status = packet->index < 16 ? g_host.portStatus[packet->index] : 0;
            for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(status >> (8 * i));
            *actual = 4;
            return KernelError::kOk;
        }
        return KernelError::kNotSupported;
    }

    // 初始化模块
    KernelError InitModule() {
        g_host = FakeHost{};
        g_log = FakeLog{};
        _Contract_Import_UsbHost_v1.RegisterClass = &HostRegisterClass;
        _Contract_Import_UsbHost_v1.ControlTransfer = &HostControlTransfer;
        _Contract_Import_CoreLog_v1.Line = &LogLine;
        _Contract_Import_CoreLog_v1.Write = &LogWrite;
        _Contract_Import_CoreMem_v1.Zero = &MemZero;
        _Contract_Import_CoreMem_v1.Copy = &MemCopy;
        KernelAbiContext context{};
        context.bytes = static_cast<std::uint32_t>(sizeof(KernelAbiContext));
        context.version = 1;
        context.moduleId = kModUsbHub;
        return _Module_Init(&context);
    }
    // 伪 hub 设备事实
    UsbDeviceFacts HubFacts() {
        UsbDeviceFacts facts{};
        facts.bytes = static_cast<std::uint32_t>(sizeof(UsbDeviceFacts));
        facts.deviceHandle = 5;
        facts.controllerHandle = 2;
        facts.classCode = 0x09;
        facts.speed = 3;
        return facts;
    }
    // 触发 Bind
    KernelError BindHub() {
        UsbDeviceFacts facts = HubFacts();
        return g_host.ops->Bind(g_host.context, &facts, g_host.handle);
    // 一条用例
    }
    struct Case { std::string name; std::function<void()> body; };

    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        cases.push_back({ "Init registers hub class and reports no device", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(g_host.classCode == 0x09, "class 0x09 wasn't registered");
            Require(std::strcmp(g_log.text, "usbhub: no hub device") == 0, "no-hub marker is missing");
        } });
        cases.push_back({ "Probe claims hub subclasses 0 and 1", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            std::uint32_t claimed = 0;
            UsbDeviceFacts facts = HubFacts();
            Require(g_host.ops->Probe(g_host.context, &facts, &claimed) == KernelError::kOk && claimed == 1, "hub wasn't claimed");
            facts.subclass = 1;
            Require(g_host.ops->Probe(g_host.context, &facts, &claimed) == KernelError::kOk && claimed == 1, "multi-TT hub wasn't claimed");
            facts.subclass = 2;
            Require(g_host.ops->Probe(g_host.context, &facts, &claimed) == KernelError::kOk && claimed == 0, "unknown subclass was claimed");
        } });
        cases.push_back({ "Bind powers every port", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_host.ports = 4;
            Require(BindHub() == KernelError::kOk, "Bind failed");
            Require(g_host.powerRequests == 4, "not every port was powered");
        } });
        cases.push_back({ "Bind clears connection change bits", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_host.ports = 4;
            g_host.portStatus[2] = 1u | (1u << 16);
            Require(BindHub() == KernelError::kOk, "Bind failed");
            Require(g_host.clearRequests == 1 && g_host.cleared[0] == 2, "connection change wasn't cleared");
            Require(std::strcmp(g_log.text, "usbhub: hub bound ports=4 connected=1") == 0, "bound marker is wrong");
        } });
        cases.push_back({ "Bind rejects a short hub descriptor", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_host.descriptorBytes = 5;
            Require(BindHub() == KernelError::kCorrupt, "short descriptor was accepted");
        } });
        cases.push_back({ "Bind rejects a wrong descriptor type", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_host.descriptorType = 0x02;
            Require(BindHub() == KernelError::kCorrupt, "wrong descriptor type was accepted");
        } });
        cases.push_back({ "Bind rejects zero ports", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            g_host.ports = 0;
            Require(BindHub() == KernelError::kCorrupt, "zero-port hub was accepted");
        } });
        cases.push_back({ "Abort clears hub state", [] {
            Require(InitModule() == KernelError::kOk, "Init failed");
            Require(BindHub() == KernelError::kOk, "Bind failed");
            Require(_Module_Abort(nullptr) == KernelError::kOk, "Abort failed");
        } });
        return cases;
    }
}

int main() {
    const std::vector<Case> cases = BuildCases();
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckUsbHub: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try { item.body(); std::cout << "[ OK ]\n"; }
        catch (const std::exception& error) { ++g_failed; std::cout << "[ FAIL ] " << error.what() << "\n"; }
    }
    std::cout << "CheckUsbHub: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
