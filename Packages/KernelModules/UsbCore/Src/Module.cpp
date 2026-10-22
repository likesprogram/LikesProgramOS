/* Module.cpp
    UsbCore 模块入口：导出 Usb.Host v1 六槽，转发到 UsbCore.cpp 的实现

    UsbCore 是闭包里 USB 链路的核心：Xhci 在 Init 里把控制器登记进来，UsbHub/UsbStorage
    在后面的 Init 里登记类驱动（晚注册重扫已枚举设备）；内核编排在模块 Init 之间反复调
    Usb.Host.Poll 推进枚举，直到出现 usb-storage 设备
    导出表的符号名与槽序由生成头 UsbHostSlotsV1 钉住，槽函数签名由结构体成员类型决定，
    本文件不手写第二份布局
*/

#include <ModuleAbi.hpp>

#include "UsbCore.hpp"

// 导入回填区：符号名由生成头钉住，装载器在 Init 前把提供者槽表指针写进这些槽
//   Core.Log 3 槽（Write/Line/Hex）：控制器/设备/绑定的标识行
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};

namespace {
    UsbCore::State g_usb;
    uint32_t g_logReady = 0;

    // 平台适配：日志槽来自 Core.Log 的导入回填区
    void PlatformLine(void* context, const char* tag, const char* text) {
        (void)context;
        if (_Contract_Import_CoreLog_v1.Line != nullptr) _Contract_Import_CoreLog_v1.Line(tag, text);
    }

    void PlatformText(void* context, const char* text) {
        (void)context;
        if (_Contract_Import_CoreLog_v1.Write != nullptr) _Contract_Import_CoreLog_v1.Write(text);
    }

    void PlatformHex(void* context, uint64_t value) {
        (void)context;
        if (_Contract_Import_CoreLog_v1.Hex != nullptr) _Contract_Import_CoreLog_v1.Hex(value);
    }

    // 平台注入实例：字段在 Init 时按导入表就绪情况设置
    UsbCore::Platform g_platform = {&PlatformLine, &PlatformText, &PlatformHex, nullptr};

    // Usb.Host 槽 0：登记控制器
    KernelError SlotAddController(uint32_t* handleOut, const UsbControllerOps* ops, void* context) {
        return UsbCore::AddController(g_usb, g_platform, ops, context, handleOut);
    }

    // Usb.Host 槽 1：登记类驱动
    KernelError SlotRegisterClass(uint32_t classCode, const UsbClassOps* ops, void* context, uint32_t* handleOut) {
        return UsbCore::RegisterClass(g_usb, g_platform, classCode, ops, context, handleOut);
    }

    // Usb.Host 槽 2：控制传输
    KernelError SlotControlTransfer(uint32_t deviceHandle, const UsbSetupPacket* packet, void* buffer, uint32_t bytes,
                                    uint32_t* actual) {
        return UsbCore::ControlTransfer(g_usb, deviceHandle, packet, buffer, bytes, actual);
    }

    // Usb.Host 槽 3：批量传输
    KernelError SlotBulkTransfer(uint32_t deviceHandle, uint32_t endpoint, void* buffer, uint32_t bytes,
                                 uint32_t* actual) {
        return UsbCore::BulkTransfer(g_usb, deviceHandle, endpoint, buffer, bytes, actual);
    }

    // Usb.Host 槽 4：推进事件与枚举（budget 为 0 时取默认值，可重复调用）
    KernelError SlotPoll(uint32_t budget) { return UsbCore::Poll(g_usb, g_platform, budget); }

    // Usb.Host 槽 5：查询设备事实
    KernelError SlotDeviceFacts(uint32_t deviceHandle, UsbDeviceFacts* factsOut) {
        return UsbCore::DeviceFacts(g_usb, deviceHandle, factsOut);
    }
}

// 导出槽表：类型取生成头 UsbHostSlotsV1，槽序与 Interfaces.def 一致
LP_CONTRACT_EXPORT(UsbHost, 1, UsbHostSlotsV1) = {
    &SlotAddController, &SlotRegisterClass, &SlotControlTransfer, &SlotBulkTransfer, &SlotPoll, &SlotDeviceFacts};

// Init 入口：清空三张表并打印就绪标记；不依赖其它模块的 Init 顺序
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    UsbCore::Reset(g_usb);
    g_usb.initialized = 1;
    g_logReady = _Contract_Import_CoreLog_v1.Line != nullptr ? 1u : 0u;
    if (g_logReady != 0) _Contract_Import_CoreLog_v1.Line("USB", "usbcore: ready");
    return KernelError::kOk;
}

// Abort 入口：清空登记表（控制器与设备的状态由各自的驱动模块撤销）
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    UsbCore::Reset(g_usb);
    return KernelError::kOk;
}
