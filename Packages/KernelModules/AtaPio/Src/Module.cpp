/* Module.cpp
    AtaPio 模块入口：探测传统 ATA / ATAPI 设备，登记 DriverDesc 并把块设备发布给 Block.Registry

    启动闭包里的驱动模块：Init 里做可撤销准备（读设备事实、登记驱动描述与绑定），
    不启用中断、不建线程；读路径同步轮询，与运行期共用同一实例
    Init 顺序：探测两条通道的四个盘位 → 按 BootInfo.media 选启动盘 → 在 Dev.Model 里
    找 IDE 控制器设备 → Drv.Loader.RegisterDriver + Bind（块设备由 Vfs 按绑定事实登记）
    没有设备时 Init 仍返回成功并打 "ata: no device"：闭包模块是必需模块，缺硬件不该让
    整集装载失败；挂卷失败由挂载阶段的诊断编号报告
    块设备回调按 context 取盘位事实，context 是模块内静态 Drive 的地址，实例与模块同寿命
*/

#include <ModuleAbi.hpp>

#include <Abi.hpp>

#include "AtaPio.hpp"

// 导入回填区：符号名由生成头钉住，装载器在 Init 前把提供者槽表指针写进这些槽
LP_CONTRACT_IMPORT(CoreBoot, 1, CoreBootSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(DevModel, 1, DevModelSlotsV1) = {};
LP_CONTRACT_IMPORT(DrvLoader, 1, DrvLoaderSlotsV1) = {};

namespace {
    // 探测到的盘位事实与启动盘下标：静态存储，实例与模块同寿命
    AtaPio::Drive g_drives[AtaPio::kMaxDrives];
    uint32_t g_driveCount = 0;
    uint32_t g_bootIndex = 0;
    // 驱动实例状态：Start 之后才置就绪位（本阶段只启用同步服务）
    uint32_t g_ready = 0;
    // 登记与发布句柄：Abort 时按相反顺序撤销
    uint32_t g_deviceHandle = 0;
    uint32_t g_bindHandle = 0;
    // 日志槽是否可用：契约导入表被装载器填好之前不调用
    uint32_t g_logReady = 0;
    // BootInfo 的启动介质编码：Hdd = 2、Cdrom = 4（见 BootInfo.hpp 的 Media）
    constexpr uint32_t kMediaHdd = 2;
    constexpr uint32_t kMediaCdrom = 4;
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

    // 打印一个盘位的标识行：型号、扇区数、逻辑扇区大小，供集成阶段断言
    void LogDrive(const AtaPio::Drive& drive) {
        LogText("ata: ");
        LogText(drive.model);
        LogText(drive.kind == AtaPio::kKindAtapi ? " atapi sectors=" : " ata sectors=");
        LogHex(drive.sectors);
        LogText(" block=");
        LogHex(drive.sectorBytes);
        LogText("\r\n");
    }

    // —— BlockDeviceOps 回调：context 是盘位事实的地址 ——
    uint32_t OpsBlockBytes(void* context) { return static_cast<const AtaPio::Drive*>(context)->sectorBytes; }

    uint64_t OpsCapacityBytes(void* context) {
        const AtaPio::Drive* drive = static_cast<const AtaPio::Drive*>(context);
        return drive->sectors * drive->sectorBytes;
    }

    uint32_t OpsWritable(void* context) {
        // 物理盘可写；本阶段只有读路径，Write 仍返回只读
        return static_cast<const AtaPio::Drive*>(context)->kind == AtaPio::kKindAta ? 1u : 0u;
    }

    Storage::Error OpsRead(void* context, uint64_t offset, void* buffer, uint32_t bytes) {
        return AtaPio::Read(*static_cast<const AtaPio::Drive*>(context), offset, buffer, bytes);
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
        LogLine("ATA", "ata: driver started");
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

    // 是否传统 IDE 控制器：大容量存储类（0x01）的传统 IDE 子类（0x01）
    bool IsIdeController(const DeviceFacts& facts) {
        return ClassOf(facts.classCode) == 0x01u && SubclassOf(facts.classCode) == 0x01u;
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

    // 在 Dev.Model 里找 IDE 控制器：启动候选里匹配的优先，其次全量枚举里第一个匹配
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
            if (!IsIdeController(facts)) continue;
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

    // 按启动介质选盘位：Hdd 优先 ATA、Cdrom 优先 ATAPI；介质未知时先 ATA 再 ATAPI
    uint32_t ChooseBootIndex(uint32_t media) {
        uint32_t preferred = AtaPio::kKindNone;
        if (media == kMediaHdd) preferred = AtaPio::kKindAta;
        if (media == kMediaCdrom) preferred = AtaPio::kKindAtapi;
        if (preferred != AtaPio::kKindNone) {
            for (uint32_t index = 0; index < g_driveCount; ++index) {
                if (g_drives[index].kind == preferred) return index;
            }
        }
        for (uint32_t index = 0; index < g_driveCount; ++index) {
            if (g_drives[index].kind == AtaPio::kKindAta) return index;
        }
        return 0;
    }

    // 登记 DriverDesc 并绑定设备（块设备由 Vfs 按绑定事实登记）；各步失败只打诊断，Init 仍返回成功
    void RegisterAndBind(const DeviceFacts& facts, bool factsValid) {
        if (_Contract_Import_DrvLoader_v1.RegisterDriver == nullptr) {
            LogLine("WARN", "ata: Drv.Loader unavailable");
            return;
        }
        // DriverDesc v1：匹配字段来自 Dev.Model 的事实；没有事实时用通配值，由绑定方决定
        DriverDesc desc = {};
        desc.bytes = static_cast<uint32_t>(sizeof(DriverDesc));
        desc.driveSdkVersion = kDriveSdkVersion;
        desc.opsKind = 1;   // 1 = BlockDeviceOps
        desc.busKind = factsValid ? facts.busKind : 0u;
        desc.classCode = factsValid ? facts.classCode : 0xFFFFFFFFu;
        desc.vendorId = factsValid ? facts.vendorId : 0xFFFFu;
        desc.deviceId = factsValid ? facts.deviceId : 0xFFFFFFFFu;
        desc.flags = 0;
        desc.start = &StartDriver;
        desc.stop = &StopDriver;
        desc.ops = &kOps;
        desc.context = &g_drives[g_bootIndex];
        const KernelError registered = _Contract_Import_DrvLoader_v1.RegisterDriver(&desc);
        if (registered != KernelError::kOk) {
            LogText("ata: RegisterDriver failed error=");
            LogHex(static_cast<uint64_t>(registered));
            LogText("\r\n");
            return;
        }
        LogLine("ATA", "ata: driver descriptor registered");
        if (g_deviceHandle == 0) {
            LogLine("WARN", "ata: no devmodel controller, block device not published");
            return;
        }
        if (_Contract_Import_DrvLoader_v1.Bind != nullptr) {
            const KernelError bound = _Contract_Import_DrvLoader_v1.Bind(g_deviceHandle, &desc, &g_bindHandle);
            if (bound != KernelError::kOk) {
                LogText("ata: Bind failed error=");
                LogHex(static_cast<uint64_t>(bound));
                LogText("\r\n");
                return;
            }
            LogLine("ATA", "ata: device bound");
        }
        LogLine("OK", "ata: driver bound, block device handed to devmodel");
    }
}

// Init 入口：探测设备、按启动介质选盘、登记驱动描述并发布块设备
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    g_logReady = _Contract_Import_CoreLog_v1.Line != nullptr ? 1u : 0u;
    LogLine("ATA", "ata: probing legacy channels");
    g_driveCount = AtaPio::Probe(g_drives, AtaPio::kMaxDrives);
    for (uint32_t index = 0; index < g_driveCount; ++index) LogDrive(g_drives[index]);
    LogText("ata: found=");
    LogHex(g_driveCount);
    LogText("\r\n");
    if (g_driveCount == 0) {
        LogLine("ATA", "ata: no device");
        return KernelError::kOk;
    }
    // 启动介质决定选盘优先级：Hdd 优先 ATA，Cdrom 优先 ATAPI
    uint32_t media = 0;
    if (_Contract_Import_CoreBoot_v1.Info != nullptr) {
        const Boot::Info* bootInfo = _Contract_Import_CoreBoot_v1.Info();
        if (bootInfo != nullptr) media = bootInfo->media;
    }
    g_bootIndex = ChooseBootIndex(media);
    LogText("ata: boot drive index=");
    LogHex(g_bootIndex);
    LogText(" media=");
    LogHex(media);
    LogText("\r\n");
    // Dev.Model 提供控制器设备句柄：按 IDE 控制器匹配；没有匹配也能只登记驱动描述
    DeviceFacts facts = {};
    const bool factsValid = FindController(&facts);
    if (factsValid) {
        g_deviceHandle = facts.deviceHandle;
        // 设备认领归 Drv.Loader.Bind：驱动自己不再 Claim，
        // 否则 Bind 内部再 Claim 一次会拿到 kBusy，绑定建不起来
        LogLine("ATA", "ata: controller found");
    } else {
        LogLine("WARN", "ata: controller not found in devmodel");
    }
    RegisterAndBind(facts, factsValid);
    return KernelError::kOk;
}

// Abort 入口：反向撤销绑定（认领与释放归 Drv.Loader.Bind/Stop）
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    if (g_bindHandle != 0 && _Contract_Import_DrvLoader_v1.Stop != nullptr) {
        (void)_Contract_Import_DrvLoader_v1.Stop(g_bindHandle);
        g_bindHandle = 0;
    }
    g_ready = 0;
    return KernelError::kOk;
}
