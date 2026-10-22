/* Module.cpp
    Ahci 模块入口：找 AHCI 控制器、映射 ABAR、申请 DMA 页、初始化端口并发布块设备

    启动闭包里的驱动模块：Init 只做可撤销准备（读设备事实、登记驱动描述与绑定），
    不启用中断、不建线程；读路径同步轮询，与运行期共用同一实例
    驱动不直接发布块设备：BlockDeviceOps 随绑定事实交给 DriverLoader/DevModel，
    Vfs 模块在自己的 Init 里按绑定事实调用 Block.Registry.Publish
    控制器线索来自 Dev.Model（PCI 枚举与 BAR），ABAR 映射用 Core.Mmio.Map，取不到时
    退回 Core.PhysMap 的直映射（恒等映射存在，物理地址可直接访问）；两条路径都失败
    就打 WARN 并跳过发布，不猜地址
    命令列表、FIS 接收区与命令表放在一页 DMA 缓冲里（4KiB 页内布局），数据弹跳页单独一页，
    都经 Core.Page 申请、Core.PhysMap 取虚拟地址；Abort 时按相反顺序归还
*/

#include <ModuleAbi.hpp>

#include <Abi.hpp>

#include "Ahci.hpp"

// 导入回填区：符号名由生成头钉住，装载器在 Init 前把提供者槽表指针写进这些槽
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMmio, 1, CoreMmioSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePage, 1, CorePageSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePhysMap, 1, CorePhysMapSlotsV1) = {};
LP_CONTRACT_IMPORT(DevModel, 1, DevModelSlotsV1) = {};
LP_CONTRACT_IMPORT(DrvLoader, 1, DrvLoaderSlotsV1) = {};

namespace {
    // 控制器实例：DMA 缓冲的物理/虚拟地址与端口事实都在这里，实例与模块同寿命
    Ahci::Controller g_controller;
    // DMA 缓冲：第 0 页放命令列表(0x000)/FIS(0x400)/命令表(0x800)，第 1 页是数据弹跳页
    uint64_t g_dmaPhysical[2] = {};
    void* g_dmaVirtual[2] = {};
    // 驱动实例状态与登记句柄
    uint32_t g_ready = 0;
    uint32_t g_deviceHandle = 0;
    uint32_t g_bindHandle = 0;
    uint32_t g_dmaAllocated = 0;
    uint32_t g_logReady = 0;
    // 一页 DMA 缓冲的内部布局
    constexpr uint32_t kCommandListOffset = 0x000;
    constexpr uint32_t kFisOffset = 0x400;
    constexpr uint32_t kCommandTableOffset = 0x800;
    // ABAR 长度兜底：BAR 未解码出长度时按 AHCI 规范的最小窗口
    constexpr uint32_t kAbarMinBytes = 0x2000;
    // 枚举设备的上限：防止提供者实现异常时死循环
    constexpr uint32_t kEnumerateLimit = 256;
    // 启动候选句柄的收集上限
    constexpr uint32_t kMaxCandidates = 32;

    // 写一段原始文本（无换行）
    void LogText(const char* text) {
        if (g_logReady != 0) _Contract_Import_CoreLog_v1.Write(text);
    }

    // 写 16 位十六进制
    void LogHex(uint64_t value) {
        if (g_logReady != 0) _Contract_Import_CoreLog_v1.Hex(value);
    }

    // 写一整行带标签的诊断
    void LogLine(const char* tag, const char* text) {
        if (g_logReady != 0) _Contract_Import_CoreLog_v1.Line(tag, text);
    }

    // 打印控制器与设备的标识行：端口、型号、扇区数、逻辑扇区大小，供集成阶段断言
    void LogDevice() {
        LogText("ahci: port=");
        LogHex(g_controller.port);
        LogText(" ");
        LogText(g_controller.model);
        LogText(" sectors=");
        LogHex(g_controller.sectors);
        LogText(" block=");
        LogHex(g_controller.sectorBytes);
        LogText("\r\n");
    }

    // —— BlockDeviceOps 回调：单一控制器实例，context 不使用 ——
    uint32_t OpsBlockBytes(void* context) {
        (void)context;
        return g_controller.sectorBytes;
    }

    uint64_t OpsCapacityBytes(void* context) {
        (void)context;
        return g_controller.sectors * g_controller.sectorBytes;
    }

    uint32_t OpsWritable(void* context) {
        (void)context;
        return 1u;   // 物理盘可写；本阶段只实现读，Write 返回只读
    }

