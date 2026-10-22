/* Xhci.cpp
    xHCI 主机控制器驱动的实现：CAP/运行寄存器校验、命令环/事件环/TRB、EP0 控制传输、批量端点

    TRB 与 PORTSC 的编码是纯函数（Xhci.hpp 声明），宿主检查直接断言位域；寄存器读写与 DMA
    只在 Initialize / 传输路径里发生，DMA 缓冲由 Platform 注入（目标侧 Core.Page + Core.PhysMap）
    命令与传输都是同步的：入队 → 写门铃 → 轮询事件环等对应事件 → 检查完成码，超时返回 kTimeout
    事件环只消费不产生；Poll 把端口状态变化事件读掉并推进事件出队指针，返回 kOk
*/

#include "Xhci.hpp"

namespace Xhci {
    namespace {
        // 按字节偏移访问 MMIO 的 32 位寄存器
        volatile uint32_t* At32(volatile uint32_t* base, uint32_t offset) {
            return reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base) + offset);
        }

        uint32_t ReadReg(volatile uint32_t* base, uint32_t offset) { return *At32(base, offset); }

        void WriteReg(volatile uint32_t* base, uint32_t offset, uint32_t value) { *At32(base, offset) = value; }

        // 64 位寄存器：xHCI 建议单次 64 位访问；拆成两次 32 位访问时必须先写低、再写高
        // （低字写入锁存、高字写入提交；反过来写高字不会提交，寄存器保持旧值——
        //   QEMU 的 xHCI 实现按此语义，写反时 CRCR/DCBAAP 读回正确但控制器拿到的仍是 0）
        void Write64(volatile uint32_t* base, uint32_t offset, uint64_t value) {
            WriteReg(base, offset, static_cast<uint32_t>(value));
            WriteReg(base, offset + 4, static_cast<uint32_t>(value >> 32));
        }

        // 操作寄存器基址：MMIO + CAPLENGTH
        volatile uint32_t* Operational(const Controller& controller) { return At32(controller.abar, controller.caplength); }

        // 日志字段：平台注入的日志可用时写 "前缀 + 十六进制"，用于失败诊断
        void LogField(Controller& controller, const char* prefix, uint64_t value) {
            if (controller.platform.logText != nullptr) controller.platform.logText(controller.platform.context, prefix);
            if (controller.platform.logHex != nullptr) controller.platform.logHex(controller.platform.context, value);
        }

        // 日志换行
        void LogNewline(Controller& controller) {
            if (controller.platform.logText != nullptr) controller.platform.logText(controller.platform.context, "\r\n");
        }

        // 运行寄存器基址：MMIO + RTSOFF
        volatile uint32_t* Runtime(const Controller& controller) { return At32(controller.abar, controller.rtsoff); }

        // 门铃数组基址：MMIO + DBOFF
        volatile uint32_t* Doorbells(const Controller& controller) { return At32(controller.abar, controller.dboff); }

        // 端口寄存器基址
        volatile uint32_t* PortRegister(const Controller& controller, uint32_t port) {
            return At32(Operational(controller), kPortBase + (port - 1) * kPortStride);
        }

        // 端口状态位 → UsbControllerOps.PortStatus 编码（UsbCore 的位定义）
        // 保持与 UsbCore.hpp 一致：bit0 连接、bit1 变化、bit2 已启用、bit3 复位中、bit4..7 速度
    }

    bool ReadCapabilities(volatile const uint32_t* abar, Capabilities& caps) {
        if (abar == nullptr) return false;
        const uint32_t hcsparams1 = abar[kHcsparams1 / 4];
        const uint32_t hccparams1 = abar[kHccparams1 / 4];
        caps.caplength = reinterpret_cast<const volatile uint8_t*>(abar)[kCapLength] & 0xFFu;
        caps.version = static_cast<uint32_t>(reinterpret_cast<const volatile uint8_t*>(abar)[kCapLength + 2]) |
                       (static_cast<uint32_t>(reinterpret_cast<const volatile uint8_t*>(abar)[kCapLength + 3]) << 8);
        caps.maxSlots = hcsparams1 & 0xFFu;
        caps.maxPorts = (hcsparams1 >> 24) & 0xFFu;
        caps.ac64 = hccparams1 & 1u;
        caps.contextBytes = ((hccparams1 >> 2) & 1u) != 0 ? 64u : 32u;
        caps.dboff = abar[kDboff / 4] & ~0x3u;
        caps.rtsoff = abar[kRtsoff / 4] & ~0x1Fu;
        if (caps.caplength < 0x20 || caps.maxSlots == 0 || caps.maxPorts == 0) return false;
        return true;
    }

    Trb MakeSetupTrb(uint32_t word0, uint32_t word1, uint32_t transferType, uint32_t cycle) {
        Trb trb;
        trb.dw0 = word0;
        trb.dw1 = word1;
        trb.dw2 = 8u;   // bmRequestType..wLength 共 8 字节
        // IDT=1（数据在 TRB 内）、type=Setup、TRT=transferType（0 无数据 / 2 OUT / 3 IN）
        trb.dw3 = (1u << 6) | (transferType << 16) | (kTrbSetup << 10) | (cycle & 1u);
        return trb;
    }

    Trb MakeDataTrb(uint64_t bufferPhysical, uint32_t bytes, bool deviceToHost, uint32_t cycle) {
        Trb trb;
        trb.dw0 = static_cast<uint32_t>(bufferPhysical);
        trb.dw1 = static_cast<uint32_t>(bufferPhysical >> 32);
        trb.dw2 = bytes & 0x1FFFFu;
        trb.dw3 = (deviceToHost ? (1u << 16) : 0u) | (kTrbData << 10) | (cycle & 1u);
        return trb;
    }

    Trb MakeStatusTrb(bool deviceToHost, uint32_t cycle) {
        Trb trb;
        trb.dw0 = 0;
        trb.dw1 = 0;
        trb.dw2 = 0;
        trb.dw3 = (deviceToHost ? (1u << 16) : 0u) | (1u << 5) | (kTrbStatus << 10) | (cycle & 1u);
        return trb;
    }

    Trb MakeNormalTrb(uint64_t bufferPhysical, uint32_t bytes, bool deviceToHost, uint32_t cycle) {
        (void)deviceToHost;
        Trb trb;
        trb.dw0 = static_cast<uint32_t>(bufferPhysical);
        trb.dw1 = static_cast<uint32_t>(bufferPhysical >> 32);
        trb.dw2 = bytes & 0x1FFFFu;
        trb.dw3 = (1u << 5) | (kTrbNormal << 10) | (cycle & 1u);
        return trb;
    }

    Trb MakeLinkTrb(uint64_t ringPhysical, uint32_t cycle) {
        Trb trb;
        trb.dw0 = static_cast<uint32_t>(ringPhysical);
        trb.dw1 = static_cast<uint32_t>(ringPhysical >> 32);
        trb.dw2 = 0;
        trb.dw3 = (1u << 1) | (kTrbLink << 10) | (cycle & 1u);   // TC=1：回绕后切换 cycle
        return trb;
    }

    Trb MakeEnableSlotTrb(uint32_t cycle) {
        Trb trb;
        trb.dw3 = (kTrbEnableSlot << 10) | (cycle & 1u);
        return trb;
    }

    Trb MakeDisableSlotTrb(uint32_t slotId, uint32_t cycle) {
        Trb trb;
        trb.dw3 = ((slotId & 0xFFu) << 24) | (kTrbDisableSlot << 10) | (cycle & 1u);
        return trb;
    }

    Trb MakeAddressDeviceTrb(uint64_t inputContextPhysical, uint32_t slotId, uint32_t cycle) {
        Trb trb;
        trb.dw0 = static_cast<uint32_t>(inputContextPhysical);
        trb.dw1 = static_cast<uint32_t>(inputContextPhysical >> 32);
        trb.dw3 = ((slotId & 0xFFu) << 24) | (kTrbAddressDevice << 10) | (cycle & 1u);
        return trb;
    }

    Trb MakeConfigureEndpointTrb(uint64_t inputContextPhysical, uint32_t slotId, uint32_t cycle) {
        Trb trb;
        trb.dw0 = static_cast<uint32_t>(inputContextPhysical);
        trb.dw1 = static_cast<uint32_t>(inputContextPhysical >> 32);
        trb.dw3 = ((slotId & 0xFFu) << 24) | (kTrbConfigureEndpoint << 10) | (cycle & 1u);
        return trb;
    }

    uint32_t TrbSlot(const Trb& trb) { return (trb.dw3 >> 24) & 0xFFu; }

    uint32_t TrbEndpoint(const Trb& trb) { return (trb.dw3 >> 16) & 0x1Fu; }

    uint32_t TrbCompletionCode(const Trb& trb) { return (trb.dw2 >> 24) & 0xFFu; }

    uint32_t PortscOffset(uint32_t port) { return kPortBase + (port - 1) * kPortStride; }

    uint32_t ContextOffset(uint32_t contextIndex, uint32_t contextBytes) { return contextIndex * contextBytes; }

    uint32_t EndpointAddFlag(uint32_t dci) { return dci < 32 ? (1u << dci) : 0; }

    uint32_t SpeedFromPortsc(uint32_t portsc) {
        const uint32_t psiv = (portsc >> kPortscSpeedShift) & kPortscSpeedMask;
        switch (psiv) {
            case 1: return kSpeedFull;    // 全速
            case 2: return kSpeedLow;     // 低速
            case 3: return kSpeedHigh;    // 高速
            case 4: return kSpeedSuper;   // 超速
            default: return 0;            // 未知
        }
    }

    uint32_t PortStatusFromPortsc(uint32_t portsc) {
        uint32_t status = 0;
        if ((portsc & kPortscCcs) != 0) status |= (1u << 0);    // 连接（UsbCore 的 Present）
        if ((portsc & (kPortscCsc | kPortscPrc | kPortscPlc | kPortscCec)) != 0) status |= (1u << 1);   // 变化
        if ((portsc & kPortscPed) != 0) status |= (1u << 2);    // 已启用
        if ((portsc & kPortscPr) != 0) status |= (1u << 3);     // 复位中
        status |= SpeedFromPortsc(portsc) << 4;                 // 速度
        return status;
    }

    namespace {
        // —— 寄存器与环操作 ——

        // 小端写 32 位到字节缓冲
        void Put32(uint8_t* target, uint32_t value) {
            target[0] = static_cast<uint8_t>(value);
            target[1] = static_cast<uint8_t>(value >> 8);
            target[2] = static_cast<uint8_t>(value >> 16);
            target[3] = static_cast<uint8_t>(value >> 24);
        }

        // 清一段字节缓冲
        void ZeroBytes(void* target, uint32_t bytes) {
            uint8_t* bytesTarget = static_cast<uint8_t*>(target);
            for (uint32_t index = 0; index < bytes; ++index) bytesTarget[index] = 0;
        }

        // 往环里写一个 TRB，并按下标推进；写到 Link 位置时写 Link TRB 并切换 cycle
        void Enqueue(void* ring, uint64_t ringPhysical, uint32_t& index, uint32_t& cycle, const Trb& trb,
                     uint32_t* outPhysical) {
            uint8_t* bytes = static_cast<uint8_t*>(ring);
            Put32(bytes + index * 16 + 0, trb.dw0);
            Put32(bytes + index * 16 + 4, trb.dw1);
            Put32(bytes + index * 16 + 8, trb.dw2);
            Put32(bytes + index * 16 + 12, trb.dw3);
            if (outPhysical != nullptr) *outPhysical = static_cast<uint32_t>(ringPhysical + index * 16);
            ++index;
            if (index == kRingLinkIndex) {
                // 写 Link TRB：指向环首、TC=1、当前 cycle；随后切换 cycle 并回到环首
                Trb link = MakeLinkTrb(ringPhysical, cycle);
                Put32(bytes + kRingLinkIndex * 16 + 0, link.dw0);
                Put32(bytes + kRingLinkIndex * 16 + 4, link.dw1);
                Put32(bytes + kRingLinkIndex * 16 + 8, link.dw2);
                Put32(bytes + kRingLinkIndex * 16 + 12, link.dw3);
                cycle ^= 1u;
                index = 0;
            }
        }

        // 读事件环下一个 TRB：cycle 不符表示空
        bool DequeueEvent(Controller& controller, Trb& trb) {
            uint8_t* bytes = reinterpret_cast<uint8_t*>(controller.eventRingVirtual);
            const uint32_t offset = controller.eventIndex * 16;
            const uint32_t dw3 = *reinterpret_cast<const uint32_t*>(bytes + offset + 12);
            if ((dw3 & 1u) != controller.eventCycle) return false;
            trb.dw0 = *reinterpret_cast<const uint32_t*>(bytes + offset + 0);
            trb.dw1 = *reinterpret_cast<const uint32_t*>(bytes + offset + 4);
            trb.dw2 = *reinterpret_cast<const uint32_t*>(bytes + offset + 8);
            trb.dw3 = dw3;
            ++controller.eventIndex;
            if (controller.eventIndex == kRingTrbs) {
                controller.eventIndex = 0;
                controller.eventCycle ^= 1u;
            }
            // 更新 ERDP：出队指针、EHB 清 0（本批事件已处理完）
            volatile uint32_t* runtime = Runtime(controller);
            Write64(runtime, kInterrupter0Base + kErdp, controller.eventRingPhysical + controller.eventIndex * 16ull);
            return true;
        }

        // 等一个指定类型的事件；trbPointer 非零时要求事件里回指的是该 TRB 物理地址
        KernelError WaitEvent(Controller& controller, uint32_t expectedType, uint32_t trbPointer, Trb& out) {
            for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
                Trb trb;
                if (!DequeueEvent(controller, trb)) {
                    asm volatile("pause");
                    continue;
                }
                const uint32_t type = (trb.dw3 >> 10) & 0x3Fu;
                if (type != expectedType) continue;
                if (trbPointer != 0 && trb.dw0 != trbPointer) continue;
                out = trb;
                return KernelError::kOk;
            }
            return KernelError::kTimeout;
        }

        // 发一条命令并等命令完成事件；失败返回 kTimeout / kIo
        KernelError RunCommand(Controller& controller, const Trb& command, Trb* completionOut) {
            uint32_t trbPhysical = 0;
            Enqueue(controller.commandRingVirtual, controller.commandRingPhysical, controller.commandIndex,
                    controller.commandCycle, command, &trbPhysical);
            // 命令环门铃：门铃 0，目标 0
            *At32(Doorbells(controller), 0) = 0;
            Trb completion;
            const KernelError waited = WaitEvent(controller, kTrbCommandCompletion, trbPhysical, completion);
            if (waited != KernelError::kOk) {
                LogField(controller, "xhci: command timeout type=", (command.dw3 >> 10) & 0x3Fu);
                LogField(controller, " usbsts=", ReadReg(Operational(controller), kUsbSts));
                LogNewline(controller);
                return waited;
            }
            if (completionOut != nullptr) *completionOut = completion;
            if (TrbCompletionCode(completion) != kCompletionSuccess) return KernelError::kIo;
            return KernelError::kOk;
        }

        // 等某个传输 TD 的完成事件：成功返回 0 并带回实际长度
        KernelError WaitTransfer(Controller& controller, uint32_t lastTrbPhysical, uint32_t bytes, uint32_t* actual) {
            Trb event;
            const KernelError waited = WaitEvent(controller, kTrbTransferEvent, lastTrbPhysical, event);
            if (waited != KernelError::kOk) {
                LogField(controller, "xhci: transfer timeout trb=", lastTrbPhysical);
                LogField(controller, " usbsts=", ReadReg(Operational(controller), kUsbSts));
                LogNewline(controller);
                return waited;
            }
            if (TrbCompletionCode(event) != kCompletionSuccess) {
                LogField(controller, "xhci: transfer completion code=", TrbCompletionCode(event));
                LogField(controller, " residual=", event.dw2 & 0xFFFFFFu);
                LogNewline(controller);
                return KernelError::kIo;
            }
            const uint32_t residual = event.dw2 & 0xFFFFFFu;
            if (actual != nullptr) *actual = residual <= bytes ? bytes - residual : 0;
            return KernelError::kOk;
        }

        // 设备上下文与输入上下文的指针换算：上下文大小由 CSZ 决定（32 或 64 字节）
        uint8_t* ContextAt(void* base, uint32_t index, uint32_t contextBytes) {
            return static_cast<uint8_t*>(base) + ContextOffset(index, contextBytes);
        }

        // 填输入上下文的 Slot Context：速度、根端口、上下文条目数
        void FillSlotContext(uint8_t* inputContext, uint32_t contextBytes, uint32_t speed, uint32_t port,
                             uint32_t contextEntries) {
            uint8_t* slot = ContextAt(inputContext, 1, contextBytes);   // 0 是输入控制上下文
            ZeroBytes(slot, contextBytes);
            const uint32_t speedCode = speed == kSpeedLow ? 2u : speed == kSpeedFull ? 1u : speed == kSpeedHigh ? 3u : 4u;
            Put32(slot + 0, (speedCode << 20) | ((contextEntries & 0x1Fu) << 27));
            Put32(slot + 4, (port & 0xFFu) << 16);   // 根端口号
        }

        // 填输入上下文的 EP0 上下文：控制端点、最大包长、传输环
        void FillEp0Context(uint8_t* inputContext, uint32_t contextBytes, uint64_t ringPhysical, uint32_t maxPacket) {
            uint8_t* endpoint = ContextAt(inputContext, 2, contextBytes);
            ZeroBytes(endpoint, contextBytes);
            Put32(endpoint + 0, 0u);   // Interval = 0，Mult = 0
            // DW1：CErr = 3（控制端点的错误重试次数）、EP Type = Control(4)、Max Packet Size 按速度
            Put32(endpoint + 4, 3u | (4u << 3) | (maxPacket << 16));
            Put32(endpoint + 8, static_cast<uint32_t>(ringPhysical | 1u));   // DCS = 1
            Put32(endpoint + 12, static_cast<uint32_t>(ringPhysical >> 32));
            Put32(endpoint + 16, 8u);   // Average TRB Length
            Put32(endpoint + 20, 0u);
        }

        // 填输入控制上下文：DW0 是 Drop Context Flags（Address Device 必须为 0），DW1 是 Add Context Flags
        // （A0 槽上下文、A1 EP0、A(n+1) 端点上下文 index n）
        void FillInputControl(uint8_t* inputContext, uint32_t addFlags) {
            Put32(inputContext + 0, 0);
            Put32(inputContext + 4, addFlags);
        }

        // 为槽位分配 EP0 传输环（一页）并写 Link TRB
        KernelError AllocateEp0Ring(Controller& controller, SlotRecord& slot) {
            if (slot.ep0Virtual != nullptr) return KernelError::kOk;
            uint64_t physical = 0;
            void* virtualAddress = controller.platform.allocatePages(controller.platform.context, 1, &physical);
            if (virtualAddress == nullptr || physical == 0) return KernelError::kOutOfResources;
            slot.ep0Physical = physical;
            slot.ep0Virtual = virtualAddress;
            slot.ep0Index = 0;
            slot.ep0Cycle = 1;
            ZeroBytes(virtualAddress, controller.pageBytes);
            const Trb link = MakeLinkTrb(physical, 1);
            uint8_t* bytes = static_cast<uint8_t*>(virtualAddress);
            Put32(bytes + kRingLinkIndex * 16 + 0, link.dw0);
            Put32(bytes + kRingLinkIndex * 16 + 4, link.dw1);
            Put32(bytes + kRingLinkIndex * 16 + 8, link.dw2);
            Put32(bytes + kRingLinkIndex * 16 + 12, link.dw3);
            return KernelError::kOk;
        }

        // 为槽位分配设备上下文（DCBAA[slotId] 指向的物理地址）
        KernelError AllocateDeviceContext(Controller& controller, SlotRecord& slot) {
            uint64_t physical = 0;
            void* virtualAddress = controller.platform.allocatePages(controller.platform.context, 1, &physical);
            if (virtualAddress == nullptr || physical == 0) return KernelError::kOutOfResources;
            ZeroBytes(virtualAddress, controller.pageBytes);
            uint8_t* dcbaa = static_cast<uint8_t*>(controller.dcbaaVirtual);
            Put32(dcbaa + slot.slotId * 8 + 0, static_cast<uint32_t>(physical));
            Put32(dcbaa + slot.slotId * 8 + 4, static_cast<uint32_t>(physical >> 32));
            slot.deviceContextPhysical = physical;
            slot.deviceContextVirtual = virtualAddress;
            return KernelError::kOk;
        }

        // 端口写 1 清变化位
        void ClearPortChanges(Controller& controller, uint32_t port) {
            volatile uint32_t* portreg = PortRegister(controller, port);
            const uint32_t portsc = ReadReg(portreg, 0);
            WriteReg(portreg, 0, portsc | kPortscChangeBits);
        }
    }

    KernelError AttachPlatform(Controller& controller, const Platform& platform, uint32_t pageBytes) {
        controller.platform = platform;
        controller.pageBytes = pageBytes;
        return KernelError::kOk;
    }

    void Shutdown(Controller& controller) {
        if (controller.abar != nullptr) {
            volatile uint32_t* operational = Operational(controller);
            WriteReg(operational, kUsbCmd, ReadReg(operational, kUsbCmd) & ~kCmdRun);
            for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
                if ((ReadReg(operational, kUsbSts) & kStsHalted) != 0) break;
            }
        }
        for (uint32_t index = 0; index < kMaxSlots; ++index) {
            SlotRecord& slot = controller.slots[index];
            if (slot.ep0Virtual != nullptr && controller.platform.freePages != nullptr) {
                controller.platform.freePages(controller.platform.context, slot.ep0Physical, 1);
            }
            if (slot.deviceContextVirtual != nullptr && controller.platform.freePages != nullptr) {
                controller.platform.freePages(controller.platform.context, slot.deviceContextPhysical, 1);
            }
            for (uint32_t endpoint = 0; endpoint < kMaxEndpoints; ++endpoint) {
                EndpointRing& ring = slot.endpoints[endpoint];
                if (ring.used != 0 && ring.virtualAddress != nullptr && controller.platform.freePages != nullptr) {
                    controller.platform.freePages(controller.platform.context, ring.physical, 1);
                }
            }
            slot = SlotRecord{};
        }
        const uint64_t pages[7] = {controller.commandRingPhysical, controller.eventRingPhysical, controller.erstPhysical,
                                   controller.dcbaaPhysical, controller.inputContextPhysical, controller.dataPhysical};
        void* virtuals[7] = {controller.commandRingVirtual, controller.eventRingVirtual, controller.erstVirtual,
                             controller.dcbaaVirtual, controller.inputContextVirtual, controller.dataVirtual};
        for (uint32_t index = 0; index < 6; ++index) {
            if (virtuals[index] != nullptr && controller.platform.freePages != nullptr) {
                controller.platform.freePages(controller.platform.context, pages[index], 1);
            }
        }
        controller = Controller{};
    }

    KernelError Initialize(Controller& controller) {
        if (controller.abar == nullptr || controller.platform.allocatePages == nullptr) return KernelError::kInvalidArgument;
        Capabilities caps;
        if (!ReadCapabilities(controller.abar, caps)) return KernelError::kNotSupported;
        controller.caplength = caps.caplength;
        controller.maxPorts = caps.maxPorts > kMaxPorts ? kMaxPorts : caps.maxPorts;
        controller.maxSlots = caps.maxSlots > kMaxSlots ? kMaxSlots : caps.maxSlots;
        controller.contextBytes = caps.contextBytes;
        controller.dboff = caps.dboff;
        controller.rtsoff = caps.rtsoff;
        volatile uint32_t* operational = Operational(controller);
        // 等控制器脱离复位，再执行 HCRST
        for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
            if ((ReadReg(operational, kUsbSts) & kStsNotReady) == 0) break;
        }
        WriteReg(operational, kUsbCmd, ReadReg(operational, kUsbCmd) & ~kCmdRun);
        for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
            if ((ReadReg(operational, kUsbSts) & kStsHalted) != 0) break;
        }
        WriteReg(operational, kUsbCmd, ReadReg(operational, kUsbCmd) | kCmdReset);
        bool resetDone = false;
        for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
            if ((ReadReg(operational, kUsbCmd) & kCmdReset) == 0) {
                resetDone = true;
                break;
            }
        }
        if (!resetDone) return KernelError::kTimeout;
        // 复位完成后控制器应处于停机态（HCHalted=1）；CNR 可能在启动后才清，不在这里阻塞
        for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
            if ((ReadReg(operational, kUsbSts) & kStsHalted) != 0) break;
        }
        // 分配命令环 / 事件环 / 事件环段表 / DCBAA / 输入上下文 / 数据弹跳页
        uint64_t commandPhysical = 0;
        void* commandVirtual = controller.platform.allocatePages(controller.platform.context, 1, &commandPhysical);
        if (commandVirtual == nullptr || commandPhysical == 0) return KernelError::kOutOfResources;
        controller.commandRingPhysical = commandPhysical;
        controller.commandRingVirtual = commandVirtual;
        ZeroBytes(commandVirtual, controller.pageBytes);
        const Trb commandLink = MakeLinkTrb(commandPhysical, 1);
        uint8_t* commandBytes = static_cast<uint8_t*>(commandVirtual);
        Put32(commandBytes + kRingLinkIndex * 16 + 0, commandLink.dw0);
        Put32(commandBytes + kRingLinkIndex * 16 + 4, commandLink.dw1);
        Put32(commandBytes + kRingLinkIndex * 16 + 8, commandLink.dw2);
        Put32(commandBytes + kRingLinkIndex * 16 + 12, commandLink.dw3);

        uint64_t eventPhysical = 0;
        void* eventVirtual = controller.platform.allocatePages(controller.platform.context, 1, &eventPhysical);
        if (eventVirtual == nullptr || eventPhysical == 0) return KernelError::kOutOfResources;
        controller.eventRingPhysical = eventPhysical;
        controller.eventRingVirtual = eventVirtual;
        ZeroBytes(eventVirtual, controller.pageBytes);

        uint64_t erstPhysical = 0;
        void* erstVirtual = controller.platform.allocatePages(controller.platform.context, 1, &erstPhysical);
        if (erstVirtual == nullptr || erstPhysical == 0) return KernelError::kOutOfResources;
        controller.erstPhysical = erstPhysical;
        controller.erstVirtual = erstVirtual;
        ZeroBytes(erstVirtual, controller.pageBytes);
        uint8_t* erst = static_cast<uint8_t*>(erstVirtual);
        Put32(erst + 0, static_cast<uint32_t>(eventPhysical));   // 事件环段基址低
        Put32(erst + 4, static_cast<uint32_t>(eventPhysical >> 32));
        Put32(erst + 8, kRingTrbs);                              // 段大小（TRB 数）

        uint64_t dcbaaPhysical = 0;
        void* dcbaaVirtual = controller.platform.allocatePages(controller.platform.context, 1, &dcbaaPhysical);
        if (dcbaaVirtual == nullptr || dcbaaPhysical == 0) return KernelError::kOutOfResources;
        controller.dcbaaPhysical = dcbaaPhysical;
        controller.dcbaaVirtual = dcbaaVirtual;
        ZeroBytes(dcbaaVirtual, controller.pageBytes);

        uint64_t inputPhysical = 0;
        void* inputVirtual = controller.platform.allocatePages(controller.platform.context, 1, &inputPhysical);
        if (inputVirtual == nullptr || inputPhysical == 0) return KernelError::kOutOfResources;
        controller.inputContextPhysical = inputPhysical;
        controller.inputContextVirtual = inputVirtual;
        ZeroBytes(inputVirtual, controller.pageBytes);

        uint64_t dataPhysical = 0;
        void* dataVirtual = controller.platform.allocatePages(controller.platform.context, 1, &dataPhysical);
        if (dataVirtual == nullptr || dataPhysical == 0) return KernelError::kOutOfResources;
        controller.dataPhysical = dataPhysical;
        controller.dataVirtual = dataVirtual;
        ZeroBytes(dataVirtual, controller.pageBytes);

        // CONFIG：MaxSlotsEn 取本驱动支持的槽位；DCBAAP；CRCR（命令环 + RCS=1）
        WriteReg(operational, kConfig, controller.maxSlots & 0xFFu);
        Write64(operational, kDcbaap, dcbaaPhysical);
        Write64(operational, kCrcr, commandPhysical | 1u);   // RCS = 1
        // 事件环：ERSTSZ / ERSTBA / ERDP；关闭中断（本驱动只轮询）
        volatile uint32_t* runtime = Runtime(controller);
        WriteReg(runtime, kInterrupter0Base + kErstsz, 1u);
        Write64(runtime, kInterrupter0Base + kErstba, erstPhysical);
        Write64(runtime, kInterrupter0Base + kErdp, eventPhysical);   // EHB = 0：事件处理者空闲
        WriteReg(runtime, kInterrupter0Base + kIman, ReadReg(runtime, kInterrupter0Base + kIman) & ~(1u << 1));   // IMAN.IE = 0
        WriteReg(runtime, kInterrupter0Base + kImod, 0u);                                                          // IMOD = 0
        WriteReg(operational, kUsbSts, 0x1Fu);                          // 清 USBSTS 挂起位（EINT/PCD 等）
        // 启动控制器
        WriteReg(operational, kUsbCmd, ReadReg(operational, kUsbCmd) | kCmdRun);
        for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
            if ((ReadReg(operational, kUsbSts) & kStsHalted) == 0) {
                controller.initialized = 1;
                break;
            }
        }
        if (controller.initialized == 0) return KernelError::kTimeout;
        if (controller.platform.logLine != nullptr) controller.platform.logLine(controller.platform.context, "XHCI", "xhci: controller running");
        return KernelError::kOk;
    }

    namespace {
        // 按 UsbCore 给的设备地址找槽位
        SlotRecord* FindSlotByAddress(Controller& controller, uint32_t address) {
            for (uint32_t index = 0; index < kMaxSlots; ++index) {
                if (controller.slots[index].used != 0 && controller.slots[index].address == address) {
                    return &controller.slots[index];
                }
            }
            return nullptr;
        }

        // 找非 EP0 端点环；endpoint 是 USB 端点地址（bit7 方向、bit0..3 端点号）
        EndpointRing* FindEndpointRing(SlotRecord& slot, uint32_t endpoint) {
            for (uint32_t index = 0; index < kMaxEndpoints; ++index) {
                if (slot.endpoints[index].used != 0 && slot.endpoints[index].endpoint == endpoint) {
                    return &slot.endpoints[index];
                }
            }
            return nullptr;
        }

        // 端点地址 → 设备上下文索引（DCI）：端点号 * 2 + 方向（IN 为 1）
        uint32_t DciOf(uint32_t endpoint) {
            const uint32_t number = endpoint & 0x0Fu;
            const uint32_t direction = (endpoint & 0x80u) != 0 ? 1u : 0u;
            return number * 2u + direction;
        }

        // 为该端点分配一页传输环并写 Link TRB
        EndpointRing* AllocateEndpointRing(Controller& controller, SlotRecord& slot, uint32_t endpoint) {
            if (FindEndpointRing(slot, endpoint) != nullptr) return FindEndpointRing(slot, endpoint);
            for (uint32_t index = 0; index < kMaxEndpoints; ++index) {
                EndpointRing& ring = slot.endpoints[index];
                if (ring.used != 0) continue;
                uint64_t physical = 0;
                void* virtualAddress = controller.platform.allocatePages(controller.platform.context, 1, &physical);
                if (virtualAddress == nullptr || physical == 0) return nullptr;
                ZeroBytes(virtualAddress, controller.pageBytes);
                const Trb link = MakeLinkTrb(physical, 1);
                uint8_t* bytes = static_cast<uint8_t*>(virtualAddress);
                Put32(bytes + kRingLinkIndex * 16 + 0, link.dw0);
                Put32(bytes + kRingLinkIndex * 16 + 4, link.dw1);
                Put32(bytes + kRingLinkIndex * 16 + 8, link.dw2);
                Put32(bytes + kRingLinkIndex * 16 + 12, link.dw3);
                ring.used = 1;
                ring.endpoint = endpoint;
                ring.physical = physical;
                ring.virtualAddress = virtualAddress;
                ring.index = 0;
                ring.cycle = 1;
                return &ring;
            }
            return nullptr;
        }

        // 批量端点上下文：类型按方向（Bulk IN 6 / Bulk OUT 2）、最大包长按速度、CErr = 3
        void FillBulkEndpointContext(uint8_t* inputContext, uint32_t contextBytes, uint32_t dci, uint32_t speed,
                                     uint64_t ringPhysical) {
            uint8_t* endpoint = ContextAt(inputContext, 1u + dci, contextBytes);
            ZeroBytes(endpoint, contextBytes);
            const uint32_t maxPacket = speed == kSpeedSuper ? kMaxPacketSuper : kMaxPacketOther;
            const uint32_t direction = (dci & 1u);
            const uint32_t type = direction != 0 ? 6u : 2u;
            Put32(endpoint + 0, 0u);
            Put32(endpoint + 4, 3u | (type << 3) | (maxPacket << 16));
            Put32(endpoint + 8, static_cast<uint32_t>(ringPhysical | 1u));
            Put32(endpoint + 12, static_cast<uint32_t>(ringPhysical >> 32));
            Put32(endpoint + 16, 8u);
            Put32(endpoint + 20, 0u);
        }

        // 配置端点：输入上下文里加 slot 与目标端点，下发 Configure Endpoint
        KernelError ConfigureEndpoint(Controller& controller, SlotRecord& slot, uint32_t endpoint) {
            EndpointRing* ring = AllocateEndpointRing(controller, slot, endpoint);
            if (ring == nullptr) return KernelError::kOutOfResources;
            const uint32_t dci = DciOf(endpoint);
            // Add Context Flags：A0 槽、A1 EP0、A(i) 对应端点上下文 index i（DCI i）；可加的最高 DCI 决定 Context Entries
            const uint32_t addFlags = kAddSlotContextFlag | EndpointAddFlag(dci);
            FillInputControl(static_cast<uint8_t*>(controller.inputContextVirtual), addFlags);
            FillSlotContext(static_cast<uint8_t*>(controller.inputContextVirtual), controller.contextBytes, slot.speed,
                            slot.port, dci);
            FillBulkEndpointContext(static_cast<uint8_t*>(controller.inputContextVirtual), controller.contextBytes, dci,
                                    slot.speed, ring->physical);
            return RunCommand(controller,
                              MakeConfigureEndpointTrb(controller.inputContextPhysical, slot.slotId, controller.commandCycle),
                              nullptr);
        }

        // 按端点环推进 doorbell：门铃号是槽位号，目标值是 DCI
        void RingDoorbell(Controller& controller, uint32_t slotId, uint32_t target) {
            *At32(Doorbells(controller), slotId * 4u) = target;
        }
    }

    uint32_t PortCount(Controller& controller) { return controller.maxPorts; }

    KernelError PortStatus(Controller& controller, uint32_t port, uint32_t* status) {
        if (status == nullptr || port == 0 || port > controller.maxPorts) return KernelError::kInvalidArgument;
        volatile uint32_t* portreg = PortRegister(controller, port);
        const uint32_t portsc = ReadReg(portreg, 0);
        *status = PortStatusFromPortsc(portsc);
        if ((portsc & kPortscChangeBits) != 0) WriteReg(portreg, 0, portsc | kPortscChangeBits);   // 写 1 清变化位
        return KernelError::kOk;
    }

    KernelError PortReset(Controller& controller, uint32_t port) {
        if (port == 0 || port > controller.maxPorts) return KernelError::kInvalidArgument;
        volatile uint32_t* portreg = PortRegister(controller, port);
        WriteReg(portreg, 0, ReadReg(portreg, 0) | kPortscPr);
        for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
            if ((ReadReg(portreg, 0) & kPortscPr) == 0) break;
        }
        bool enabled = false;
        for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
            if ((ReadReg(portreg, 0) & kPortscPed) != 0) {
                enabled = true;
                break;
            }
        }
        ClearPortChanges(controller, port);
        return enabled ? KernelError::kOk : KernelError::kTimeout;
    }

    KernelError EnableDeviceOp(Controller& controller, uint32_t port, uint32_t deviceAddress) {
        if (port == 0 || port > controller.maxPorts || deviceAddress == 0) return KernelError::kInvalidArgument;
        SlotRecord* freeSlot = nullptr;
        for (uint32_t index = 0; index < kMaxSlots; ++index) {
            if (controller.slots[index].used == 0) {
                freeSlot = &controller.slots[index];
                break;
            }
        }
        if (freeSlot == nullptr) return KernelError::kOutOfResources;
        // Enable Slot：拿到槽位号
        Trb completion;
        KernelError error = RunCommand(controller, MakeEnableSlotTrb(controller.commandCycle), &completion);
        if (error != KernelError::kOk) {
            LogField(controller, "xhci: enable slot failed error=", static_cast<uint64_t>(error));
            LogNewline(controller);
            return error;
        }
        const uint32_t slotId = TrbSlot(completion);
        if (slotId == 0 || slotId > controller.maxSlots) return KernelError::kIo;
        freeSlot->used = 1;
        freeSlot->slotId = slotId;
        freeSlot->port = port;
        freeSlot->address = deviceAddress;
        const uint32_t portsc = ReadReg(PortRegister(controller, port), 0);
        freeSlot->speed = SpeedFromPortsc(portsc);
        if (AllocateEp0Ring(controller, *freeSlot) != KernelError::kOk) return KernelError::kOutOfResources;
        if (AllocateDeviceContext(controller, *freeSlot) != KernelError::kOk) return KernelError::kOutOfResources;
        // Address Device：输入上下文（slot + EP0）
        FillInputControl(static_cast<uint8_t*>(controller.inputContextVirtual), kAddSlotContextFlag | kAddEp0ContextFlag);
        FillSlotContext(static_cast<uint8_t*>(controller.inputContextVirtual), controller.contextBytes, freeSlot->speed,
                        port, 1u);
        const uint32_t maxPacket = freeSlot->speed == kSpeedSuper ? kMaxPacketSuper : kMaxPacketOther;
        FillEp0Context(static_cast<uint8_t*>(controller.inputContextVirtual), controller.contextBytes,
                       freeSlot->ep0Physical, maxPacket);
        error = RunCommand(controller,
                           MakeAddressDeviceTrb(controller.inputContextPhysical, slotId, controller.commandCycle), nullptr);
        if (error != KernelError::kOk) {
            LogField(controller, "xhci: address device failed error=", static_cast<uint64_t>(error));
            LogNewline(controller);
            return error;
        }
        return KernelError::kOk;
    }

    KernelError DisableDeviceOp(Controller& controller, uint32_t deviceAddress) {
        SlotRecord* slot = FindSlotByAddress(controller, deviceAddress);
        if (slot == nullptr) return KernelError::kNotFound;
        const KernelError error = RunCommand(controller, MakeDisableSlotTrb(slot->slotId, controller.commandCycle), nullptr);
        if (slot->ep0Virtual != nullptr && controller.platform.freePages != nullptr) {
            controller.platform.freePages(controller.platform.context, slot->ep0Physical, 1);
        }
        if (slot->deviceContextVirtual != nullptr && controller.platform.freePages != nullptr) {
            controller.platform.freePages(controller.platform.context, slot->deviceContextPhysical, 1);
        }
        for (uint32_t index = 0; index < kMaxEndpoints; ++index) {
            EndpointRing& ring = slot->endpoints[index];
            if (ring.used != 0 && ring.virtualAddress != nullptr && controller.platform.freePages != nullptr) {
                controller.platform.freePages(controller.platform.context, ring.physical, 1);
            }
        }
        *slot = SlotRecord{};
        return error;
    }

    KernelError SubmitControl(Controller& controller, uint32_t deviceAddress, const UsbSetupPacket* packet, void* buffer,
                              uint32_t bytes, uint32_t* actual) {
        if (packet == nullptr || bytes > controller.pageBytes) return KernelError::kInvalidArgument;
        SlotRecord* slot = FindSlotByAddress(controller, deviceAddress);
        if (slot == nullptr || slot->ep0Virtual == nullptr) return KernelError::kNotFound;
        const bool deviceToHost = (packet->requestType & 0x80u) != 0;
        if (!deviceToHost && bytes != 0 && buffer != nullptr) {
            uint8_t* target = static_cast<uint8_t*>(controller.dataVirtual);
            const uint8_t* source = static_cast<const uint8_t*>(buffer);
            for (uint32_t index = 0; index < bytes; ++index) target[index] = source[index];
        }
        // Setup Stage：IDT=1，8 字节请求嵌在 dw0/dw1
        const uint32_t word0 = static_cast<uint32_t>(packet->requestType) | (static_cast<uint32_t>(packet->request) << 8) |
                               (static_cast<uint32_t>(packet->value) << 16);
        const uint32_t word1 = static_cast<uint32_t>(packet->index) | (static_cast<uint32_t>(packet->length) << 16);
        const uint32_t transferType = bytes == 0 ? 0u : (deviceToHost ? 3u : 2u);
        uint32_t lastPhysical = 0;
        Enqueue(slot->ep0Virtual, slot->ep0Physical, slot->ep0Index, slot->ep0Cycle,
                MakeSetupTrb(word0, word1, transferType, slot->ep0Cycle), nullptr);
        if (bytes != 0) {
            Enqueue(slot->ep0Virtual, slot->ep0Physical, slot->ep0Index, slot->ep0Cycle,
                    MakeDataTrb(controller.dataPhysical, bytes, deviceToHost, slot->ep0Cycle), nullptr);
        }
        Enqueue(slot->ep0Virtual, slot->ep0Physical, slot->ep0Index, slot->ep0Cycle,
                MakeStatusTrb(!deviceToHost, slot->ep0Cycle), &lastPhysical);
        RingDoorbell(controller, slot->slotId, 1u);   // EP0 的 DCI 是 1
        uint32_t transferred = 0;
        const KernelError error = WaitTransfer(controller, lastPhysical, bytes, &transferred);
        if (error != KernelError::kOk) return error;
        if (deviceToHost && bytes != 0 && buffer != nullptr) {
            uint8_t* target = static_cast<uint8_t*>(buffer);
            const uint8_t* source = static_cast<const uint8_t*>(controller.dataVirtual);
            const uint32_t copy = transferred < bytes ? transferred : bytes;
            for (uint32_t index = 0; index < copy; ++index) target[index] = source[index];
        }
        if (actual != nullptr) *actual = transferred;
        return KernelError::kOk;
    }

    KernelError SubmitBulk(Controller& controller, uint32_t deviceAddress, uint32_t endpoint, void* buffer, uint32_t bytes,
                           uint32_t* actual) {
        if (buffer == nullptr || bytes == 0) return KernelError::kInvalidArgument;
        SlotRecord* slot = FindSlotByAddress(controller, deviceAddress);
        if (slot == nullptr) return KernelError::kNotFound;
        EndpointRing* ring = FindEndpointRing(*slot, endpoint);
        if (ring == nullptr) {
            const KernelError configured = ConfigureEndpoint(controller, *slot, endpoint);
            if (configured != KernelError::kOk) return configured;
            ring = FindEndpointRing(*slot, endpoint);
            if (ring == nullptr) return KernelError::kState;
        }
        const bool deviceToHost = (endpoint & 0x80u) != 0;
        const uint32_t dci = DciOf(endpoint);
        uint32_t transferred = 0;
        uint32_t remaining = bytes;
        uint8_t* target = static_cast<uint8_t*>(buffer);
        while (remaining > 0) {
            uint32_t chunk = remaining;
            if (chunk > controller.pageBytes) chunk = controller.pageBytes;
            if (!deviceToHost) {
                const uint8_t* source = target;
                uint8_t* staging = static_cast<uint8_t*>(controller.dataVirtual);
                for (uint32_t index = 0; index < chunk; ++index) staging[index] = source[index];
            }
            uint32_t lastPhysical = 0;
            Enqueue(ring->virtualAddress, ring->physical, ring->index, ring->cycle,
                    MakeNormalTrb(controller.dataPhysical, chunk, deviceToHost, ring->cycle), &lastPhysical);
            RingDoorbell(controller, slot->slotId, dci);
            uint32_t chunkActual = 0;
            const KernelError error = WaitTransfer(controller, lastPhysical, chunk, &chunkActual);
            if (error != KernelError::kOk) return error;
            if (deviceToHost) {
                const uint8_t* staging = static_cast<const uint8_t*>(controller.dataVirtual);
                for (uint32_t index = 0; index < chunkActual; ++index) target[index] = staging[index];
            }
            target += chunkActual;
            transferred += chunkActual;
            if (chunkActual < chunk) break;
            remaining -= chunk;
        }
        if (actual != nullptr) *actual = transferred;
        return KernelError::kOk;
    }

    KernelError Poll(Controller& controller, uint32_t budget) {
        if (budget == 0) budget = 16;
        // 事件都靠轮询消费；EINT 是写 1 清位，顺手清掉避免残留（本驱动不启用中断）
        volatile uint32_t* operational = Operational(controller);
        if ((ReadReg(operational, kUsbSts) & (1u << 3)) != 0) WriteReg(operational, kUsbSts, 1u << 3);
        for (uint32_t processed = 0; processed < budget; ++processed) {
            Trb event;
            if (!DequeueEvent(controller, event)) break;
            const uint32_t type = (event.dw3 >> 10) & 0x3Fu;
            if (type == kTrbPortStatusChange) {
                const uint32_t port = (event.dw0 >> 24) & 0xFFu;
                if (port != 0 && port <= controller.maxPorts) ClearPortChanges(controller, port);
            }
        }
        return KernelError::kOk;
    }

    namespace {
        // —— UsbControllerOps 的适配回调：context 是 Controller* ——
        KernelError OpsSubmitControl(void* context, uint32_t deviceAddress, const UsbSetupPacket* packet, void* buffer,
                                     uint32_t bytes, uint32_t* actual) {
            return SubmitControl(*static_cast<Controller*>(context), deviceAddress, packet, buffer, bytes, actual);
        }

        KernelError OpsSubmitBulk(void* context, uint32_t deviceAddress, uint32_t endpoint, void* buffer, uint32_t bytes,
                                  uint32_t* actual) {
            return SubmitBulk(*static_cast<Controller*>(context), deviceAddress, endpoint, buffer, bytes, actual);
        }

        KernelError OpsPoll(void* context, uint32_t budget) { return Poll(*static_cast<Controller*>(context), budget); }

        uint32_t OpsPortCount(void* context) { return PortCount(*static_cast<Controller*>(context)); }

        KernelError OpsPortStatus(void* context, uint32_t port, uint32_t* status) {
            return PortStatus(*static_cast<Controller*>(context), port, status);
        }

        KernelError OpsPortReset(void* context, uint32_t port) { return PortReset(*static_cast<Controller*>(context), port); }

        KernelError OpsEnableDevice(void* context, uint32_t port, uint32_t deviceAddress) {
            return EnableDeviceOp(*static_cast<Controller*>(context), port, deviceAddress);
        }

        KernelError OpsDisableDevice(void* context, uint32_t deviceAddress) {
            return DisableDeviceOp(*static_cast<Controller*>(context), deviceAddress);
        }
    }

    const UsbControllerOps* Ops() {
        static const UsbControllerOps kOps = {&OpsSubmitControl, &OpsSubmitBulk, &OpsPoll, &OpsPortCount, &OpsPortStatus,
                                              &OpsPortReset, &OpsEnableDevice, &OpsDisableDevice};
        return &kOps;
    }
}
