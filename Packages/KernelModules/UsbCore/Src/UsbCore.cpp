/* UsbCore.cpp
    USB 主机核心的实现：控制器/类驱动/设备三张定长表、枚举状态机、传输路由

    枚举用标准控制传输读描述符：GET_DESCRIPTOR(Device) 取 idVendor/idProduct，
    GET_DESCRIPTOR(Configuration) 先读 9 字节头拿 wTotalLength，再读全量并解析
    bInterfaceClass/SubClass/Protocol；类驱动按接口类码匹配，Probe 返回接管后 Bind
    晚注册重扫：RegisterClass 时对已枚举未绑定设备立即尝试匹配，UsbHub/UsbStorage 在
    UsbCore 之后 Init 也能接上已枚举的设备
    Poll 的进展只有两种：新设备枚举完成、设备移除（含类驱动 Unbind 与 DisableDevice）；
    没有进展时照样返回 kOk，调用方可以反复调用
*/

#include "UsbCore.hpp"

namespace UsbCore {
    namespace {
        // 句柄与下标互转：句柄 = 下标 + 1
        uint32_t HandleOf(uint32_t index) { return index + 1; }

        // 0 表示无效句柄
        bool ValidHandle(uint32_t handle, uint32_t count) { return handle != 0 && handle <= count; }

        // 写一整行带标签诊断
        void Line(const Platform& platform, const char* tag, const char* text) {
            if (platform.logLine != nullptr) platform.logLine(platform.context, tag, text);
        }

        // 写原始文本
        void Text(const Platform& platform, const char* text) {
            if (platform.logText != nullptr) platform.logText(platform.context, text);
        }

        // 写十六进制
        void Hex(const Platform& platform, uint64_t value) {
            if (platform.logHex != nullptr) platform.logHex(platform.context, value);
        }

        // 打印一行 "前缀 + 十六进制 + 后缀"：标识行与失败 detail 共用
        void HexLine(const Platform& platform, const char* prefix, uint64_t value, const char* suffix) {
            Text(platform, prefix);
            Hex(platform, value);
            Text(platform, suffix);
        }

        // 找空闲的表项下标；表满返回容量值
        template <typename Record>
        uint32_t FindFree(Record* records, uint32_t capacity) {
            for (uint32_t index = 0; index < capacity; ++index) {
                if (records[index].used == 0) return index;
            }
            return capacity;
        }

        // 找某个控制器上某个根端口对应的设备；没有返回 kMaxDevices
        uint32_t FindDeviceByPort(const State& state, ControllerHandle controller, uint32_t port) {
            for (uint32_t index = 0; index < kMaxDevices; ++index) {
                const DeviceRecord& device = state.devices[index];
                if (device.used == 0) continue;
                if (device.controller != controller || device.port != port) continue;
                return index;
            }
            return kMaxDevices;
        }

        // 状态位里的速度字段
        uint32_t SpeedOf(uint32_t portStatus) {
            return (portStatus >> kPortStatusSpeedShift) & kPortStatusSpeedMask;
        }

        // 解析配置描述符并取出第一个接口描述符的类码三字段
        bool ParseInterfaceClass(const uint8_t* config, uint32_t bytes, uint32_t& classCode, uint32_t& subclass,
                                 uint32_t& protocol) {
            if (bytes < kConfigHeaderBytes) return false;
            const uint32_t total = static_cast<uint32_t>(config[2]) | (static_cast<uint32_t>(config[3]) << 8);
            uint32_t offset = static_cast<uint32_t>(config[0]);
            if (offset < kConfigHeaderBytes) offset = kConfigHeaderBytes;
            while (offset + 2 <= bytes && offset < total) {
                const uint32_t length = config[offset];
                const uint32_t type = config[offset + 1];
                if (length < 2) return false;
                if (type == kDescriptorTypeInterface && length >= 9 && offset + 9 <= bytes) {
                    classCode = config[offset + 5];
                    subclass = config[offset + 6];
                    protocol = config[offset + 7];
                    return true;
                }
                offset += length;
            }
            return false;
        }

        // 通过控制器发一条标准控制传输：packet 与 buffer 由调用方给
        KernelError RawControl(const ControllerRecord& controller, uint32_t address, const UsbSetupPacket& packet,
                               void* buffer, uint32_t bytes, uint32_t* actual) {
            if (controller.ops == nullptr || controller.ops->SubmitControl == nullptr) return KernelError::kState;
            uint32_t transferred = 0;
            const KernelError error = controller.ops->SubmitControl(
                controller.context, address, &packet, buffer, bytes, actual != nullptr ? actual : &transferred);
            return error;
        }

