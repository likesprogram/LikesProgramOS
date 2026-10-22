/* Module.cpp
    Nvme 模块入口：找 NVMe 控制器、映射 BAR0、申请队列与数据页、初始化控制器并登记驱动

    启动闭包里的驱动模块：Init 只做可撤销准备（读设备事实、映射寄存器、建队列、Identify、
    登记 DriverDesc 与绑定），不启用中断、不建线程；读路径同步轮询，与运行期共用同一实例
    驱动不直接发布块设备：BlockDeviceOps 随绑定事实交给 DriverLoader/DevModel，
    Vfs 模块在自己的 Init 里按绑定事实调用 Block.Registry.Publish
    控制器线索来自 Dev.Model（PCI 枚举与 BAR）：classCode 的 NVMe 类 0x01 / 子类 0x08，
    BAR0 是 64 位 MMIO；ABAR 优先 Core.Mmio.Map，取不到退回 Core.PhysMap 直映射，
    两条都失败就打 WARN 并返回，不猜地址
    队列内存五页：Admin SQ、Admin CQ、I/O SQ、I/O CQ 各占一页（NVMe 要求 ASQ/ACQ 按内存页对齐，
    QEMU 的 nvme_start_ctrl 会以 startfail_acq_misaligned 拒绝未对齐的完成队列），第 4 页是
    数据弹跳页；都经 Core.Page 申请、Core.PhysMap 取虚拟地址，Abort 时按相反顺序归还
    找不到控制器返回 kOk（非 NVMe 启动档不因缺少设备失败）；找到但初始化失败按
    Timeout / NotSupported / Corrupt / Io 返回，由装载器按必需模块失败处理
*/

#include <ModuleAbi.hpp>

#include <Abi.hpp>

#include "Nvme.hpp"
#include "NvmeRegisters.hpp"

// 导入回填区：符号名由生成头钉住，装载器在 Init 前把提供者槽表指针写进这些槽
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMem, 1, CoreMemSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMmio, 1, CoreMmioSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePage, 1, CorePageSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePhysMap, 1, CorePhysMapSlotsV1) = {};
LP_CONTRACT_IMPORT(DevModel, 1, DevModelSlotsV1) = {};
LP_CONTRACT_IMPORT(DrvLoader, 1, DrvLoaderSlotsV1) = {};

namespace {
    // 控制器实例：寄存器访问与队列缓冲指针都在这里，实例与模块同寿命
    Nvme::Controller g_controller;
    // 队列与数据页：每张队列独占一页，保证 ASQ/ACQ/I/O 队列都按 4 KiB 页对齐；最后一页是数据弹跳页
    constexpr uint32_t kPageCount = 5;
    uint64_t g_pagesPhysical[kPageCount] = {};
    void* g_pagesVirtual[kPageCount] = {};
    uint32_t g_pagesAllocated = 0;
    // 驱动实例状态与登记句柄
    uint32_t g_ready = 0;
    uint32_t g_deviceHandle = 0;
    uint32_t g_bindHandle = 0;
    uint32_t g_logReady = 0;
    // BAR0 长度兜底：BAR 未解码出长度时按 NVMe 规范的最小窗口
    constexpr uint64_t kBar0MinBytes = 0x4000;
    // 枚举设备的上限：防止提供者实现异常时死循环
    constexpr uint32_t kEnumerateLimit = 256;
    // 启动候选句柄的收集上限
    constexpr uint32_t kMaxCandidates = 32;
    // 标识行缓冲长度
    constexpr uint32_t kDeviceLineBytes = 96;

