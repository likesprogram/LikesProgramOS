/* ModuleRegistry.cpp
    ModuleRegistry 与内核核心导出的实现：记录登记、导出池水位，以及 Core.* 十张槽表

    槽表符号是生成头 kKernelExports 的链接目标：符号名、类型与槽数由生成头钉住，
    本文件只提供实现；需要平台状态的槽经 BindCoreServices 绑定，未绑定时按未就绪返回
    本文件不打印、不停机，也不产生诊断编号（编号由调用方选）
*/

#include <ModuleRegistry.hpp>

#include <Libc.hpp>
#include <MmioMap.hpp>
#include <Objects.hpp>
#include <Paging.hpp>
#include <Print.hpp>
#include <Reset.hpp>
#include <Tags.hpp>

namespace {
    // 内核核心服务绑定：未绑定时相关槽按“未就绪”返回空值或零
    Kernel::PageAllocator* s_pages = nullptr;   // Core.Page 的页帧分配器
    const Boot::Info* s_bootInfo = nullptr;     // Core.Boot 的交接事实与 Core.Abi 槽 1 的来源

    // —— Core.Abi ——
    // 槽 1：内核集 BuildId 前 8 字节按小端装入；未绑定 BootInfo 时返回 0
    uint64_t CoreAbiBuildId64() {
        if (s_bootInfo == nullptr) return 0;
        uint64_t value = 0;
        for (uint32_t index = 0; index < 8; ++index) value |= static_cast<uint64_t>(s_bootInfo->kernelSetBuildId[index]) << (8 * index);
        return value;
    }

    // —— Core.Log ——
    // 槽 0：写文本；槽 1：写 [ tag ] text 一行；槽 2：写 16 位十六进制
    // 槽 3：写模块诊断行 [ tag ] text (code=0x… detail)，code 是模块命名空间内的审计码
    // 标签按等级名着色（与 Kernel::Tags 同一张属性表），模块名按缺省属性；流式通道忽略属性
    void CoreLogWrite(const char* text) { Print::Write(text); }

    void CoreLogLine(const char* tag, const char* text) {
        const char* const label = tag != nullptr ? tag : "INFO";
        Print::Write("[ ");
        Print::SetAttribute(Kernel::Tags::TagAttributeByName(label));
        Print::Write(label);
        Print::SetAttribute(Kernel::Tags::kDefaultAttribute);
        Print::Write(" ] ");
        Print::Write(text != nullptr ? text : "");
        Print::Write("\r\n");
    }

    void CoreLogHex(uint64_t value) { Print::WriteHex64(value); }

    void CoreLogCode(const char* tag, uint32_t code, const char* text, const char* detail) {
        const char* const label = tag != nullptr ? tag : "INFO";
        Print::Write("[ ");
        Print::SetAttribute(Kernel::Tags::TagAttributeByName(label));
        Print::Write(label);
        Print::SetAttribute(Kernel::Tags::kDefaultAttribute);
        Print::Write(" ] ");
        Print::Write(text != nullptr ? text : "");
        Print::Write(" (code=");
        Print::WriteHex(code);
        if (detail != nullptr) {
            Print::Write(" ");
            Print::Write(detail);
        }
        Print::Write(")\r\n");
    }

    // —— Core.Mem ——
    // 槽 0..3：拷贝、重叠安全拷贝、清零、比较；空指针与零长度按无操作处理
    void CoreMemCopy(void* destination, const void* source, uint64_t bytes) {
        if (destination == nullptr || source == nullptr || bytes == 0) return;
        memcpy(destination, source, static_cast<size_t>(bytes));
    }

    void* CoreMemCopyOverlap(void* destination, const void* source, uint64_t bytes) {
        if (destination == nullptr || source == nullptr || bytes == 0) return destination;
        return memmove(destination, source, static_cast<size_t>(bytes));
    }

    void CoreMemZero(void* destination, uint64_t bytes) {
        if (destination == nullptr || bytes == 0) return;
        memset(destination, 0, static_cast<size_t>(bytes));
    }

    int32_t CoreMemCompare(const void* left, const void* right, uint64_t bytes) {
        if (left == nullptr || right == nullptr) return 0;
        return static_cast<int32_t>(memcmp(left, right, static_cast<size_t>(bytes)));
    }