        // 为某个已枚举设备尝试匹配类驱动（定义在枚举函数之后）
        bool MatchAndBind(State& state, const Platform& platform, uint32_t deviceIndex);

        // 枚举一个端口上的新设备：复位 → 分配地址 → 启用 → 读描述符 → 记表 → 匹配类驱动
        bool EnumeratePort(State& state, const Platform& platform, uint32_t controllerIndex, uint32_t port,
                           uint32_t speed) {
            ControllerRecord& controller = state.controllers[controllerIndex];
            const ControllerHandle controllerHandle = HandleOf(controllerIndex);
            const uint32_t index = FindFree(state.devices, kMaxDevices);
            if (index == kMaxDevices) {
                Line(platform, "WARN", "usbcore: device table is full");
                return false;
            }
            const uint32_t address = HandleOf(index);
            if (controller.ops->PortReset != nullptr) {
                const KernelError reset = controller.ops->PortReset(controller.context, port);
                if (reset != KernelError::kOk) {
                    HexLine(platform, "usbcore: port reset failed error=", static_cast<uint64_t>(reset), "\r\n");
                    return false;
                }
            }
            if (controller.ops->EnableDevice == nullptr) return false;
            const KernelError enabled = controller.ops->EnableDevice(controller.context, port, address);
            if (enabled != KernelError::kOk) {
                HexLine(platform, "usbcore: enable device failed error=", static_cast<uint64_t>(enabled), "\r\n");
                return false;
            }
            // GET_DESCRIPTOR(Device)：18 字节，取 idVendor/idProduct
            uint8_t deviceDescriptor[kDeviceDescriptorBytes] = {};
            UsbSetupPacket packet = {};
            packet.requestType = kRequestTypeDeviceIn;
            packet.request = kRequestGetDescriptor;
            packet.value = static_cast<uint16_t>(kDescriptorTypeDevice << 8);
            packet.index = 0;
            packet.length = static_cast<uint16_t>(kDeviceDescriptorBytes);
            uint32_t actual = 0;
            if (RawControl(controller, address, packet, deviceDescriptor, kDeviceDescriptorBytes, &actual) != KernelError::kOk ||
                actual < 8) {
                Line(platform, "WARN", "usbcore: device descriptor failed");
                if (controller.ops->DisableDevice != nullptr) (void)controller.ops->DisableDevice(controller.context, address);
                return false;
            }
            const uint32_t vendorId = static_cast<uint32_t>(deviceDescriptor[8]) |
                                      (static_cast<uint32_t>(deviceDescriptor[9]) << 8);
            const uint32_t productId = static_cast<uint32_t>(deviceDescriptor[10]) |
                                       (static_cast<uint32_t>(deviceDescriptor[11]) << 8);
            // GET_DESCRIPTOR(Configuration)：先读头拿 wTotalLength，再读全量
            uint8_t config[kMaxConfigBytes] = {};
            packet.value = static_cast<uint16_t>(kDescriptorTypeConfiguration << 8);
            packet.length = static_cast<uint16_t>(kConfigHeaderBytes);
            if (RawControl(controller, address, packet, config, kConfigHeaderBytes, &actual) != KernelError::kOk ||
                actual < kConfigHeaderBytes) {
                Line(platform, "WARN", "usbcore: config descriptor header failed");
                if (controller.ops->DisableDevice != nullptr) (void)controller.ops->DisableDevice(controller.context, address);
                return false;
            }
            uint32_t total = static_cast<uint32_t>(config[2]) | (static_cast<uint32_t>(config[3]) << 8);
            if (total > kMaxConfigBytes) total = kMaxConfigBytes;
            if (total == 0) total = kConfigHeaderBytes;
            packet.length = static_cast<uint16_t>(total);
            if (RawControl(controller, address, packet, config, total, &actual) != KernelError::kOk) {
                Line(platform, "WARN", "usbcore: config descriptor failed");
                if (controller.ops->DisableDevice != nullptr) (void)controller.ops->DisableDevice(controller.context, address);
                return false;
            }
            uint32_t classCode = 0;
            uint32_t subclass = 0;
            uint32_t protocol = 0;
            if (!ParseInterfaceClass(config, actual < total ? actual : total, classCode, subclass, protocol)) {
                Line(platform, "WARN", "usbcore: interface descriptor missing");
                if (controller.ops->DisableDevice != nullptr) (void)controller.ops->DisableDevice(controller.context, address);
                return false;
            }
            DeviceRecord& device = state.devices[index];
            device.used = 1;
            device.bound = 0;
            device.port = port;
            device.address = address;
            device.controller = controllerHandle;
            device.classHandle = 0;
            device.facts.bytes = static_cast<uint32_t>(sizeof(UsbDeviceFacts));
            device.facts.deviceHandle = HandleOf(index);
            device.facts.controllerHandle = controllerHandle;
            device.facts.classCode = classCode;
            device.facts.subclass = subclass;
            device.facts.protocol = protocol;
            device.facts.vendorId = vendorId;
            device.facts.productId = productId;
            device.facts.speed = speed;
            device.facts.port = port;
            ++state.deviceCount;
            Line(platform, "USB", "usbcore: device enumerated");
            HexLine(platform, "usbcore: device class=", classCode, "\r\n");
            (void)MatchAndBind(state, platform, index);
            return true;
        }