    // 读一个 32 位 MMIO 寄存器：context 是映射后的 ABAR 指针
    uint32_t RegisterRead(void* context, uint32_t offset) {
        volatile uint32_t* address = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(context) + offset);
        return *address;
    }

    // 写一个 32 位 MMIO 寄存器：context 是映射后的 ABAR 指针
    void RegisterWrite(void* context, uint32_t offset, uint32_t value) {
        volatile uint32_t* address = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(context) + offset);
        *address = value;
    }

    // 驱动启停回调：在登记之前声明，RegisterAndBind 取它们的地址
    Storage::Error StartDriver(void* context);
    void StopDriver(void* context);

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

    // 类/子类的编码兼容：DeviceFacts.classCode 可能是完整 PCI class 寄存器，也可能是三段编码
    uint32_t ClassOf(uint32_t classCode) {
        return classCode > 0xFFFFFFu ? (classCode >> 24) : ((classCode >> 16) & 0xFFu);
    }

    uint32_t SubclassOf(uint32_t classCode) {
        return classCode > 0xFFFFFFu ? ((classCode >> 16) & 0xFFu) : ((classCode >> 8) & 0xFFu);
    }

    // 是否 NVMe 控制器：大容量存储类（0x01）的 NVM 子类（0x08）
    bool IsNvmeController(const DeviceFacts& facts) {
        return ClassOf(facts.classCode) == 0x01u && SubclassOf(facts.classCode) == 0x08u;
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

    // 在 Dev.Model 里找 NVMe 控制器：启动候选里匹配的优先，其次全量枚举里第一个匹配
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
            if (!IsNvmeController(facts)) continue;
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

    // 从设备资源里取 BAR0（NVMe 的 ABAR）：索引 0 的内存 BAR，没有就取第一个内存 BAR
    bool FindBar0(const DeviceFacts& facts, uint64_t* base, uint64_t* bytes) {
        if (_Contract_Import_DevModel_v1.Resources == nullptr) return false;
        DeviceResources resources = {};
        resources.bytes = static_cast<uint32_t>(sizeof(DeviceResources));
        if (_Contract_Import_DevModel_v1.Resources(facts.deviceHandle, &resources) != KernelError::kOk) return false;
        uint32_t count = resources.barCount;
        if (count > kDeviceMaxBars) count = kDeviceMaxBars;
        for (uint32_t index = 0; index < count; ++index) {
            const DeviceBar& bar = resources.bars[index];
            if (bar.index != 0 || bar.base == 0 || (bar.flags & 1u) != 0) continue;
            *base = bar.base;
            *bytes = bar.bytes != 0 ? bar.bytes : kBar0MinBytes;
            return true;
        }
        for (uint32_t index = 0; index < count; ++index) {
            const DeviceBar& bar = resources.bars[index];
            if (bar.base == 0 || (bar.flags & 1u) != 0) continue;
            *base = bar.base;
            *bytes = bar.bytes != 0 ? bar.bytes : kBar0MinBytes;
            return true;
        }
        return false;
    }

    // 申请四页队列与数据缓冲并登记物理/虚拟地址
    bool AllocatePages() {
        if (_Contract_Import_CorePage_v1.Allocate == nullptr || _Contract_Import_CorePhysMap_v1.ToVirt == nullptr) return false;
        const uint32_t pageBytes = _Contract_Import_CorePage_v1.PageBytes != nullptr ? _Contract_Import_CorePage_v1.PageBytes() : 0;
        if (pageBytes < Nvme::kPageBytes) return false;
        for (uint32_t index = 0; index < kPageCount; ++index) {
            g_pagesPhysical[index] = _Contract_Import_CorePage_v1.Allocate(1);
            if (g_pagesPhysical[index] == 0) {
                for (uint32_t rollback = 0; rollback < index; ++rollback) {
                    if (_Contract_Import_CorePage_v1.Free != nullptr) (void)_Contract_Import_CorePage_v1.Free(g_pagesPhysical[rollback], 1);
                    g_pagesPhysical[rollback] = 0;
                    g_pagesVirtual[rollback] = nullptr;
                }
                return false;
            }
            g_pagesVirtual[index] = _Contract_Import_CorePhysMap_v1.ToVirt(g_pagesPhysical[index]);
            if (g_pagesVirtual[index] == nullptr) {
                for (uint32_t rollback = 0; rollback <= index; ++rollback) {
                    if (_Contract_Import_CorePage_v1.Free != nullptr) (void)_Contract_Import_CorePage_v1.Free(g_pagesPhysical[rollback], 1);
                    g_pagesPhysical[rollback] = 0;
                    g_pagesVirtual[rollback] = nullptr;
                }
                return false;
            }
            // 队列内存清零：上电残留不能当成队列条目
            if (_Contract_Import_CoreMem_v1.Zero != nullptr) {
                _Contract_Import_CoreMem_v1.Zero(g_pagesVirtual[index], pageBytes);
            }
        }
        g_pagesAllocated = 1;
        g_controller.pageBytes = pageBytes;
        return true;
    }

    // 归还四页缓冲
    void ReleasePages() {
        if (g_pagesAllocated == 0) return;
        for (uint32_t index = 0; index < kPageCount; ++index) {
            if (g_pagesPhysical[index] != 0 && _Contract_Import_CorePage_v1.Free != nullptr) {
                (void)_Contract_Import_CorePage_v1.Free(g_pagesPhysical[index], 1);
            }
            g_pagesPhysical[index] = 0;
            g_pagesVirtual[index] = nullptr;
        }
        g_pagesAllocated = 0;
    }

    // 填控制器实例的队列指针与物理地址：每张队列一页，页内偏移为零
    void FillQueues() {
        g_controller.adminSq = static_cast<uint8_t*>(g_pagesVirtual[0]);
        g_controller.adminSqPhysical = g_pagesPhysical[0];
        g_controller.adminCq = static_cast<uint8_t*>(g_pagesVirtual[1]);
        g_controller.adminCqPhysical = g_pagesPhysical[1];
        g_controller.ioSq = static_cast<uint8_t*>(g_pagesVirtual[2]);
        g_controller.ioSqPhysical = g_pagesPhysical[2];
        g_controller.ioCq = static_cast<uint8_t*>(g_pagesVirtual[3]);
        g_controller.ioCqPhysical = g_pagesPhysical[3];
        g_controller.data = static_cast<uint8_t*>(g_pagesVirtual[4]);
        g_controller.dataPhysical = g_pagesPhysical[4];
    }

    // 初始化结果映射成入口返回码并打固定诊断行
    KernelError ReportInitFailure(Nvme::Result result) {
        switch (result) {
            case Nvme::Result::Timeout:
                LogLine("WARN", "nvme: controller not ready");
                return KernelError::kTimeout;
            case Nvme::Result::NotSupported:
                LogLine("WARN", "nvme: controller not supported");
                return KernelError::kNotSupported;
            case Nvme::Result::Corrupt:
                LogLine("WARN", "nvme: identify data invalid");
                return KernelError::kCorrupt;
            case Nvme::Result::Io:
            case Nvme::Result::Ok:
                break;
        }
        LogLine("WARN", "nvme: controller init failed");
        return KernelError::kIo;
    }

    // 登记 DriverDesc 并绑定设备（块设备由 Vfs 按绑定事实登记）；失败只打诊断，Init 仍返回成功
    void RegisterAndBind(const DeviceFacts& facts) {
        if (_Contract_Import_DrvLoader_v1.RegisterDriver == nullptr) {
            LogLine("WARN", "nvme: Drv.Loader unavailable");
            return;
        }
        DriverDesc desc = {};
        desc.bytes = static_cast<uint32_t>(sizeof(DriverDesc));
        desc.driveSdkVersion = kDriveSdkVersion;
        desc.opsKind = 1;
        desc.busKind = facts.busKind;
        desc.classCode = facts.classCode;
        desc.vendorId = facts.vendorId;
        desc.deviceId = facts.deviceId;
        desc.flags = 0;
        desc.start = &StartDriver;
        desc.stop = &StopDriver;
        desc.ops = &Nvme::kOps;
        desc.context = &g_controller;
        const KernelError registered = _Contract_Import_DrvLoader_v1.RegisterDriver(&desc);
        if (registered != KernelError::kOk) {
            LogText("nvme: RegisterDriver failed error=");
            LogHex(static_cast<uint64_t>(registered));
            LogText("\r\n");
            return;
        }
        LogLine("NVME", "nvme: driver descriptor registered");
        if (g_deviceHandle == 0) {
            LogLine("WARN", "nvme: no devmodel controller, block device not published");
            return;
        }
        if (_Contract_Import_DrvLoader_v1.Bind != nullptr) {
            const KernelError bound = _Contract_Import_DrvLoader_v1.Bind(g_deviceHandle, &desc, &g_bindHandle);
            if (bound != KernelError::kOk) {
                LogText("nvme: Bind failed error=");
                LogHex(static_cast<uint64_t>(bound));
                LogText("\r\n");
                return;
            }
            LogLine("NVME", "nvme: device bound");
        }
        LogLine("OK", "nvme: driver bound, block device handed to devmodel");
    }

    // 驱动启用回调：只置就绪位，同步读服务在 Init 探测后已经可用
    Storage::Error StartDriver(void* context) {
        (void)context;
        g_ready = 1;
        LogLine("NVME", "nvme: driver started");
        return Storage::Error::Ok;
    }

    // 驱动停用回调：本阶段没有在途请求与中断
    void StopDriver(void* context) {
        (void)context;
        g_ready = 0;
    }
}

