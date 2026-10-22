/* Xhci.hpp
    xHCI 主机控制器驱动：寄存器层、TRB 编码、命令环/事件环、EP0 控制传输、批量端点与端口

    纯编码与解析（CAP 解析、TRB 构造、PORTSC 解析、环下标推进）都是可宿主断言的纯函数，
    CheckXhci 用假 MMIO 数组逐项验证；真正的寄存器读写与 DMA 只在这里的 Init/传输路径里
    DMA 缓冲经 Platform 注入分配（目标侧接 Core.Page + Core.PhysMap，物理地址直写进 TRB）
    xHCI 规范版本按 1.1 的布局：CAP 在 MMIO 起始，操作寄存器在 CAPLENGTH 之后，
    运行寄存器在 RTSOFF，门铃在 DBOFF；命令环、事件环、设备上下文数组、输入上下文与
    端点环都由本驱动分配并持有，Abort 时归还
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

namespace Xhci {
    // 端口/槽位/端点上限（本驱动按定长表实现）
    constexpr uint32_t kMaxPorts = 32;
    constexpr uint32_t kMaxSlots = 8;
    constexpr uint32_t kMaxEndpoints = 2;   // 每设备非 EP0 端点环：EP1 IN / EP1 OUT
    // 标准请求：GET_DESCRIPTOR 与 SET_CONFIGURATION 的编号在 UsbCore 侧，Xhci 只搬运
    // 寄存器偏移：CAP 区
    constexpr uint32_t kCapLength = 0x00;      // byte0 = CAPLENGTH，byte2..3 = HCIVERSION
    constexpr uint32_t kHcsparams1 = 0x04;     // MaxSlots bit0..7、MaxIntrs bit8..18、MaxPorts bit24..31
    constexpr uint32_t kHccparams1 = 0x10;     // AC64 bit0、CSZ bit2
    constexpr uint32_t kDboff = 0x14;          // 门铃数组偏移
    constexpr uint32_t kRtsoff = 0x18;         // 运行寄存器偏移
    // 操作寄存器偏移（相对操作寄存器基址 = MMIO + CAPLENGTH）
    constexpr uint32_t kUsbCmd = 0x00;
    constexpr uint32_t kUsbSts = 0x04;
    constexpr uint32_t kConfig = 0x38;
    constexpr uint32_t kCrcr = 0x18;
    constexpr uint32_t kDcbaap = 0x30;
    constexpr uint32_t kPortBase = 0x400;
    constexpr uint32_t kPortStride = 0x10;
    // USBCMD 位：运行、复位
    constexpr uint32_t kCmdRun = 1u << 0;
    constexpr uint32_t kCmdReset = 1u << 1;
    // USBSTS 位：停机、控制器未就绪
    constexpr uint32_t kStsHalted = 1u << 0;
    constexpr uint32_t kStsNotReady = 1u << 11;
    // 运行寄存器布局：偏移 0x00 是 MFINDEX，中断器寄存器从 +0x20 起、每个中断器 0x20 字节
    constexpr uint32_t kInterrupter0Base = 0x20;
    constexpr uint32_t kIman = 0x00;      // 中断管理
    constexpr uint32_t kImod = 0x04;      // 中断调节
    constexpr uint32_t kErstsz = 0x08;    // 事件环段表大小
    constexpr uint32_t kErstba = 0x10;    // 事件环段表基址
    constexpr uint32_t kErdp = 0x18;      // 事件环出队指针
    // 命令环/事件环/端点环的条目数与 Link TRB 位置：一页 4KiB，每 TRB 16 字节
    constexpr uint32_t kRingTrbs = 256;
    constexpr uint32_t kRingLinkIndex = kRingTrbs - 1;
    // 事件环的完成码：成功
    constexpr uint32_t kCompletionSuccess = 1;
    // TRB 类型
    constexpr uint32_t kTrbNormal = 1;
    constexpr uint32_t kTrbSetup = 2;
    constexpr uint32_t kTrbData = 3;
    constexpr uint32_t kTrbStatus = 4;
    constexpr uint32_t kTrbLink = 6;
    constexpr uint32_t kTrbEnableSlot = 9;
    constexpr uint32_t kTrbDisableSlot = 10;
    constexpr uint32_t kTrbAddressDevice = 11;
    constexpr uint32_t kTrbConfigureEndpoint = 12;
    constexpr uint32_t kTrbTransferEvent = 32;
    constexpr uint32_t kTrbCommandCompletion = 33;
    constexpr uint32_t kTrbPortStatusChange = 34;
    // PORTSC 位
    constexpr uint32_t kPortscCcs = 1u << 0;
    constexpr uint32_t kPortscPed = 1u << 1;
    constexpr uint32_t kPortscPr = 1u << 4;
    constexpr uint32_t kPortscPlsShift = 5;
    constexpr uint32_t kPortscPlsMask = 0x0F;
    constexpr uint32_t kPortscSpeedShift = 10;
    constexpr uint32_t kPortscSpeedMask = 0x0F;
    constexpr uint32_t kPortscCsc = 1u << 16;
    constexpr uint32_t kPortscPrc = 1u << 20;
    constexpr uint32_t kPortscPlc = 1u << 22;
    constexpr uint32_t kPortscCec = 1u << 23;
    // PORTSC 写 1 清的变化位集合
    constexpr uint32_t kPortscChangeBits = kPortscCsc | kPortscPrc | kPortscPlc | kPortscCec | (1u << 17) | (1u << 18) | (1u << 19) | (1u << 21);
    // 轮询上限：命令与传输是毫秒级，次数足够宽松；超时返回 kTimeout
    constexpr uint32_t kPollLimit = 20000000;
    // 端点默认最大包长：超速 512，其余 64（未读端点描述符时的保守值，够 BOT 使用）
    constexpr uint32_t kMaxPacketSuper = 512;
    constexpr uint32_t kMaxPacketOther = 64;
    // 速度编码（UsbDeviceFacts.speed 与 UsbControllerOps.PortStatus 共用）：1 低速 2 全速 3 高速 4 超速
    constexpr uint32_t kSpeedLow = 1;
    constexpr uint32_t kSpeedFull = 2;
    constexpr uint32_t kSpeedHigh = 3;
    constexpr uint32_t kSpeedSuper = 4;

    // 一个 TRB：16 字节，四条 32 位字
    struct Trb {
        uint32_t dw0 = 0;   // 参数低 / 类型与标志
        uint32_t dw1 = 0;   // 参数中 / 状态
        uint32_t dw2 = 0;   // 参数高 / 剩余长度
        uint32_t dw3 = 0;   // 类型、完成码、槽位与端点、cycle
    };

    // CAP 区的解析结果
    struct Capabilities {
        uint32_t caplength = 0;      // 操作寄存器偏移
        uint32_t version = 0;        // HCIVERSION
        uint32_t maxSlots = 0;       // HCSPARAMS1.MaxSlots
        uint32_t maxPorts = 0;       // HCSPARAMS1.MaxPorts
        uint32_t ac64 = 0;           // HCCPARAMS1.AC64
        uint32_t contextBytes = 32;  // 设备上下文大小：CSZ 置位为 64
        uint32_t dboff = 0;          // 门铃数组偏移
        uint32_t rtsoff = 0;         // 运行寄存器偏移
    };

    // 从 ABAR 起始处解析 CAP 与偏移；关键字段不成立返回 false
    bool ReadCapabilities(volatile const uint32_t* abar, Capabilities& caps);

    // TRB 构造：地址参数都是物理地址
    Trb MakeSetupTrb(uint32_t word0, uint32_t word1, uint32_t transferType, uint32_t cycle);
    Trb MakeDataTrb(uint64_t bufferPhysical, uint32_t bytes, bool deviceToHost, uint32_t cycle);
    Trb MakeStatusTrb(bool deviceToHost, uint32_t cycle);
    Trb MakeNormalTrb(uint64_t bufferPhysical, uint32_t bytes, bool deviceToHost, uint32_t cycle);
    Trb MakeLinkTrb(uint64_t ringPhysical, uint32_t cycle);
    Trb MakeEnableSlotTrb(uint32_t cycle);
    Trb MakeDisableSlotTrb(uint32_t slotId, uint32_t cycle);
    Trb MakeAddressDeviceTrb(uint64_t inputContextPhysical, uint32_t slotId, uint32_t cycle);
    Trb MakeConfigureEndpointTrb(uint64_t inputContextPhysical, uint32_t slotId, uint32_t cycle);
    // 命令/传输 TRB 的 slot 与 endpoint 字段（dw3 bit16..23 是端点，bit24..31 是槽位）
    uint32_t TrbSlot(const Trb& trb);
    uint32_t TrbEndpoint(const Trb& trb);
    uint32_t TrbCompletionCode(const Trb& trb);

    // PORTSC → UsbControllerOps.PortStatus 的编码（UsbCore 的位定义）
    uint32_t PortStatusFromPortsc(uint32_t portsc);
    // PORTSC 的速度字段 → UsbDeviceFacts.speed 编码
    uint32_t SpeedFromPortsc(uint32_t portsc);
    // 端口号（从 1 起）对应 PORTSC 的寄存器偏移
    uint32_t PortscOffset(uint32_t port);

    // 输入上下文的上下文偏移：上下文索引 0 是输入控制上下文、1 是槽上下文、2 是 EP0、
    // n（n>=2）是端点上下文 DCI n-1；偏移 = 索引 * 上下文大小（CSZ=0 时 32 字节，CSZ=1 时 64）
    uint32_t ContextOffset(uint32_t contextIndex, uint32_t contextBytes);

    // 输入控制上下文的 Add Context Flags 位：A0 槽上下文、A1 EP0 上下文、
    // A(n)（n>=2）对应端点上下文索引 n（即 DCI n）—— 注意不是 DCI+1
    constexpr uint32_t kAddSlotContextFlag = 1u << 0;
    constexpr uint32_t kAddEp0ContextFlag = 1u << 1;
    uint32_t EndpointAddFlag(uint32_t dci);

    // —— 控制器运行期状态与驱动实现 ——
    // DMA 分配与日志经 Platform 注入：目标侧接 Core.Page/Core.PhysMap/Core.Log，宿主夹具注入假实现
    struct Platform {
        void* (*allocatePages)(void* context, uint32_t pages, uint64_t* physicalOut);  // 连续页：虚拟指针 + 物理地址
        void (*freePages)(void* context, uint64_t physical, uint32_t pages);          // 归还连续页
        void (*logLine)(void* context, const char* tag, const char* text);            // 一整行诊断
        void (*logText)(void* context, const char* text);                             // 原始文本
        void (*logHex)(void* context, uint64_t value);                                // 十六进制
        void* context;                                                                // 透传上下文
    };

    // 每个非 EP0 端点的传输环
    struct EndpointRing {
        uint32_t used = 0;      // 0 未配置，1 已配置
        uint32_t endpoint = 0;  // USB 端点地址（bit7 方向，bit0..3 端点号）
        uint64_t physical = 0;  // 环首物理地址
        void* virtualAddress = nullptr; // 环首虚拟地址
        uint32_t index = 0;     // 下一 TRB 下标
        uint32_t cycle = 1;     // 当前 cycle 位
    };

    // 一个槽位（设备）的上下文与端点环
    struct SlotRecord {
        uint32_t used = 0;      // 0 未启用
        uint32_t slotId = 0;    // xHCI 槽位号（1..MaxSlots）
        uint32_t port = 0;      // 所属根端口
        uint32_t speed = 0;     // 速度编码（UsbDeviceFacts 同款）
        uint32_t address = 0;      // UsbCore 侧设备地址（与槽位号独立）
        uint64_t ep0Physical = 0;  // EP0 传输环物理地址
        void* ep0Virtual = nullptr;
        uint64_t deviceContextPhysical = 0;  // 设备上下文物理地址（DCBAA[slotId]）
        void* deviceContextVirtual = nullptr;
        uint32_t ep0Index = 0;  // EP0 环下一 TRB 下标
        uint32_t ep0Cycle = 1;  // EP0 环 cycle 位
        EndpointRing endpoints[kMaxEndpoints];
    };

    // 控制器状态：MMIO 指针、各环缓冲与槽位记录
    struct Controller {
        volatile uint32_t* abar = nullptr;   // MMIO 基址
        uint32_t caplength = 0;              // 操作寄存器偏移
        uint32_t maxPorts = 0;
        uint32_t maxSlots = 0;
        uint32_t contextBytes = 32;
        uint32_t dboff = 0;
        uint32_t rtsoff = 0;
        uint64_t commandRingPhysical = 0;    // 命令环
        void* commandRingVirtual = nullptr;
        uint32_t commandIndex = 0;
        uint32_t commandCycle = 1;
        uint64_t eventRingPhysical = 0;      // 事件环
        void* eventRingVirtual = nullptr;
        uint32_t eventIndex = 0;
        uint32_t eventCycle = 1;
        uint64_t erstPhysical = 0;           // 事件环段表
        void* erstVirtual = nullptr;
        uint64_t dcbaaPhysical = 0;          // 设备上下文数组
        void* dcbaaVirtual = nullptr;
        uint64_t inputContextPhysical = 0;   // 输入上下文（命令用，串行复用）
        void* inputContextVirtual = nullptr;
        uint64_t dataPhysical = 0;           // 数据弹跳页（控制/批量传输用）
        void* dataVirtual = nullptr;
        uint32_t pageBytes = 0;
        SlotRecord slots[kMaxSlots];
        Platform platform = {};
        uint32_t initialized = 0;
    };

    // 初始化控制器：CAP 校验、HCRST、CONFIG/DCBAAP/CRCR/事件环、RS；失败返回 KernelError
    KernelError Initialize(Controller& controller);
    // 停止控制器并归还全部 DMA 缓冲（Abort 与错误回滚共用）
    void Shutdown(Controller& controller);
    // UsbControllerOps 的 8 个回调（context 是 Controller*）
    const UsbControllerOps* Ops();
    // 平台适配：目标侧由 Module.cpp 用 Core.Page/Core.PhysMap/Core.Log 填好
    KernelError AttachPlatform(Controller& controller, const Platform& platform, uint32_t pageBytes);

    // —— UsbControllerOps 的 8 个操作（context 是 Controller*，ops 表由 Ops() 给出）——
    uint32_t PortCount(Controller& controller);
    KernelError PortStatus(Controller& controller, uint32_t port, uint32_t* status);
    KernelError PortReset(Controller& controller, uint32_t port);
    KernelError EnableDeviceOp(Controller& controller, uint32_t port, uint32_t deviceAddress);
    KernelError DisableDeviceOp(Controller& controller, uint32_t deviceAddress);
    KernelError SubmitControl(Controller& controller, uint32_t deviceAddress, const UsbSetupPacket* packet,
                              void* buffer, uint32_t bytes, uint32_t* actual);
    KernelError SubmitBulk(Controller& controller, uint32_t deviceAddress, uint32_t endpoint, void* buffer,
                           uint32_t bytes, uint32_t* actual);
    KernelError Poll(Controller& controller, uint32_t budget);
}
