/* main.cpp
    CheckUsbCore：Usb.Host 核心实现（UsbCore.cpp）的宿主侧检查

    用假 UsbControllerOps + 假类驱动驱动枚举状态机与传输路由：控制器只按脚本回答端口状态
    与描述符，类驱动只记录 Probe/Bind/Unbind，不涉及任何真实硬件
    用例覆盖 AddController/RegisterClass 的容量与参数、枚举状态机、晚注册重扫、Control/Bulk
    路由、设备移除、Poll 幂等与多控制器路由；最后一行打印固定格式的用例计数
*/

#include <UsbCore.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    // 用例总数：末尾与实跑数核对
    constexpr unsigned kCaseCount = 12;

    // 已跑用例数
    unsigned g_ran = 0;

    // 日志记录：只统计关键标记出现的次数，足够断言
    struct LogRecorder {
        uint32_t lineCount = 0;           // Line 调用次数
        uint32_t texturedCount = 0;       // Write 调用次数
        uint32_t hexCount = 0;            // Hex 调用次数
        uint32_t enumeratedCount = 0;     // "usbcore: device enumerated" 次数
        uint32_t boundCount = 0;          // "usbcore: class bound" 次数
        uint32_t removedCount = 0;        // "usbcore: device removed" 次数
        uint32_t fullCount = 0;           // "device table is full" 次数
        uint32_t controllerCount = 0;     // "usbcore: controller added" 次数
    };

    // 平台回调：context 是 LogRecorder
    void LogLineCallback(void* context, const char* tag, const char* text) {
        (void)tag;
        LogRecorder* recorder = static_cast<LogRecorder*>(context);
        if (recorder == nullptr || text == nullptr) return;
        ++recorder->lineCount;
        if (std::strstr(text, "usbcore: device enumerated") != nullptr) ++recorder->enumeratedCount;
        if (std::strstr(text, "usbcore: class bound") != nullptr) ++recorder->boundCount;
        if (std::strstr(text, "usbcore: device removed") != nullptr) ++recorder->removedCount;
        if (std::strstr(text, "device table is full") != nullptr) ++recorder->fullCount;
        if (std::strstr(text, "usbcore: controller added") != nullptr) ++recorder->controllerCount;
    }

    void LogTextCallback(void* context, const char* text) {
        (void)text;
        LogRecorder* recorder = static_cast<LogRecorder*>(context);
        if (recorder != nullptr) ++recorder->texturedCount;
    }

    void LogHexCallback(void* context, uint64_t value) {
        (void)value;
        LogRecorder* recorder = static_cast<LogRecorder*>(context);
        if (recorder != nullptr) ++recorder->hexCount;
    }

    // 假控制器的脚本与计数
    struct FakeController {
        uint32_t portCount = 0;            // 根端口数
        uint32_t presentMask = 0;          // 端口连接位图（位 = 端口号 - 1）
        uint32_t speed = 3;                // 端口报告的速度编码
        uint32_t descriptorClass = 0x08;   // 接口类码（默认 hub）
        uint32_t descriptorSubclass = 0;
        uint32_t descriptorProtocol = 0;
        uint32_t vendorId = 0x1234;        // 设备厂商 ID
        uint32_t productId = 0x5678;       // 设备产品 ID
        bool failControl = false;          // 控制传输失败注入
        bool failEnumeration = false;      // 端口复位失败注入
        uint32_t pollCount = 0;            // Poll 调用次数
        uint32_t resetCount = 0;           // PortReset 次数
        uint32_t enableCount = 0;          // EnableDevice 次数
        uint32_t disableCount = 0;         // DisableDevice 次数
        uint32_t controlCount = 0;         // SubmitControl 次数
        uint32_t bulkCount = 0;            // SubmitBulk 次数
        uint32_t lastAddress = 0;          // 最近一次控制/批量传输的设备地址
        uint32_t lastEndpoint = 0;         // 最近一次批量传输的端点
        uint32_t lastSetupRequest = 0;     // 最近一次控制传输的 bRequest
        std::string lastBulkPayload;       // 最近一次批量传输的首字节内容
    };

    // 设备描述符：18 字节，idVendor/idProduct 在 8..11
    void FillDeviceDescriptor(uint8_t* buffer, uint32_t* actual, uint32_t bytes, const FakeController& fake) {
        uint8_t descriptor[18] = {};
        descriptor[0] = 18;
        descriptor[1] = 0x01;
        descriptor[2] = 0x00;
        descriptor[3] = 0x02;
        descriptor[8] = static_cast<uint8_t>(fake.vendorId);
        descriptor[9] = static_cast<uint8_t>(fake.vendorId >> 8);
        descriptor[10] = static_cast<uint8_t>(fake.productId);
        descriptor[11] = static_cast<uint8_t>(fake.productId >> 8);
        const uint32_t copy = bytes < 18 ? bytes : 18;
        for (uint32_t index = 0; index < copy; ++index) buffer[index] = descriptor[index];
        *actual = copy;
    }

    // 配置描述符：配置头 9 字节 + 接口 9 字节，接口类码在偏移 9+5..9+7
    void FillConfigDescriptor(uint8_t* buffer, uint32_t* actual, uint32_t bytes, const FakeController& fake) {
        uint8_t descriptor[18] = {};
        descriptor[0] = 9;      // 配置描述符长度
        descriptor[1] = 0x02;   // 类型：配置
        descriptor[2] = 18;     // wTotalLength = 18
        descriptor[3] = 0;
        descriptor[4] = 1;      // 接口数
        descriptor[5] = 1;
        descriptor[9] = 9;      // 接口描述符长度
        descriptor[10] = 0x04;  // 类型：接口
        descriptor[11] = 0;
        descriptor[12] = 0;
        descriptor[13] = 1;     // 端点数
        descriptor[14] = static_cast<uint8_t>(fake.descriptorClass);
        descriptor[15] = static_cast<uint8_t>(fake.descriptorSubclass);
        descriptor[16] = static_cast<uint8_t>(fake.descriptorProtocol);
        const uint32_t copy = bytes < 18 ? bytes : 18;
        for (uint32_t index = 0; index < copy; ++index) buffer[index] = descriptor[index];
        *actual = copy;
    }

    // —— UsbControllerOps 的假实现：context 是 FakeController ——
    KernelError FakeSubmitControl(void* context, uint32_t deviceAddress, const UsbSetupPacket* packet, void* buffer,
                                  uint32_t bytes, uint32_t* actual) {
        FakeController* fake = static_cast<FakeController*>(context);
        if (fake == nullptr || packet == nullptr) return KernelError::kInvalidArgument;
        ++fake->controlCount;
        fake->lastAddress = deviceAddress;
        fake->lastSetupRequest = packet->request;
        if (fake->failControl) return KernelError::kIo;
        const uint32_t type = packet->value >> 8;
        if (type == 0x01) {
            FillDeviceDescriptor(static_cast<uint8_t*>(buffer), actual, bytes, *fake);
            return KernelError::kOk;
        }
        if (type == 0x02) {
            FillConfigDescriptor(static_cast<uint8_t*>(buffer), actual, bytes, *fake);
            return KernelError::kOk;
        }
        // 其它请求（类请求、SET_CONFIGURATION 等）按无数据成功处理
        if (actual != nullptr) *actual = 0;
        return KernelError::kOk;
    }

    KernelError FakeSubmitBulk(void* context, uint32_t deviceAddress, uint32_t endpoint, void* buffer, uint32_t bytes,
                               uint32_t* actual) {
        FakeController* fake = static_cast<FakeController*>(context);
        if (fake == nullptr || buffer == nullptr) return KernelError::kInvalidArgument;
        ++fake->bulkCount;
        fake->lastAddress = deviceAddress;
        fake->lastEndpoint = endpoint;
        uint8_t* target = static_cast<uint8_t*>(buffer);
        for (uint32_t index = 0; index < bytes; ++index) target[index] = static_cast<uint8_t>(index & 0xFFu);
        fake->lastBulkPayload = std::string(1, static_cast<char>(target[0]));
        if (actual != nullptr) *actual = bytes;
        return KernelError::kOk;
    }

    KernelError FakePoll(void* context, uint32_t budget) {
        (void)budget;
        FakeController* fake = static_cast<FakeController*>(context);
        if (fake != nullptr) ++fake->pollCount;
        return KernelError::kOk;
    }

    uint32_t FakePortCount(void* context) { return static_cast<FakeController*>(context)->portCount; }

    KernelError FakePortStatus(void* context, uint32_t port, uint32_t* status) {
        FakeController* fake = static_cast<FakeController*>(context);
        if (fake == nullptr || status == nullptr || port == 0 || port > fake->portCount) {
            return KernelError::kInvalidArgument;
        }
        *status = 0;
        const uint32_t bit = 1u << (port - 1);
        if ((fake->presentMask & bit) != 0) *status |= UsbCore::kPortStatusPresent | UsbCore::kPortStatusEnabled;
        *status |= (fake->speed & UsbCore::kPortStatusSpeedMask) << UsbCore::kPortStatusSpeedShift;
        return KernelError::kOk;
    }

    KernelError FakePortReset(void* context, uint32_t port) {
        (void)port;
        FakeController* fake = static_cast<FakeController*>(context);
        if (fake == nullptr) return KernelError::kInvalidArgument;
        ++fake->resetCount;
        return fake->failEnumeration ? KernelError::kIo : KernelError::kOk;
    }

    KernelError FakeEnableDevice(void* context, uint32_t port, uint32_t deviceAddress) {
        (void)port;
        FakeController* fake = static_cast<FakeController*>(context);
        if (fake == nullptr) return KernelError::kInvalidArgument;
        ++fake->enableCount;
        fake->lastAddress = deviceAddress;
        return KernelError::kOk;
    }

    KernelError FakeDisableDevice(void* context, uint32_t deviceAddress) {
        (void)deviceAddress;
        FakeController* fake = static_cast<FakeController*>(context);
        if (fake == nullptr) return KernelError::kInvalidArgument;
        ++fake->disableCount;
        return KernelError::kOk;
    }

    // 假类驱动：记录 Probe/Bind/Unbind 与收到的设备事实
    struct FakeClass {
        uint32_t probeCount = 0;
        uint32_t bindCount = 0;
        uint32_t unbindCount = 0;
        uint32_t claimResult = 1;   // Probe 的 claimed 返回值
        UsbDeviceFacts lastFacts = {};
        uint32_t lastClassHandle = 0;
    };

    KernelError FakeProbe(void* context, const UsbDeviceFacts* facts, uint32_t* claimed) {
        FakeClass* fake = static_cast<FakeClass*>(context);
        if (fake == nullptr || facts == nullptr || claimed == nullptr) return KernelError::kInvalidArgument;
        ++fake->probeCount;
        fake->lastFacts = *facts;
        *claimed = fake->claimResult;
        return KernelError::kOk;
    }

    KernelError FakeBind(void* context, const UsbDeviceFacts* facts, uint32_t classHandle) {
        FakeClass* fake = static_cast<FakeClass*>(context);
        if (fake == nullptr || facts == nullptr) return KernelError::kInvalidArgument;
        ++fake->bindCount;
        fake->lastClassHandle = classHandle;
        return KernelError::kOk;
    }

    void FakeUnbind(void* context, uint32_t deviceHandle) {
        (void)deviceHandle;
        FakeClass* fake = static_cast<FakeClass*>(context);
        if (fake != nullptr) ++fake->unbindCount;
    }

    // —— 用例公共夹具 ——
    struct Fixture {
        UsbCore::State state;
        LogRecorder log;
        UsbCore::Platform platform;
    };

    // 造一份夹具：日志接记录器
    Fixture MakeFixture() {
        Fixture fixture;
        fixture.platform = {&LogLineCallback, &LogTextCallback, &LogHexCallback, &fixture.log};
        UsbCore::Reset(fixture.state);
        return fixture;
    }

    // 静态控制器 ops：全部回调都走假实现
    const UsbControllerOps kFakeOps = {&FakeSubmitControl, &FakeSubmitBulk, &FakePoll,  &FakePortCount,
                                       &FakePortStatus,  &FakePortReset,  &FakeEnableDevice, &FakeDisableDevice};

    // 静态类驱动 ops
    const UsbClassOps kFakeClassOps = {&FakeProbe, &FakeBind, &FakeUnbind};

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 登记一个控制器并断言成功
    ControllerHandle MustAddController(UsbCore::State& state, const UsbCore::Platform& platform, FakeController& fake) {
        ControllerHandle handle = 0;
        const KernelError error = UsbCore::AddController(state, platform, &kFakeOps, &fake, &handle);
        Require(error == KernelError::kOk, "AddController 必须成功");
        Require(handle != 0, "控制器句柄必须非零");
        return handle;
    }

    // 登记一个类驱动并断言成功
    ClassHandle MustRegisterClass(UsbCore::State& state, const UsbCore::Platform& platform, uint32_t classCode,
                                  FakeClass& fake) {
        ClassHandle handle = 0;
        const KernelError error = UsbCore::RegisterClass(state, platform, classCode, &kFakeClassOps, &fake, &handle);
        Require(error == KernelError::kOk, "RegisterClass 必须成功");
        Require(handle != 0, "类驱动句柄必须非零");
        return handle;
    }

    // 枚举一个端口上的设备：Poll 一次并在失败时给出原因
    void PollOnce(UsbCore::State& state, const UsbCore::Platform& platform, uint32_t budget = 16) {
        const KernelError error = UsbCore::Poll(state, platform, budget);
        Require(error == KernelError::kOk, "Poll 必须成功");
    }

    // 取第一个设备句柄；没有设备时失败
    UsbDeviceHandle FirstDevice(UsbCore::State& state) {
        for (uint32_t index = 0; index < UsbCore::kMaxDevices; ++index) {
            if (state.devices[index].used != 0) return static_cast<UsbDeviceHandle>(index + 1);
        }
        Fail("没有已枚举设备");
    }

    // 控制器的表容量与参数拒绝
    void CaseAddControllerLimits() {
        Fixture fixture = MakeFixture();
        FakeController first;
        FakeController second;
        FakeController third;
        first.portCount = 2;
        second.portCount = 2;
        third.portCount = 2;
        MustAddController(fixture.state, fixture.platform, first);
        MustAddController(fixture.state, fixture.platform, second);
        ControllerHandle rejected = 0;
        Require(UsbCore::AddController(fixture.state, fixture.platform, &kFakeOps, &third, &rejected) ==
                    KernelError::kOutOfResources, "控制器表满必须返回 kOutOfResources");
        Require(UsbCore::AddController(fixture.state, fixture.platform, nullptr, &third, &rejected) ==
                    KernelError::kInvalidArgument, "空 ops 必须拒绝");
        Require(fixture.log.controllerCount == 2, "控制器标识行必须打印两次");
    }

    // 类驱动表容量与参数拒绝
    void CaseRegisterClassLimits() {
        Fixture fixture = MakeFixture();
        FakeClass classes[UsbCore::kMaxClasses];
        for (uint32_t index = 0; index < UsbCore::kMaxClasses; ++index) {
            MustRegisterClass(fixture.state, fixture.platform, 0x08u + index, classes[index]);
        }
        FakeClass overflow;
        ClassHandle rejected = 0;
        Require(UsbCore::RegisterClass(fixture.state, fixture.platform, 0x30, &kFakeClassOps, &overflow, &rejected) ==
                    KernelError::kOutOfResources, "类驱动表满必须返回 kOutOfResources");
        Require(UsbCore::RegisterClass(fixture.state, fixture.platform, 0x08, nullptr, &overflow, &rejected) ==
                    KernelError::kInvalidArgument, "空 ops 必须拒绝");
        Require(UsbCore::RegisterClass(fixture.state, fixture.platform, 0, &kFakeClassOps, &overflow, &rejected) ==
                    KernelError::kInvalidArgument, "类码 0 必须拒绝");
    }

    // 枚举状态机：连接端口 → Poll 后得到 DeviceFacts
    void CaseEnumerateDevice() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 2;
        controller.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, controller);
        PollOnce(fixture.state, fixture.platform);
        Require(controller.resetCount == 1 && controller.enableCount == 1, "枚举必须复位并启用设备");
        Require(controller.controlCount >= 2, "枚举必须读设备与配置描述符");
        Require(fixture.state.deviceCount == 1, "枚举后设备数必须为 1");
        Require(fixture.log.enumeratedCount == 1, "必须打印设备枚举标识行");
        UsbDeviceFacts facts = {};
        const UsbDeviceHandle device = FirstDevice(fixture.state);
        Require(UsbCore::DeviceFacts(fixture.state, device, &facts) == KernelError::kOk, "设备事实查询必须成功");
        Require(facts.classCode == 0x08, "设备类码必须来自接口描述符");
        Require(facts.vendorId == controller.vendorId && facts.productId == controller.productId, "厂商/产品 ID 必须来自设备描述符");
        Require(facts.port == 1, "设备必须记录所在根端口");
        Require(facts.speed == controller.speed, "设备速度必须来自端口状态");
    }

    // 晚注册：类驱动后登记时立即重扫已枚举设备并绑定
    void CaseLateClassRegister() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 1;
        controller.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, controller);
        PollOnce(fixture.state, fixture.platform);
        Require(fixture.state.deviceCount == 1, "设备必须先枚举");
        FakeClass hubClass;
        MustRegisterClass(fixture.state, fixture.platform, 0x08, hubClass);
        Require(hubClass.probeCount == 1, "晚注册必须立即 Probe 已枚举设备");
        Require(hubClass.bindCount == 1, "Probe 接管后必须立即 Bind");
        Require(hubClass.lastFacts.classCode == 0x08, "Bind 必须拿到设备事实");
        Require(fixture.log.boundCount == 1, "必须打印类绑定标识行");
    }

    // Probe 拒绝接管时不绑定
    void CaseProbeRejects() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 1;
        controller.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, controller);
        FakeClass hubClass;
        hubClass.claimResult = 0;
        MustRegisterClass(fixture.state, fixture.platform, 0x08, hubClass);
        PollOnce(fixture.state, fixture.platform);
        Require(hubClass.probeCount == 1, "枚举后必须尝试 Probe");
        Require(hubClass.bindCount == 0, "claimed 为 0 时不得 Bind");
        Require(fixture.state.devices[0].bound == 0, "设备必须保持未绑定");
    }

    // 控制/批量传输按设备句柄路由到所属控制器
    void CaseTransferRouting() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 1;
        controller.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, controller);
        PollOnce(fixture.state, fixture.platform);
        const UsbDeviceHandle device = FirstDevice(fixture.state);
        const uint32_t beforeControl = controller.controlCount;
        UsbSetupPacket packet = {};
        packet.requestType = 0x21;
        packet.request = 0x0A;
        uint8_t buffer[16] = {};
        uint32_t actual = 0;
        Require(UsbCore::ControlTransfer(fixture.state, device, &packet, buffer, sizeof(buffer), &actual) ==
                    KernelError::kOk, "控制传输必须路由到控制器");
        Require(controller.controlCount == beforeControl + 1, "控制器必须收到控制传输");
        Require(controller.lastSetupRequest == 0x0A, "控制器必须看到原始请求");
        uint8_t bulkData[8] = {};
        Require(UsbCore::BulkTransfer(fixture.state, device, 0x81, bulkData, sizeof(bulkData), &actual) ==
                    KernelError::kOk, "批量传输必须路由到控制器");
        Require(controller.bulkCount == 1 && controller.lastEndpoint == 0x81, "控制器必须收到批量端点");
        Require(UsbCore::ControlTransfer(fixture.state, 0xFFFF, &packet, buffer, sizeof(buffer), &actual) ==
                    KernelError::kInvalidArgument, "无效设备句柄必须拒绝");
        controller.failControl = true;
        Require(UsbCore::ControlTransfer(fixture.state, device, &packet, buffer, sizeof(buffer), &actual) ==
                    KernelError::kIo, "控制器错误必须原样传播");
    }

    // 设备移除：连接消失后 Unbind 与 DisableDevice 都被调用
    void CaseDeviceRemoved() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 1;
        controller.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, controller);
        FakeClass hubClass;
        MustRegisterClass(fixture.state, fixture.platform, 0x08, hubClass);
        PollOnce(fixture.state, fixture.platform);
        const UsbDeviceHandle device = FirstDevice(fixture.state);
        controller.presentMask = 0;
        PollOnce(fixture.state, fixture.platform);
        Require(hubClass.unbindCount == 1, "设备移除必须先 Unbind 类驱动");
        Require(controller.disableCount == 1, "设备移除必须停用控制器侧设备");
        Require(fixture.state.deviceCount == 0, "移除后设备数必须归零");
        UsbDeviceFacts facts = {};
        Require(UsbCore::DeviceFacts(fixture.state, device, &facts) == KernelError::kInvalidArgument,
                "已移除设备的句柄必须失效");
        Require(fixture.log.removedCount == 1, "必须打印设备移除标识行");
    }

    // Poll 幂等：没有变化时第二次 Poll 不重复枚举、不重复绑定
    void CaseIdempotentPoll() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 1;
        controller.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, controller);
        FakeClass hubClass;
        MustRegisterClass(fixture.state, fixture.platform, 0x08, hubClass);
        PollOnce(fixture.state, fixture.platform);
        const uint32_t controls = controller.controlCount;
        PollOnce(fixture.state, fixture.platform);
        PollOnce(fixture.state, fixture.platform);
        Require(controller.controlCount == controls, "重复 Poll 不得重复读描述符");
        Require(hubClass.bindCount == 1, "重复 Poll 不得重复绑定");
        Require(fixture.log.enumeratedCount == 1, "枚举标识行只能出现一次");
    }

    // 设备表满：超出的端口只打诊断，不越界写
    void CaseDeviceTableFull() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = UsbCore::kMaxDevices + 4;
        controller.presentMask = 0xFFFFFFFFu;
        MustAddController(fixture.state, fixture.platform, controller);
        PollOnce(fixture.state, fixture.platform, 64);
        Require(fixture.state.deviceCount == UsbCore::kMaxDevices, "设备数必须被表容量截断");
        Require(fixture.log.fullCount >= 1, "设备表满必须打印诊断");
        UsbDeviceFacts facts = {};
        Require(UsbCore::DeviceFacts(fixture.state, static_cast<UsbDeviceHandle>(UsbCore::kMaxDevices + 1), &facts) ==
                    KernelError::kInvalidArgument, "越界句柄必须拒绝");
    }

    // 多控制器：设备上的传输路由到它所属的控制器
    void CaseMultiController() {
        Fixture fixture = MakeFixture();
        FakeController first;
        first.portCount = 1;
        FakeController second;
        second.portCount = 1;
        second.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, first);
        const ControllerHandle secondHandle = MustAddController(fixture.state, fixture.platform, second);
        PollOnce(fixture.state, fixture.platform);
        Require(fixture.state.deviceCount == 1, "第二个控制器上的设备必须枚举");
        UsbDeviceFacts facts = {};
        const UsbDeviceHandle device = FirstDevice(fixture.state);
        Require(UsbCore::DeviceFacts(fixture.state, device, &facts) == KernelError::kOk, "设备事实查询必须成功");
        Require(facts.controllerHandle == secondHandle, "设备必须记录所属控制器句柄");
        UsbSetupPacket packet = {};
        uint8_t buffer[2] = {};
        uint32_t actual = 0;
        Require(UsbCore::ControlTransfer(fixture.state, device, &packet, buffer, sizeof(buffer), &actual) ==
                    KernelError::kOk, "控制传输必须成功");
        Require(first.controlCount == 0, "第一个控制器不得收到不属于它的传输");
        Require(second.controlCount >= 3, "第二个控制器必须收到枚举与传输");
    }

    // 复位状态：清空三张表与计数
    void CaseResetState() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 1;
        controller.presentMask = 0x1;
        MustAddController(fixture.state, fixture.platform, controller);
        FakeClass hubClass;
        MustRegisterClass(fixture.state, fixture.platform, 0x08, hubClass);
        PollOnce(fixture.state, fixture.platform);
        Require(fixture.state.deviceCount == 1, "复位前必须已枚举");
        UsbCore::Reset(fixture.state);
        Require(fixture.state.controllerCount == 0 && fixture.state.classCount == 0 && fixture.state.deviceCount == 0,
                "复位必须清空三张表");
        UsbDeviceFacts facts = {};
        Require(UsbCore::DeviceFacts(fixture.state, 1, &facts) == KernelError::kInvalidArgument, "复位后句柄必须失效");
    }

    // 描述符失败：不产生设备记录，Poll 不报错
    void CaseDescriptorFailure() {
        Fixture fixture = MakeFixture();
        FakeController controller;
        controller.portCount = 1;
        controller.presentMask = 0x1;
        controller.failControl = true;
        MustAddController(fixture.state, fixture.platform, controller);
        PollOnce(fixture.state, fixture.platform);
        Require(fixture.state.deviceCount == 0, "描述符失败不得产生设备记录");
        Require(fixture.log.enumeratedCount == 0, "描述符失败不得打印枚举标识行");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckUsbCore: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 依次跑全部用例
    void Execute() {
        Case("控制器表容量与参数", CaseAddControllerLimits);
        Case("类驱动表容量与参数", CaseRegisterClassLimits);
        Case("枚举状态机与设备事实", CaseEnumerateDevice);
        Case("晚注册立即重扫绑定", CaseLateClassRegister);
        Case("Probe 拒绝接管", CaseProbeRejects);
        Case("控制与批量传输路由", CaseTransferRouting);
        Case("设备移除解绑", CaseDeviceRemoved);
        Case("Poll 幂等", CaseIdempotentPoll);
        Case("设备表满", CaseDeviceTableFull);
        Case("多控制器路由", CaseMultiController);
        Case("复位清空状态", CaseResetState);
        Case("描述符失败不产生设备", CaseDescriptorFailure);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckUsbCore: " << g_ran << "/" << kCaseCount << " cases passed" << std::endl;
    }
}

// 入口：跑全部用例
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string argument = argv[1];
            if (argument == "-h" || argument == "--help") {
                std::cout << "CheckUsbCore — Usb.Host 核心实现的宿主侧检查\n\n用法：CheckUsbCore\n";
                return 0;
            }
            Fail("无法识别的参数：" + argument);
        }
        Execute();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckUsbCore: " << error.what() << '\n';
        return 1;
    }
}