    Storage::Error OpsRead(void* context, uint64_t offset, void* buffer, uint32_t bytes) {
        (void)context;
        return Ahci::Read(g_controller, offset, buffer, bytes);
    }

    Storage::Error OpsWrite(void* context, uint64_t offset, const void* buffer, uint32_t bytes) {
        (void)context;
        (void)offset;
        (void)buffer;
        (void)bytes;
        return Storage::Error::ReadOnly;
    }

    Storage::Error OpsFlush(void* context) {
        (void)context;
        return Storage::Error::Ok;
    }

    // 块设备调用表：能力位为 0（没有真实刷新原语），回调全部是模块内函数地址
    const BlockDeviceOps kOps = {0u, &OpsBlockBytes, &OpsCapacityBytes, &OpsWritable, &OpsRead, &OpsWrite, &OpsFlush};

    // 驱动启用回调：只置就绪位，同步读服务在 Init 探测后已经可用
    Storage::Error StartDriver(void* context) {
        (void)context;
        g_ready = 1;
        LogLine("AHCI", "ahci: driver started");
        return Storage::Error::Ok;
    }

    // 驱动停用回调：本阶段没有在途请求与中断
    void StopDriver(void* context) {
        (void)context;
        g_ready = 0;
    }

    // 类/子类的编码兼容：DeviceFacts.classCode 可能是完整 PCI class 寄存器，也可能是三段编码
    uint32_t ClassOf(uint32_t classCode) {
        return classCode > 0xFFFFFFu ? (classCode >> 24) : ((classCode >> 16) & 0xFFu);
    }

    uint32_t SubclassOf(uint32_t classCode) {
        return classCode > 0xFFFFFFu ? ((classCode >> 16) & 0xFFu) : ((classCode >> 8) & 0xFFu);
    }

    // 是否 AHCI 控制器：大容量存储类（0x01）的 SATA 子类（0x06）
    bool IsAhciController(const DeviceFacts& facts) {
        return ClassOf(facts.classCode) == 0x01u && SubclassOf(facts.classCode) == 0x06u;
    }

    // 句柄是否在启动候选表里
    bool IsCandidate(uint32_t handle, const uint32_t* candidates, uint32_t count) {
        for (uint32_t index = 0; index < count; ++index) {
            if (candidates[index] == handle) return true;
        }
        return false;
    }

    // 收集启动候选设备句柄：BootCandidates 不可用或报错时返回 0，由调用方退回全量枚举
    uint32_t CollectCandidates(uint32_t* candidates) {
        if (_Contract_Import_DevModel_v1.BootCandidates == nullptr) return 0;
        uint32_t count = 0;
        uint32_t cursor = 0;
        for (uint32_t step = 0; step < kEnumerateLimit; ++step) {
            uint32_t handle = 0;
            if (_Contract_Import_DevModel_v1.BootCandidates(&cursor, &handle) != KernelError::kOk) break;
            if (handle == 0 || count >= kMaxCandidates) continue;
            candidates[count++] = handle;
        }
        return count;
    }

    // 在 Dev.Model 里找 AHCI 控制器：启动候选里匹配的优先，其次全量枚举里第一个匹配
    bool FindController(DeviceFacts* factsOut) {
        if (_Contract_Import_DevModel_v1.Enumerate == nullptr) return false;
        uint32_t candidates[kMaxCandidates] = {};
        const uint32_t candidateCount = CollectCandidates(candidates);
        bool hasFallback = false;
        DeviceFacts fallback = {};
        uint32_t cursor = 0;
        for (uint32_t step = 0; step < kEnumerateLimit; ++step) {
            DeviceFacts facts = {};
            facts.bytes = static_cast<uint32_t>(sizeof(DeviceFacts));
            const KernelError error = _Contract_Import_DevModel_v1.Enumerate(&cursor, &facts);
            if (error != KernelError::kOk) break;
            if (!IsAhciController(facts)) continue;
            if (facts.deviceHandle != 0 && IsCandidate(facts.deviceHandle, candidates, candidateCount)) {
                *factsOut = facts;
                return true;
            }
            if (!hasFallback) {
                fallback = facts;
                hasFallback = true;
            }
        }
        if (!hasFallback) return false;
        *factsOut = fallback;
        return factsOut->deviceHandle != 0;
    }

