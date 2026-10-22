/* Module.cpp
    Xhci 模块入口：Dev.Model 找 xHCI 控制器 → Claim → ABAR 映射 → 初始化控制器 →
    把 UsbControllerOps 登记进 Usb.Host（UsbCore 的 AddController）

    平台能力全部经导入槽实现：DMA 页来自 Core.Page，虚拟地址来自 Core.PhysMap，
    MMIO 来自 Core.Mmio（取不到退回直映射），日志来自 Core.Log
    无控制器时打 "xhci: no device" 并返回成功（闭包模块缺硬件不失败整集装载）；
    初始化失败返回 kTimeout / kIo，由装载器按 E-KRN 处理
*/

#include <ModuleAbi.hpp>

#include "Xhci.hpp"

// 导入回填区：装载器在 Init 前把提供者槽表指针写进这些槽
//   Core.Log 3 槽（Write/Line/Hex）、Core.Mem 3 槽（Copy/CopyOverlap/Zero）、
//   Core.Page 3 槽（PageBytes/Allocate/Free）、Core.PhysMap 2 槽（ToVirt/ToPhys）、
//   Core.Mmio 2 槽（Map/Unmap）、Dev.Model 6 槽（v1 全槽）、Usb.Host 2 槽（AddController 等）
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMem, 1, CoreMemSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePage, 1, CorePageSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePhysMap, 1, CorePhysMapSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMmio, 1, CoreMmioSlotsV1) = {};
LP_CONTRACT_IMPORT(DevModel, 1, DevModelSlotsV1) = {};
LP_CONTRACT_IMPORT(UsbHost, 1, UsbHostSlotsV1) = {};

namespace {
    // PCI 类码：xHCI 串行总线控制器的 class 0x0C / subclass 0x03 / progIf 0x30
    constexpr uint32_t kXhciClassCode = 0x0C0330u;
    // 枚举上限与 BAR 长度兜底
    constexpr uint32_t kEnumerateLimit = 256;
    constexpr uint64_t kAbarMinBytes = 0x10000u;
    // 控制器实例与设备句柄：实例与模块同寿命
    Xhci::Controller g_controller;
    uint32_t g_deviceHandle = 0;
    uint32_t g_claimHandle = 0;
    uint32_t g_controllerHandle = 0;
    uint32_t g_logReady = 0;

    // —— 平台适配：DMA 页与日志都走契约导入槽 ——
    void* PlatformAllocatePages(void* context, uint32_t pages, uint64_t* physicalOut) {
        (void)context;
        if (_Contract_Import_CorePage_v1.Allocate == nullptr || _Contract_Import_CorePhysMap_v1.ToVirt == nullptr) {
            return nullptr;
        }
        const uint64_t physical = _Contract_Import_CorePage_v1.Allocate(pages);
        if (physical == 0) return nullptr;
        void* virtualAddress = _Contract_Import_CorePhysMap_v1.ToVirt(physical);
        if (virtualAddress == nullptr) {
            if (_Contract_Import_CorePage_v1.Free != nullptr) (void)_Contract_Import_CorePage_v1.Free(physical, pages);
            return nullptr;
        }
        if (physicalOut != nullptr) *physicalOut = physical;
        return virtualAddress;
    }

    void PlatformFreePages(void* context, uint64_t physical, uint32_t pages) {
        (void)context;
        if (_Contract_Import_CorePage_v1.Free != nullptr) (void)_Contract_Import_CorePage_v1.Free(physical, pages);
    }

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

    // 平台注入表
    const Xhci::Platform kPlatform = {&PlatformAllocatePages, &PlatformFreePages, &PlatformLine, &PlatformText,
                                      &PlatformHex, nullptr};

    // 写原始文本
    void LogText(const char* text) {
        if (g_logReady != 0 && _Contract_Import_CoreLog_v1.Write != nullptr) _Contract_Import_CoreLog_v1.Write(text);
    }

    // 写十六进制
    void LogHex(uint64_t value) {
        if (g_logReady != 0 && _Contract_Import_CoreLog_v1.Hex != nullptr) _Contract_Import_CoreLog_v1.Hex(value);
    }

    // 写带标签整行
    void LogLine(const char* tag, const char* text) {
        if (g_logReady != 0 && _Contract_Import_CoreLog_v1.Line != nullptr) _Contract_Import_CoreLog_v1.Line(tag, text);
    }

    // 在 Dev.Model 里找 xHCI 控制器：按类码匹配
    bool FindController(DeviceFacts* factsOut) {
        if (_Contract_Import_DevModel_v1.Enumerate == nullptr) return false;
        uint32_t cursor = 0;
        for (uint32_t step = 0; step < kEnumerateLimit; ++step) {
            DeviceFacts facts = {};
            facts.bytes = static_cast<uint32_t>(sizeof(DeviceFacts));
            if (_Contract_Import_DevModel_v1.Enumerate(&cursor, &facts) != KernelError::kOk) return false;
            if (facts.classCode != kXhciClassCode) continue;
            *factsOut = facts;
            return facts.deviceHandle != 0;
        }
        return false;
    }