        // 移除一个设备：有类驱动绑定时先 Unbind，再让控制器停用设备
        void RemoveDevice(State& state, const Platform& platform, uint32_t index) {
            DeviceRecord& device = state.devices[index];
            if (device.bound != 0) {
                const uint32_t classIndex = device.classHandle - 1;
                if (classIndex < kMaxClasses && state.classes[classIndex].ops != nullptr &&
                    state.classes[classIndex].ops->Unbind != nullptr) {
                    state.classes[classIndex].ops->Unbind(state.classes[classIndex].context, device.facts.deviceHandle);
                }
                device.bound = 0;
            }
            const uint32_t controllerIndex = device.controller - 1;
            if (controllerIndex < kMaxControllers) {
                const ControllerRecord& controller = state.controllers[controllerIndex];
                if (controller.ops != nullptr && controller.ops->DisableDevice != nullptr) {
                    (void)controller.ops->DisableDevice(controller.context, device.address);
                }
            }
            Line(platform, "USB", "usbcore: device removed");
            device = DeviceRecord{};
            --state.deviceCount;
        }

        // 为某个已枚举设备尝试匹配类驱动：Probe 返回接管后 Bind，成功置绑定状态
        bool MatchAndBind(State& state, const Platform& platform, uint32_t deviceIndex) {
            DeviceRecord& device = state.devices[deviceIndex];
            if (device.used == 0 || device.bound != 0) return false;
            for (uint32_t classIndex = 0; classIndex < kMaxClasses; ++classIndex) {
                ClassRecord& driver = state.classes[classIndex];
                if (driver.used == 0) continue;
                if (driver.classCode != device.facts.classCode) continue;
                if (driver.ops == nullptr) continue;
                uint32_t claimed = 0;
                if (driver.ops->Probe != nullptr) {
                    const KernelError probed = driver.ops->Probe(driver.context, &device.facts, &claimed);
                    if (probed != KernelError::kOk || claimed == 0) continue;
                }
                if (driver.ops->Bind != nullptr) {
                    const KernelError bound = driver.ops->Bind(driver.context, &device.facts, HandleOf(classIndex));
                    if (bound != KernelError::kOk) continue;
                }
                device.bound = 1;
                device.classHandle = HandleOf(classIndex);
                Line(platform, "USB", "usbcore: class bound");
                HexLine(platform, "usbcore: bound class=", driver.classCode, "\r\n");
                return true;
            }
            return false;
        }
    }

    void Reset(State& state) {
        for (uint32_t index = 0; index < kMaxControllers; ++index) state.controllers[index] = ControllerRecord{};
        for (uint32_t index = 0; index < kMaxClasses; ++index) state.classes[index] = ClassRecord{};
        for (uint32_t index = 0; index < kMaxDevices; ++index) state.devices[index] = DeviceRecord{};
        state.controllerCount = 0;
        state.classCount = 0;
        state.deviceCount = 0;
        state.initialized = 0;
    }

    KernelError AddController(State& state, const Platform& platform, const UsbControllerOps* ops, void* context,
                              ControllerHandle* handleOut) {
        if (ops == nullptr || handleOut == nullptr) return KernelError::kInvalidArgument;
        if (ops->SubmitControl == nullptr || ops->SubmitBulk == nullptr || ops->PortStatus == nullptr) {
            return KernelError::kInvalidArgument;
        }
        const uint32_t index = FindFree(state.controllers, kMaxControllers);
        if (index == kMaxControllers) return KernelError::kOutOfResources;
        ControllerRecord& controller = state.controllers[index];
        controller.used = 1;
        controller.ops = ops;
        controller.context = context;
        controller.portCount = ops->PortCount != nullptr ? ops->PortCount(context) : 0;
        if (controller.portCount > kMaxPortsPerController) controller.portCount = kMaxPortsPerController;
        controller.presentPorts = 0;
        ++state.controllerCount;
        *handleOut = HandleOf(index);
        Line(platform, "USB", "usbcore: controller added");
        HexLine(platform, "usbcore: controller ports=", controller.portCount, "\r\n");
        return KernelError::kOk;
    }