    // 从设备资源里取 BAR5（AHCI 的 ABAR）：优先索引 5 的内存 BAR，没有就取第一个内存 BAR
    bool FindAbar(const DeviceFacts& facts, uint64_t* base, uint64_t* bytes) {
        if (_Contract_Import_DevModel_v1.Resources == nullptr) return false;
        DeviceResources resources = {};
        resources.bytes = static_cast<uint32_t>(sizeof(DeviceResources));
        if (_Contract_Import_DevModel_v1.Resources(facts.deviceHandle, &resources) != KernelError::kOk) return false;
        uint32_t count = resources.barCount;
        if (count > kDeviceMaxBars) count = kDeviceMaxBars;
        for (uint32_t index = 0; index < count; ++index) {
            const DeviceBar& bar = resources.bars[index];
            if (bar.index != 5 || bar.base == 0) continue;
            *base = bar.base;
            *bytes = bar.bytes != 0 ? bar.bytes : kAbarMinBytes;
            return true;
        }
        for (uint32_t index = 0; index < count; ++index) {
            const DeviceBar& bar = resources.bars[index];
            if (bar.base == 0 || (bar.flags & 1u) != 0) continue;
            *base = bar.base;
            *bytes = bar.bytes != 0 ? bar.bytes : kAbarMinBytes;
            return true;
        }
        return false;
    }

    // 申请两页 DMA 缓冲并登记物理/虚拟地址：第 0 页命令列表与命令表，第 1 页数据弹跳
    bool AllocateBuffers() {
        if (_Contract_Import_CorePage_v1.Allocate == nullptr || _Contract_Import_CorePhysMap_v1.ToVirt == nullptr) return false;
        const uint32_t pageBytes = _Contract_Import_CorePage_v1.PageBytes != nullptr ? _Contract_Import_CorePage_v1.PageBytes() : 0;
        if (pageBytes == 0 || pageBytes < 0x1000) return false;
        for (uint32_t index = 0; index < 2; ++index) {
            g_dmaPhysical[index] = _Contract_Import_CorePage_v1.Allocate(1);
            if (g_dmaPhysical[index] == 0) {
                for (uint32_t rollback = 0; rollback < index; ++rollback) {
                    if (_Contract_Import_CorePage_v1.Free != nullptr) (void)_Contract_Import_CorePage_v1.Free(g_dmaPhysical[rollback], 1);
                    g_dmaPhysical[rollback] = 0;
                    g_dmaVirtual[rollback] = nullptr;
                }
                return false;
            }
            g_dmaVirtual[index] = _Contract_Import_CorePhysMap_v1.ToVirt(g_dmaPhysical[index]);
            if (g_dmaVirtual[index] == nullptr) {
                for (uint32_t rollback = 0; rollback <= index; ++rollback) {
                    if (_Contract_Import_CorePage_v1.Free != nullptr) (void)_Contract_Import_CorePage_v1.Free(g_dmaPhysical[rollback], 1);
                    g_dmaPhysical[rollback] = 0;
                    g_dmaVirtual[rollback] = nullptr;
                }
                return false;
            }
        }
        g_dmaAllocated = 1;
        g_controller.pageBytes = pageBytes;
        return true;
    }

    // 归还两页 DMA 缓冲
    void ReleaseBuffers() {
        if (g_dmaAllocated == 0) return;
        for (uint32_t index = 0; index < 2; ++index) {
            if (g_dmaPhysical[index] != 0 && _Contract_Import_CorePage_v1.Free != nullptr) {
                (void)_Contract_Import_CorePage_v1.Free(g_dmaPhysical[index], 1);
            }
            g_dmaPhysical[index] = 0;
            g_dmaVirtual[index] = nullptr;
        }
        g_dmaAllocated = 0;
    }

    // 填控制器实例的缓冲指针：命令列表在页首、FIS 在 0x400、命令表在 0x800
    void FillBuffers() {
        uint8_t* page = static_cast<uint8_t*>(g_dmaVirtual[0]);
        g_controller.commandList = page + kCommandListOffset;
        g_controller.fis = page + kFisOffset;
        g_controller.commandTable = page + kCommandTableOffset;
        g_controller.data = static_cast<uint8_t*>(g_dmaVirtual[1]);
        g_controller.commandListPhysical = g_dmaPhysical[0] + kCommandListOffset;
        g_controller.fisPhysical = g_dmaPhysical[0] + kFisOffset;
        g_controller.commandTablePhysical = g_dmaPhysical[0] + kCommandTableOffset;
        g_controller.dataPhysical = g_dmaPhysical[1];
    }