    // 从设备资源里取 BAR0（xHCI 的 ABAR）：内存空间，64 位 BAR 已由 DevModel 合并
    bool FindBar0(const DeviceFacts& facts, uint64_t* base, uint64_t* bytes) {
        if (_Contract_Import_DevModel_v1.Resources == nullptr) return false;
        DeviceResources resources = {};
        resources.bytes = static_cast<uint32_t>(sizeof(DeviceResources));
        if (_Contract_Import_DevModel_v1.Resources(facts.deviceHandle, &resources) != KernelError::kOk) return false;
        uint32_t count = resources.barCount;
        if (count > kDeviceMaxBars) count = kDeviceMaxBars;
        for (uint32_t index = 0; index < count; ++index) {
            const DeviceBar& bar = resources.bars[index];
            if (bar.index != 0) continue;
            if (bar.base == 0) continue;
            if ((bar.flags & 1u) != 0) continue;   // I/O 空间不是 ABAR
            *base = bar.base;
            *bytes = bar.bytes != 0 ? bar.bytes : kAbarMinBytes;
            if (*bytes < kAbarMinBytes) *bytes = kAbarMinBytes;
            return true;
        }
        return false;
    }
}

// Init 入口：找控制器 → 认领 → 映射 ABAR → 初始化 → 把 ops 交给 UsbCore
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    g_logReady = _Contract_Import_CoreLog_v1.Line != nullptr ? 1u : 0u;
    LogLine("XHCI", "xhci: probing controller");
    DeviceFacts facts = {};
    if (!FindController(&facts)) {
        LogLine("XHCI", "xhci: no device");
        return KernelError::kOk;
    }
    g_deviceHandle = facts.deviceHandle;
    if (_Contract_Import_DevModel_v1.Claim == nullptr) return KernelError::kState;
    if (_Contract_Import_DevModel_v1.Claim(g_deviceHandle, 0, &g_claimHandle) != KernelError::kOk) {
        LogLine("WARN", "xhci: controller claim failed");
        return KernelError::kIo;
    }
    uint64_t abarBase = 0;
    uint64_t abarBytes = 0;
    if (!FindBar0(facts, &abarBase, &abarBytes)) {
        LogLine("WARN", "xhci: ABAR not found in resources");
        return KernelError::kIo;
    }
    void* abar = nullptr;
    if (_Contract_Import_CoreMmio_v1.Map != nullptr) abar = _Contract_Import_CoreMmio_v1.Map(abarBase, abarBytes, 0);
    if (abar == nullptr && _Contract_Import_CorePhysMap_v1.ToVirt != nullptr) abar = _Contract_Import_CorePhysMap_v1.ToVirt(abarBase);
    if (abar == nullptr) {
        LogLine("WARN", "xhci: ABAR not mappable");
        return KernelError::kIo;
    }
    g_controller.abar = static_cast<volatile uint32_t*>(abar);
    const uint32_t pageBytes = _Contract_Import_CorePage_v1.PageBytes != nullptr ? _Contract_Import_CorePage_v1.PageBytes() : 4096u;
    (void)Xhci::AttachPlatform(g_controller, kPlatform, pageBytes);
    const KernelError initialized = Xhci::Initialize(g_controller);
    if (initialized != KernelError::kOk) {
        LogText("xhci: controller initialize failed error=");
        LogHex(static_cast<uint64_t>(initialized));
        LogText("\r\n");
        Xhci::Shutdown(g_controller);
        return initialized;
    }
    if (_Contract_Import_UsbHost_v1.AddController == nullptr) {
        LogLine("WARN", "xhci: Usb.Host unavailable");
        Xhci::Shutdown(g_controller);
        return KernelError::kState;
    }
    const KernelError added = _Contract_Import_UsbHost_v1.AddController(&g_controllerHandle, Xhci::Ops(), &g_controller);
    if (added != KernelError::kOk) {
        LogText("xhci: AddController failed error=");
        LogHex(static_cast<uint64_t>(added));
        LogText("\r\n");
        Xhci::Shutdown(g_controller);
        return KernelError::kIo;
    }
    LogText("xhci: controller added handle=");
    LogHex(g_controllerHandle);
    LogText(" ports=");
    LogHex(g_controller.maxPorts);
    LogText(" slots=");
    LogHex(g_controller.maxSlots);
    LogText("\r\n");
    return KernelError::kOk;
}

// Abort 入口：停控制器、归还 DMA 页、释放设备控制权
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    Xhci::Shutdown(g_controller);
    if (g_claimHandle != 0 && _Contract_Import_DevModel_v1.Release != nullptr && g_deviceHandle != 0) {
        (void)_Contract_Import_DevModel_v1.Release(g_deviceHandle);
        g_claimHandle = 0;
    }
    return KernelError::kOk;
}