    KernelError RegisterClass(State& state, const Platform& platform, uint32_t classCode, const UsbClassOps* ops,
                              void* context, ClassHandle* handleOut) {
        if (ops == nullptr || handleOut == nullptr || classCode == 0) return KernelError::kInvalidArgument;
        const uint32_t index = FindFree(state.classes, kMaxClasses);
        if (index == kMaxClasses) return KernelError::kOutOfResources;
        ClassRecord& driver = state.classes[index];
        driver.used = 1;
        driver.classCode = classCode;
        driver.ops = ops;
        driver.context = context;
        ++state.classCount;
        *handleOut = HandleOf(index);
        Line(platform, "USB", "usbcore: class registered");
        HexLine(platform, "usbcore: class code=", classCode, "\r\n");
        // 晚注册：对已枚举但还没绑定的设备立即重扫
        for (uint32_t device = 0; device < kMaxDevices; ++device) {
            if (state.devices[device].used == 0 || state.devices[device].bound != 0) continue;
            if (state.devices[device].facts.classCode != classCode) continue;
            (void)MatchAndBind(state, platform, device);
        }
        return KernelError::kOk;
    }

    KernelError ControlTransfer(State& state, UsbDeviceHandle device, const UsbSetupPacket* packet, void* buffer,
                                uint32_t bytes, uint32_t* actual) {
        if (packet == nullptr || !ValidHandle(device, kMaxDevices)) return KernelError::kInvalidArgument;
        const DeviceRecord& record = state.devices[device - 1];
        if (record.used == 0 || record.controller == 0 || record.controller > kMaxControllers) {
            return KernelError::kInvalidArgument;
        }
        return RawControl(state.controllers[record.controller - 1], record.address, *packet, buffer, bytes, actual);
    }

    KernelError BulkTransfer(State& state, UsbDeviceHandle device, uint32_t endpoint, void* buffer, uint32_t bytes,
                             uint32_t* actual) {
        if (!ValidHandle(device, kMaxDevices)) return KernelError::kInvalidArgument;
        const DeviceRecord& record = state.devices[device - 1];
        if (record.used == 0 || record.controller == 0 || record.controller > kMaxControllers) {
            return KernelError::kInvalidArgument;
        }
        const ControllerRecord& controller = state.controllers[record.controller - 1];
        if (controller.ops == nullptr || controller.ops->SubmitBulk == nullptr) return KernelError::kState;
        uint32_t transferred = 0;
        return controller.ops->SubmitBulk(controller.context, record.address, endpoint, buffer, bytes,
                                          actual != nullptr ? actual : &transferred);
    }

    KernelError Poll(State& state, const Platform& platform, uint32_t budget) {
        if (budget == 0) budget = kDefaultPollBudget;
        for (uint32_t controllerIndex = 0; controllerIndex < kMaxControllers; ++controllerIndex) {
            ControllerRecord& controller = state.controllers[controllerIndex];
            if (controller.used == 0) continue;
            if (controller.ops != nullptr && controller.ops->Poll != nullptr) {
                (void)controller.ops->Poll(controller.context, budget);
            }
            if (controller.ops == nullptr || controller.ops->PortStatus == nullptr) continue;
            uint32_t portCount = controller.portCount;
            if (portCount > kMaxPortsPerController) portCount = kMaxPortsPerController;
            for (uint32_t port = 1; port <= portCount; ++port) {
                if (budget == 0) return KernelError::kOk;
                uint32_t status = 0;
                if (controller.ops->PortStatus(controller.context, port, &status) != KernelError::kOk) continue;
                const bool present = (status & kPortStatusPresent) != 0;
                const uint32_t bit = 1u << (port - 1);
                const bool known = (controller.presentPorts & bit) != 0;
                if (present && !known) {
                    if (EnumeratePort(state, platform, controllerIndex, port, SpeedOf(status))) {
                        controller.presentPorts |= bit;
                        --budget;
                    }
                } else if (!present && known) {
                    const uint32_t device = FindDeviceByPort(state, HandleOf(controllerIndex), port);
                    if (device < kMaxDevices) RemoveDevice(state, platform, device);
                    controller.presentPorts &= ~bit;
                    --budget;
                }
            }
        }
        return KernelError::kOk;
    }

    KernelError DeviceFacts(State& state, UsbDeviceHandle device, UsbDeviceFacts* factsOut) {
        if (factsOut == nullptr || !ValidHandle(device, kMaxDevices)) return KernelError::kInvalidArgument;
        const DeviceRecord& record = state.devices[device - 1];
        if (record.used == 0) return KernelError::kInvalidArgument;
        *factsOut = record.facts;
        return KernelError::kOk;
    }
}