    // 登记 DriverDesc 并绑定设备（块设备由 Vfs 按绑定事实登记）；各步失败只打诊断，Init 仍返回成功
    void RegisterAndBind(const DeviceFacts& facts, bool factsValid) {
        if (_Contract_Import_DrvLoader_v1.RegisterDriver == nullptr) {
            LogLine("WARN", "ahci: Drv.Loader unavailable");
            return;
        }
        DriverDesc desc = {};
        desc.bytes = static_cast<uint32_t>(sizeof(DriverDesc));
        desc.driveSdkVersion = kDriveSdkVersion;
        desc.opsKind = 1;
        desc.busKind = factsValid ? facts.busKind : 0u;
        desc.classCode = factsValid ? facts.classCode : 0xFFFFFFFFu;
        desc.vendorId = factsValid ? facts.vendorId : 0xFFFFu;
        desc.deviceId = factsValid ? facts.deviceId : 0xFFFFFFFFu;
        desc.flags = 0;
        desc.start = &StartDriver;
        desc.stop = &StopDriver;
        desc.ops = &kOps;
        desc.context = nullptr;
        const KernelError registered = _Contract_Import_DrvLoader_v1.RegisterDriver(&desc);
        if (registered != KernelError::kOk) {
            LogText("ahci: RegisterDriver failed error=");
            LogHex(static_cast<uint64_t>(registered));
            LogText("\r\n");
            return;
        }
        LogLine("AHCI", "ahci: driver descriptor registered");
        if (g_deviceHandle == 0) {
            LogLine("WARN", "ahci: no devmodel controller, block device not published");
            return;
        }
        if (_Contract_Import_DrvLoader_v1.Bind != nullptr) {
            const KernelError bound = _Contract_Import_DrvLoader_v1.Bind(g_deviceHandle, &desc, &g_bindHandle);
            if (bound != KernelError::kOk) {
                LogText("ahci: Bind failed error=");
                LogHex(static_cast<uint64_t>(bound));
                LogText("\r\n");
                return;
            }
            LogLine("AHCI", "ahci: device bound");
        }
        LogLine("OK", "ahci: driver bound, block device handed to devmodel");
    }
}

// Init 入口：找控制器、映射 ABAR、申请 DMA 页、初始化端口并发布块设备
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    g_logReady = _Contract_Import_CoreLog_v1.Line != nullptr ? 1u : 0u;
    LogLine("AHCI", "ahci: probing controller");
    DeviceFacts facts = {};
    const bool factsValid = FindController(&facts);
    if (!factsValid) {
        LogLine("AHCI", "ahci: no device");
        return KernelError::kOk;
    }
    g_deviceHandle = facts.deviceHandle;
    // 设备认领归 Drv.Loader.Bind：驱动自己不再 Claim，
    // 否则 Bind 内部再 Claim 一次会拿到 kBusy，绑定建不起来
    // ABAR：优先 Core.Mmio.Map，取不到退回直映射；两者都失败就跳过发布，不猜地址
    uint64_t abarBase = 0;
    uint64_t abarBytes = 0;
    if (!FindAbar(facts, &abarBase, &abarBytes)) {
        LogLine("WARN", "ahci: ABAR not found in devmodel resources");
        return KernelError::kOk;
    }
    void* abar = nullptr;
    if (_Contract_Import_CoreMmio_v1.Map != nullptr) abar = _Contract_Import_CoreMmio_v1.Map(abarBase, abarBytes, 0);
    if (abar == nullptr && _Contract_Import_CorePhysMap_v1.ToVirt != nullptr) abar = _Contract_Import_CorePhysMap_v1.ToVirt(abarBase);
    if (abar == nullptr) {
        LogLine("WARN", "ahci: ABAR not mappable");
        return KernelError::kOk;
    }
    g_controller.abar = static_cast<volatile uint32_t*>(abar);
    if (!AllocateBuffers()) {
        LogLine("WARN", "ahci: DMA buffers not available");
        return KernelError::kOk;
    }
    FillBuffers();
    if (!Ahci::Initialize(g_controller)) {
        LogLine("WARN", "ahci: port initialize failed");
        ReleaseBuffers();
        g_controller.abar = nullptr;
        return KernelError::kOk;
    }
    LogDevice();
    RegisterAndBind(facts, true);
    return KernelError::kOk;
}

// Abort 入口：反向撤销绑定（认领与释放归 Drv.Loader.Bind/Stop）、停端口、归还 DMA 页
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    if (g_bindHandle != 0 && _Contract_Import_DrvLoader_v1.Stop != nullptr) {
        (void)_Contract_Import_DrvLoader_v1.Stop(g_bindHandle);
        g_bindHandle = 0;
    }
    Ahci::StopPort(g_controller);
    ReleaseBuffers();
    g_controller.abar = nullptr;
    g_ready = 0;
    return KernelError::kOk;
}