    // —— Core.Page ——
    // 槽 0：页大小；槽 1：分配连续页（失败返回 0）；槽 2：归还页；槽 3：空闲页数；槽 4：管理上限
    uint32_t CorePageBytes() { return static_cast<uint32_t>(Kernel::kPageBytes); }

    uint64_t CorePageAllocate(uint32_t pages) { return s_pages != nullptr ? s_pages->Allocate(pages) : 0; }

    KernelError CorePageFree(uint64_t physical, uint32_t pages) {
        if (s_pages == nullptr) return KernelError::kState;
        if (physical == 0 || pages == 0) return KernelError::kInvalidArgument;
        s_pages->Free(physical, pages);
        return KernelError::kOk;
    }

    uint32_t CorePageFreePages() { return s_pages != nullptr ? static_cast<uint32_t>(s_pages->FreePages()) : 0; }

    uint64_t CorePageManagedLimit() { return Kernel::kManagedLimitBytes; }

    // —— Core.PhysMap ——
    // 槽 0：物理→直映射指针；槽 1：直映射指针→物理；槽 2：直映射覆盖上限
    void* CorePhysMapToVirt(uint64_t physical) { return physical < Kernel::kMapLimitBytes ? Kernel::PhysToVirt(physical) : nullptr; }

    KernelError CorePhysMapToPhys(const void* address, uint64_t* physicalOut) {
        if (address == nullptr || physicalOut == nullptr) return KernelError::kInvalidArgument;
        const uintptr_t value = reinterpret_cast<uintptr_t>(address);
        // 直映射窗口：基址加物理地址
        if (value >= Kernel::kPhysMapBase && value - Kernel::kPhysMapBase < Kernel::kMapLimitBytes) {
            *physicalOut = static_cast<uint64_t>(value - Kernel::kPhysMapBase);
            return KernelError::kOk;
        }
        // 高地址 MMIO 窗口：按映射表反查（返回该条映射的物理起址与总长度）
        uint64_t physical = 0;
        uint64_t bytes = 0;
        if (Kernel::MmioWindow::Lookup(address, &physical, &bytes)) {
            *physicalOut = physical + static_cast<uint64_t>(value - Kernel::kMmioWindowBase);
            return KernelError::kOk;
        }
        return KernelError::kInvalidArgument;
    }

    uint64_t CorePhysMapLimitBytes() { return Kernel::kMapLimitBytes; }

    // —— Core.Time ——
    // 槽 0：TSC 计数；槽 1：每毫秒计数（PIT 标定结果，经 Reset 的只读入口取）；
    // 槽 2：忙等，交给 Reset 的有界等待（PIT 标定 TSC，标定不成立时退到有界忙等）
    uint64_t CoreTimeTicks() {
        uint32_t low = 0;
        uint32_t high = 0;
        asm volatile("rdtsc" : "=a"(low), "=d"(high));
        return (static_cast<uint64_t>(high) << 32) | low;
    }

    // 标定未就绪或时基不可用时返回 0；标定值按 32 位上限 clamp 兜底（正常 TSC 频率远低于上限）
    uint32_t CoreTimeTicksPerMs() {
        const uint64_t ticks = _Kernel_TicksPerMs();
        return ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(ticks);
    }

    void CoreTimeBusyWaitMs(uint32_t milliseconds) { (void)_Kernel_WaitMs(milliseconds); }

    // —— Core.Boot ——
    // 槽 0：BootInfo 只读视图；槽 1：区间完整落在受管物理内存内时给直映射指针，否则空
    const Boot::Info* CoreBootInfo() { return s_bootInfo; }

    const void* CoreBootPhysical(uint64_t physical, uint64_t bytes) {
        if (physical == 0 || bytes == 0 || physical >= Kernel::kMapLimitBytes) return nullptr;
        if (bytes > Kernel::kMapLimitBytes - physical) return nullptr;
        return Kernel::PhysToVirt(physical);
    }

    // —— Core.Dma ——
    // 当前没有 DMA 分配器：如实报不支持，不做“物理连续”的假保证（后续阶段落地）
    KernelError CoreDmaAlloc(uint64_t bytes, uint32_t align, uint64_t* physicalOut, void** virtualOut) {
        (void)bytes;
        (void)align;
        (void)physicalOut;
        (void)virtualOut;
        return KernelError::kNotSupported;
    }

    KernelError CoreDmaFree(uint64_t physical, uint64_t bytes) {
        (void)physical;
        (void)bytes;
        return KernelError::kNotSupported;
    }

