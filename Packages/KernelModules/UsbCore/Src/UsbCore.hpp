/* UsbCore.hpp
    USB 主机核心：控制器登记、类驱动登记（支持晚注册）、设备枚举状态机、控制/批量传输路由

    Usb.Host v1 的实现主体，契约槽在 Module.cpp 里转发到本文件的函数；本文件不直接依赖
    内核私有头，平台能力（日志）经 Platform 注入，宿主检查用假平台与假控制器 ops 逐项断言
    设备表 / 类驱动表 / 控制器表都是定长 POD，句柄 = 下标 + 1，0 永远无效
    枚举状态机在 Poll 里推进：端口有连接变化 → ResetPort → EnableDevice →
    GET_DESCRIPTOR(Device) → GET_DESCRIPTOR(Config) → 按接口 classCode 匹配类驱动 Probe/Bind
    Poll 可多次调用、幂等：只有端口状态变化或新设备才产生进展，没有进展时也返回 kOk
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

namespace UsbCore {
    // 表容量：引导闭包只有一条 xHCI 链，留一个余量
    constexpr uint32_t kMaxControllers = 2;
    constexpr uint32_t kMaxClasses = 4;
    constexpr uint32_t kMaxDevices = 16;
    // 控制器端口号上限：xHCI 的 MaxPorts 最多 255，这里按常见值截断
    constexpr uint32_t kMaxPortsPerController = 32;
    // 一次配置描述符读取的上限
    constexpr uint32_t kMaxConfigBytes = 256;
    // Poll 的默认预算：一次调用最多处理的端口事件数
    constexpr uint32_t kDefaultPollBudget = 16;
    // USB 请求：GET_DESCRIPTOR
    constexpr uint8_t kRequestGetDescriptor = 0x06;
    constexpr uint8_t kDescriptorTypeDevice = 0x01;
    constexpr uint8_t kDescriptorTypeConfiguration = 0x02;
    constexpr uint8_t kDescriptorTypeInterface = 0x04;
    // 标准设备请求的 bmRequestType：设备到主机、标准、接收者为设备
    constexpr uint8_t kRequestTypeDeviceIn = 0x80;
    // 设备描述符与配置描述符的前 9 字节即可拿到长度与类码
    constexpr uint32_t kDeviceDescriptorBytes = 18;
    constexpr uint32_t kConfigHeaderBytes = 9;
    // 端口状态位（UsbControllerOps.PortStatus 的编码，由控制器驱动与本文件共用）：
    //   bit0 连接（CCS）、bit1 连接变化（CSC，读清）、bit2 已启用（PED）、
    //   bit3 复位中（PR）、bit4..7 速度（0 未知、1 低速、2 全速、3 高速、4 超速）
    constexpr uint32_t kPortStatusPresent = 1u << 0;
    constexpr uint32_t kPortStatusChanged = 1u << 1;
    constexpr uint32_t kPortStatusEnabled = 1u << 2;
    constexpr uint32_t kPortStatusReset = 1u << 3;
    constexpr uint32_t kPortStatusSpeedShift = 4;
    constexpr uint32_t kPortStatusSpeedMask = 0x0Fu;

    // 平台能力注入：日志（三个回调都可为空，为空时静默）
    struct Platform {
        void (*logLine)(void* context, const char* tag, const char* text);  // 一整行带标签诊断
        void (*logText)(void* context, const char* text);                   // 原始文本（无换行）
        void (*logHex)(void* context, uint64_t value);                      // 16 位十六进制
        void* context;                                                      // 透传上下文
    };

    // 控制器记录：ops + context + 端口事实
    struct ControllerRecord {
        uint32_t used = 0;                     // 0 空闲，1 已登记
        uint32_t portCount = 0;                // 根端口数
        uint32_t presentPorts = 0;             // 已连接端口的位图（位 = 端口号 - 1）
        const UsbControllerOps* ops = nullptr; // 控制器回调表
        void* context = nullptr;               // 控制器实例上下文
    };

    // 类驱动记录：接口类码 → UsbClassOps
    struct ClassRecord {
        uint32_t used = 0;                 // 0 空闲，1 已登记
        uint32_t classCode = 0;            // 接口类码（bInterfaceClass）
        const UsbClassOps* ops = nullptr;  // 类驱动回调表
        void* context = nullptr;           // 类驱动实例上下文
    };

    // 设备记录：枚举事实、所属控制器、地址与绑定状态
    struct DeviceRecord {
        uint32_t used = 0;                  // 0 空闲，1 已枚举
        uint32_t bound = 0;                 // 0 未绑定类驱动，1 已绑定
        uint32_t port = 0;                  // 根端口号（从 1 起）
        uint32_t address = 0;               // 设备地址（xHCI 的 slot 号）
        ControllerHandle controller = 0;    // 所属控制器句柄
        ClassHandle classHandle = 0;        // 绑定到的类驱动句柄
        UsbDeviceFacts facts = {};          // 对外设备事实
    };

    // UsbCore 模块状态：三张定长表 + 计数
    struct State {
        ControllerRecord controllers[kMaxControllers] = {};
        ClassRecord classes[kMaxClasses] = {};
        DeviceRecord devices[kMaxDevices] = {};
        uint32_t controllerCount = 0;   // 已登记控制器数
        uint32_t classCount = 0;        // 已登记类驱动数
        uint32_t deviceCount = 0;       // 已枚举设备数
        uint32_t initialized = 0;       // Init 已调用标志
    };

    // 复位状态：Init / Abort / 宿主用例共用，清空全部表与计数
    void Reset(State& state);

    // Usb.Host 槽 0：登记控制器；portCount 从控制器 ops 读取并记录
    KernelError AddController(State& state, const Platform& platform, const UsbControllerOps* ops, void* context,
                              ControllerHandle* handleOut);
    // Usb.Host 槽 1：登记类驱动（接口类码 → ops）；对已枚举未绑定设备立即重扫绑定
    KernelError RegisterClass(State& state, const Platform& platform, uint32_t classCode, const UsbClassOps* ops,
                              void* context, ClassHandle* handleOut);
    // Usb.Host 槽 2：按设备句柄路由控制传输到所属控制器
    KernelError ControlTransfer(State& state, UsbDeviceHandle device, const UsbSetupPacket* packet, void* buffer,
                                uint32_t bytes, uint32_t* actual);
    // Usb.Host 槽 3：按设备句柄路由批量传输到所属控制器
    KernelError BulkTransfer(State& state, UsbDeviceHandle device, uint32_t endpoint, void* buffer, uint32_t bytes,
                             uint32_t* actual);
    // Usb.Host 槽 4：推进控制器事件与枚举状态机；budget 为 0 时取默认预算，可重复调用
    KernelError Poll(State& state, const Platform& platform, uint32_t budget);
    // Usb.Host 槽 5：查询设备事实
    KernelError DeviceFacts(State& state, UsbDeviceHandle device, UsbDeviceFacts* factsOut);
}