// Init 入口：找控制器、映射 BAR0、申请队列页、初始化控制器并登记驱动
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    g_logReady = _Contract_Import_CoreLog_v1.Line != nullptr ? 1u : 0u;
    LogLine("NVME", "nvme: probing controller");
    DeviceFacts facts = {};
    if (!FindController(&facts)) {
        LogLine("WARN", "nvme: no device");
        return KernelError::kOk;
    }
    g_deviceHandle = facts.deviceHandle;
    // 设备认领归 Drv.Loader.Bind：驱动自己不再 Claim
    uint64_t barBase = 0;
    uint64_t barBytes = 0;
    if (!FindBar0(facts, &barBase, &barBytes)) {
        LogLine("WARN", "nvme: BAR0 not found in devmodel resources");
        return KernelError::kOk;
    }
    void* abar = nullptr;
    if (_Contract_Import_CoreMmio_v1.Map != nullptr) abar = _Contract_Import_CoreMmio_v1.Map(barBase, barBytes, 0);
    if (abar == nullptr && _Contract_Import_CorePhysMap_v1.ToVirt != nullptr) abar = _Contract_Import_CorePhysMap_v1.ToVirt(barBase);
    if (abar == nullptr) {
        LogLine("WARN", "nvme: BAR0 not mappable");
        return KernelError::kOk;
    }
    g_controller.registers.read = &RegisterRead;
    g_controller.registers.write = &RegisterWrite;
    g_controller.registers.context = abar;
    if (!AllocatePages()) {
        LogLine("WARN", "nvme: DMA pages not available");
        g_controller.registers = Nvme::RegisterAccess{};
        return KernelError::kOk;
    }
    FillQueues();
    const Nvme::Result result = Nvme::Initialize(g_controller);
    if (result != Nvme::Result::Ok) {
        Nvme::Shutdown(g_controller);
        ReleasePages();
        g_controller.registers = Nvme::RegisterAccess{};
        return ReportInitFailure(result);
    }
    LogLine("NVME", "nvme: controller ready");
    char line[kDeviceLineBytes] = {};
    if (Nvme::DescribeDevice(g_controller, line, sizeof(line)) != 0) {
        LogText(line);
        LogText("\r\n");
    }
    RegisterAndBind(facts);
    return KernelError::kOk;
}

// Abort 入口：反向撤销绑定（认领与释放归 Drv.Loader.Stop）、停控制器、归还队列页
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    if (g_bindHandle != 0 && _Contract_Import_DrvLoader_v1.Stop != nullptr) {
        (void)_Contract_Import_DrvLoader_v1.Stop(g_bindHandle);
        g_bindHandle = 0;
    }
    Nvme::Shutdown(g_controller);
    ReleasePages();
    g_controller.registers = Nvme::RegisterAccess{};
    g_ready = 0;
    return KernelError::kOk;
}