    // —— Core.Mmio ——
    // <4 GiB 走直映射（忽略缓存策略，Unmap 无操作）；4 GiB 以上走 4 KiB MMIO 窗口
    // （MmioWindow 惰性安装窗口、按 4 KiB PTE 建映射，PCD|PWT 缓存禁用，支持 Unmap 与反查）
    void* CoreMmioMap(uint64_t physical, uint64_t bytes, uint32_t cachePolicy) {
        if (physical == 0 || bytes == 0) return nullptr;
        if (physical >= Kernel::kMapLimitBytes) return Kernel::MmioWindow::Map(physical, bytes, cachePolicy);
        if (bytes > Kernel::kMapLimitBytes - physical) return nullptr;
        return Kernel::PhysToVirt(physical);
    }

    // 窗口内地址交给 MmioWindow 解除；直映射不可撤销，按无操作成功返回
    KernelError CoreMmioUnmap(void* base, uint64_t bytes) {
        (void)bytes;
        if (base == nullptr) return KernelError::kInvalidArgument;
        const uintptr_t address = reinterpret_cast<uintptr_t>(base);
        if (address >= Kernel::kMmioWindowBase && address - Kernel::kMmioWindowBase < Kernel::kMmioWindowSpanBytes)
            return Kernel::MmioWindow::Unmap(base) ? KernelError::kOk : KernelError::kInvalidArgument;
        return KernelError::kOk;
    }
}

// —— 内核核心导出槽表：符号名与槽序由生成头 Slots.hpp 钉住，实现按槽位语义 ——
// 命名规则是 _Contract_Export_<稳定名去点>_v<版本>（Core.Page → _Contract_Export_CorePage_v1），
// 与 PackModule 从 ELF 符号表取导出表的规则同一处（生成头与打包器共用）
// 定义不带 extern "C" 也不带 const：生成头已用 extern "C" 声明同名符号并继承 C 链接性，
// 定义再加 extern "C" 会被 -Werror 判成 “initialized and declared 'extern'”；
// 槽表按模块侧约定是非 const（允许落在可写段），两侧用同一份声明，避免同一符号两处类型不一致
CoreAbiSlotsV1 _Contract_Export_CoreAbi_v1 = {&_Contract_CoreAbi_Version, &CoreAbiBuildId64, &_Contract_CoreAbi_DriveSdkVersion};

CoreBootSlotsV1 _Contract_Export_CoreBoot_v1 = {&CoreBootInfo, &CoreBootPhysical};

CoreDmaSlotsV1 _Contract_Export_CoreDma_v1 = {&CoreDmaAlloc, &CoreDmaFree};

CoreLogSlotsV1 _Contract_Export_CoreLog_v1 = {&CoreLogWrite, &CoreLogLine, &CoreLogHex, &CoreLogCode};

CoreMemSlotsV1 _Contract_Export_CoreMem_v1 = {&CoreMemCopy, &CoreMemCopyOverlap, &CoreMemZero, &CoreMemCompare};

CoreMmioSlotsV1 _Contract_Export_CoreMmio_v1 = {&CoreMmioMap, &CoreMmioUnmap};

CoreObjectSlotsV1 _Contract_Export_CoreObject_v1 = Kernel::Objects::kCoreObjectSlots;

CorePageSlotsV1 _Contract_Export_CorePage_v1 = {
    &CorePageBytes, &CorePageAllocate, &CorePageFree, &CorePageFreePages, &CorePageManagedLimit};

CorePhysMapSlotsV1 _Contract_Export_CorePhysMap_v1 = {
    &CorePhysMapToVirt, &CorePhysMapToPhys, &CorePhysMapLimitBytes};

CoreTimeSlotsV1 _Contract_Export_CoreTime_v1 = {&CoreTimeTicks, &CoreTimeTicksPerMs, &CoreTimeBusyWaitMs};

namespace Kernel {
    void ModuleRegistry::Reset() {
        for (uint32_t index = 0; index < m_count; ++index) m_records[index] = ModuleRecord{};
        m_count = 0;
        for (uint32_t index = 0; index < m_exportCount; ++index) m_exportEntries[index] = ModuleExportEntry{};
        m_exportCount = 0;
        for (uint32_t index = 0; index < m_slotCount; ++index) m_slotPointers[index] = nullptr;
        m_slotCount = 0;
    }

    ModuleRecord* ModuleRegistry::Add(const ModuleId& moduleId, uint16_t major, uint16_t minor, uint16_t patch, uint64_t base, uint32_t imageBytes, uint32_t pageCount, uint32_t preloadOrder) {
        if (m_count >= kModuleMaxRecords) return nullptr;
        if (Contains(moduleId)) return nullptr;
        ModuleRecord& record = m_records[m_count];
        record = ModuleRecord{};
        record.moduleId = moduleId;
        record.major = major;
        record.minor = minor;
        record.patch = patch;
        record.base = base;
        record.imageBytes = imageBytes;
        record.pageCount = pageCount;
        record.preloadOrder = preloadOrder;
        record.state = ModuleState::Loaded;
        ++m_count;
        return &record;
    }

    ModuleRecord* ModuleRegistry::Find(const ModuleId& moduleId) {
        for (uint32_t index = 0; index < m_count; ++index) {
            if (m_records[index].moduleId == moduleId) return &m_records[index];
        }
        return nullptr;
    }

    const ModuleRecord* ModuleRegistry::Find(const ModuleId& moduleId) const {
        for (uint32_t index = 0; index < m_count; ++index) {
            if (m_records[index].moduleId == moduleId) return &m_records[index];
        }
        return nullptr;
    }

    bool ModuleRegistry::Contains(const ModuleId& moduleId) const { return Find(moduleId) != nullptr; }

    const ModuleRecord* ModuleRegistry::At(uint32_t index) const { return index < m_count ? &m_records[index] : nullptr; }

    bool ModuleRegistry::SetState(const ModuleId& moduleId, ModuleState state) {
        ModuleRecord* const record = Find(moduleId);
        if (record == nullptr) return false;
        record->state = state;
        return true;
    }

    const void** ModuleRegistry::AllocateSlotPointers(uint32_t slotCount) {
        if (slotCount == 0 || slotCount > kModuleMaxSlotPointers - m_slotCount) return nullptr;
        const void** const first = &m_slotPointers[m_slotCount];
        m_slotCount += slotCount;
        return first;
    }

    ModuleExportEntry* ModuleRegistry::AllocateExportEntries(uint32_t exportCount) {
        if (exportCount == 0 || exportCount > kModuleMaxExportEntries - m_exportCount) return nullptr;
        ModuleExportEntry* const first = &m_exportEntries[m_exportCount];
        m_exportCount += exportCount;
        return first;
    }

    ExportPoolMark ModuleRegistry::MarkPools() const {
        ExportPoolMark mark;
        mark.exportCount = m_exportCount;
        mark.slotCount = m_slotCount;
        return mark;
    }

    void ModuleRegistry::RollbackPools(const ExportPoolMark& mark) {
        for (uint32_t index = mark.exportCount; index < m_exportCount; ++index) m_exportEntries[index] = ModuleExportEntry{};
        for (uint32_t index = mark.slotCount; index < m_slotCount; ++index) m_slotPointers[index] = nullptr;
        m_exportCount = mark.exportCount;
        m_slotCount = mark.slotCount;
    }

    void BindCoreServices(PageAllocator* pages, const Boot::Info* bootInfo) {
        s_pages = pages;
        s_bootInfo = bootInfo;
        // MMIO 窗口的页表页帧来自同一个分配器；窗口本身在第一次高地址 Map 时惰性安装
        Kernel::MmioWindow::Bind(pages);
    }

    // 登记与核对内核核心导出：失败按 E-KRN-59（ABI/契约失败并入此号，detail 区分失败项）
    // E-KRN-56 只留给 BuildId 与模块主版本硬门，不再用于导出登记失败
    Diagnostics::Diagnostic InitializeCoreExports(InterfaceRegistry& registry, PageAllocator* pages, const Boot::Info* bootInfo, const char*& reason) {
        reason = nullptr;
        BindCoreServices(pages, bootInfo);
        registry.Reset();
        const RegistryResult registered = RegisterKernelCoreExports(registry);
        if (registered.error != KernelError::kOk) {
            reason = registered.reason;
            return Diagnostics::kFailModuleInit;
        }
        const RegistryResult verified = registry.VerifyAbi();
        if (verified.error != KernelError::kOk) {
            reason = verified.reason;
            return Diagnostics::kFailModuleInit;
        }
        return Diagnostics::Diagnostic{};
    }
}
