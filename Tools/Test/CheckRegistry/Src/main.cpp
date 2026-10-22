/* main.cpp
    CheckRegistry：接口注册表的宿主侧检查，复用内核同一份 InterfaceRegistry 实现

    检查分五类（用例数固定，实跑数量与声明不符即内部错误）：
      1  导出登记：内核核心 10 个 Core.* 替身与闭包模块导出进入同一张表，提供者与槽表指针正确
      2  提供者唯一：重复 (InterfaceId, Version)、组内重复、模块与内核核心冲突都拒绝，
         且整组原子、不覆盖先登记者
      3  导入版本选择：区间内取最高版本、上限哨兵、区间不满足、未定义接口、区间非法
      4  槽位：使用槽数超过所选最高版本时拒绝且不回退低版本；回填逐槽指针正确
      5  ABI 版本与生命周期：VerifyAbi 核对 Core.Abi 槽 0/2、版本常量与生成头一致、Mark/Rollback

    替身表只构造导出条目，不链接内核核心的 _Contract_Export_* 槽表符号；
    真符号接入后再补链接真实表的用例；注册表是机制层，失败只给 KernelError 与原因文本，不产生编号
*/

#include <InterfaceRegistry.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    // 固定用例数：与 RunCases 里的注册数量一致
    constexpr unsigned kCaseCount = 21;
    // 已跑用例数与失败用例数
    unsigned g_ran = 0;
    unsigned g_failed = 0;

    // 失败即抛异常，由 Case 捕获并计失败
    void Fail(const std::string& message) { throw std::runtime_error(message); }

    // 断言条件成立
    void Expect(bool condition, const char* what) {
        if (!condition) Fail(what);
    }

    // 断言错误码
    void ExpectError(const Kernel::RegistryResult& result, KernelError expected, const char* what) {
        if (result.error != expected) Fail(std::string(what) + "：错误码不符");
    }

    // 跑一个用例并计数：异常计为失败，不中断其余用例
    void Case(const char* name, void (*function)()) {
        ++g_ran;
        try {
            function();
        } catch (const std::exception& error) {
            ++g_failed;
            std::cerr << "CheckRegistry: 用例失败：" << name << "：" << error.what() << '\n';
        }
    }

    // 替身 Core.Page v1 槽表：函数返回可以区分的值，回填用例逐槽比对
    uint32_t FakePageBytes() { return 4096; }
    uint64_t FakeAllocate(uint32_t pages) { return static_cast<uint64_t>(pages); }
    KernelError FakeFree(uint64_t physical, uint32_t pages) {
        (void)physical;
        (void)pages;
        return KernelError::kOk;
    }
    uint32_t FakeFreePages() { return 7; }
    uint64_t FakeManagedLimit() { return 0x100000; }
    const CorePageSlotsV1 kFakeCorePage = { FakePageBytes, FakeAllocate, FakeFree, FakeFreePages, FakeManagedLimit };

    // 替身 Core.Abi v1 槽表：返回生成头的两个版本常量
    uint16_t FakeAbiVersion() { return kKernelAbiVersion; }
    uint64_t FakeBuildId64() { return 0; }
    uint16_t FakeAbiDriveSdkVersion() { return kDriveSdkVersion; }
    const CoreAbiSlotsV1 kFakeCoreAbi = { FakeAbiVersion, FakeBuildId64, FakeAbiDriveSdkVersion };

    // 返回值与生成头不一致的替身 Core.Abi：槽 0 与槽 2 各一份，供 ABI 核对负例
    uint16_t BadAbiVersion() { return static_cast<uint16_t>(kKernelAbiVersion + 1); }
    uint16_t BadAbiDriveSdkVersion() { return static_cast<uint16_t>(kDriveSdkVersion + 1); }
    const CoreAbiSlotsV1 kBadAbiVersionSlots = { BadAbiVersion, FakeBuildId64, FakeAbiDriveSdkVersion };
    const CoreAbiSlotsV1 kBadAbiDriveSdkSlots = { FakeAbiVersion, FakeBuildId64, BadAbiDriveSdkVersion };

    // 其余 Core.* 替身槽表：登记只要求槽表指针非空，内容不参与本检查
    const CoreBootSlotsV1 kFakeCoreBoot = {};
    const CoreDmaSlotsV1 kFakeCoreDma = {};
    const CoreLogSlotsV1 kFakeCoreLog = {};
    const CoreMemSlotsV1 kFakeCoreMem = {};
    const CoreMmioSlotsV1 kFakeCoreMmio = {};
    const CoreObjectSlotsV1 kFakeCoreObject = {};
    const CorePhysMapSlotsV1 kFakeCorePhysMap = {};
    const CoreTimeSlotsV1 kFakeCoreTime = {};

    // 闭包单例替身槽表：登记只要求槽表指针非空
    const DevModelSlotsV1 kFakeDevModel = {};
    const DrvLoaderSlotsV1 kFakeDrvLoader = {};
    const BlockRegistrySlotsV1 kFakeBlockRegistry = {};
    const FsVfsSlotsV1 kFakeFsVfs = {};
    const BootVolumeSlotsV1 kFakeBootVolume = {};

    // 内核核心 10 个 Core.* 的替身导出：槽数与生成头 kKernelExports 一致
    const ModuleExportEntry kFakeKernelExports[] = {
        { kIfCoreAbi, 1, 3, &kFakeCoreAbi },
        { kIfCoreBoot, 1, 2, &kFakeCoreBoot },
        { kIfCoreDma, 1, 2, &kFakeCoreDma },
        { kIfCoreLog, 1, 4, &kFakeCoreLog },
        { kIfCoreMem, 1, 4, &kFakeCoreMem },
        { kIfCoreMmio, 1, 2, &kFakeCoreMmio },
        { kIfCoreObject, 1, 4, &kFakeCoreObject },
        { kIfCorePage, 1, 5, &kFakeCorePage },
        { kIfCorePhysMap, 1, 3, &kFakeCorePhysMap },
        { kIfCoreTime, 1, 3, &kFakeCoreTime },
    };

    // 内核核心替身导出条数
    constexpr uint16_t kFakeKernelExportCount = static_cast<uint16_t>(sizeof(kFakeKernelExports) / sizeof(kFakeKernelExports[0]));

    // 闭包单例的替身导出：按提供者模块分组，与生成头 kClosureProviders 的固定提供者一致
    const ModuleExportEntry kFakeDevModelExports[] = { { kIfDevModel, 1, 6, &kFakeDevModel } };
    const ModuleExportEntry kFakeDriverLoaderExports[] = { { kIfDrvLoader, 1, 4, &kFakeDrvLoader } };
    const ModuleExportEntry kFakeVfsExports[] = {
        { kIfBlockRegistry, 1, 4, &kFakeBlockRegistry },
        { kIfBootVolume, 1, 1, &kFakeBootVolume },
        { kIfFsVfs, 1, 8, &kFakeFsVfs },
    };

    // 登记内核核心替身导出：失败即用例失败
    void RegisterFakeKernelCore(Kernel::InterfaceRegistry& registry) {
        const Kernel::RegistryResult result = registry.RegisterExports(kFakeKernelExports, kFakeKernelExportCount, kModuleKernelCore);
        Expect(result.error == KernelError::kOk, "内核核心替身导出登记失败");
    }

    // 用例 1：内核核心 10 个 Core.* 导出登记成功，提供者与槽表指针正确
    void CaseRegisterKernelCore() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        Expect(registry.Count() == kFakeKernelExportCount, "内核核心导出条数与替身表不符");
        for (const ModuleExportEntry& entry : kFakeKernelExports) {
            const Kernel::RegisteredExport* registered = registry.Find(entry.id, entry.version);
            Expect(registered != nullptr, "内核核心导出未命中");
            Expect(registered->provider == kModuleKernelCore, "内核核心导出的提供者不是全零模块标识");
            Expect(registered->entry.slots == entry.slots, "登记的槽表指针与替身不一致");
        }
        Expect(Kernel::InterfaceRegistry::AbiVersion() == kKernelAbiVersion, "AbiVersion 与生成头不一致");
        Expect(Kernel::InterfaceRegistry::DriveSdkVersion() == kDriveSdkVersion, "DriveSdkVersion 与生成头不一致");
    }

    // 用例 2：重复提供者拒绝且不覆盖先登记者
    void CaseRegisterDuplicateProvider() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        const ModuleExportEntry duplicate[] = { { kIfCorePage, 1, 5, &kFakeCorePage } };
        const Kernel::RegistryResult result = registry.RegisterExports(duplicate, 1, kModVfs);
        ExpectError(result, KernelError::kAlreadyExists, "重复提供者未被拒绝");
        Expect(result.reason != nullptr, "重复提供者没有原因文本");
        Expect(registry.Count() == kFakeKernelExportCount, "重复登记改变了已登记条数");
        const Kernel::RegisteredExport* page = registry.Find(kIfCorePage, 1);
        Expect(page != nullptr && page->provider == kModuleKernelCore, "重复登记覆盖了先登记者");
    }

    // 用例 3：模块导出与内核核心导出合并登记
    void CaseRegisterModuleExports() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        Expect(registry.RegisterExports(kFakeVfsExports, 3, kModVfs).error == KernelError::kOk, "Vfs 模块导出登记失败");
        Expect(registry.RegisterExports(kFakeDevModelExports, 1, kModDevModel).error == KernelError::kOk, "DevModel 模块导出登记失败");
        Expect(registry.RegisterExports(kFakeDriverLoaderExports, 1, kModDriverLoader).error == KernelError::kOk, "DriverLoader 模块导出登记失败");
        Expect(registry.Count() == kFakeKernelExportCount + 5, "合并后的导出条数不对");
        const Kernel::RegisteredExport* fs = registry.Find(kIfFsVfs, 1);
        Expect(fs != nullptr && fs->provider == kModVfs, "模块导出的提供者不对");
    }

    // 用例 4：一组导出只要有一条失败整组拒绝，不留下半套
    void CaseRegisterGroupAtomic() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        const ModuleExportEntry mixed[] = {
            { kIfCoreTime, 2, 3, &kFakeCoreTime },
            { kIfCoreMem, 1, 4, &kFakeCoreMem },
        };
        const Kernel::RegistryResult result = registry.RegisterExports(mixed, 2, kModVfs);
        ExpectError(result, KernelError::kAlreadyExists, "组内重复未被拒绝");
        Expect(registry.Count() == kFakeKernelExportCount, "整组拒绝后仍写入了条目");
        Expect(registry.Find(kIfCoreTime, 2) == nullptr, "失败组的第一条被写入");
    }

    // 用例 5：同一组内两条相同 (InterfaceId, Version) 拒绝
    void CaseDuplicateInsideGroup() {
        Kernel::InterfaceRegistry registry;
        const ModuleExportEntry duplicated[] = {
            { kIfCoreTime, 2, 3, &kFakeCoreTime },
            { kIfCoreTime, 2, 3, &kFakeCoreTime },
        };
        ExpectError(registry.RegisterExports(duplicated, 2, kModVfs), KernelError::kAlreadyExists, "组内重复未被拒绝");
        Expect(registry.Count() == 0, "组内重复时写入了条目");
    }

    // 用例 6：区间内取最高版本、上限哨兵与锁定单版本
    void CaseSelectHighestVersion() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        const ModuleExportEntry higher[] = { { kIfCoreTime, 2, 3, &kFakeCoreTime } };
        Expect(registry.RegisterExports(higher, 1, kModVfs).error == KernelError::kOk, "Core.Time v2 登记失败");
        ModuleExportEntry selected{};
        Expect(registry.SelectImport(kIfCoreTime, 1, 2, selected).error == KernelError::kOk, "区间 [1,2] 选择失败");
        Expect(selected.version == 2, "区间 [1,2] 未取最高版本");
        Expect(registry.SelectImport(kIfCoreTime, 1, 1, selected).error == KernelError::kOk, "区间 [1,1] 选择失败");
        Expect(selected.version == 1, "区间 [1,1] 未选到 v1");
        Expect(registry.SelectImport(kIfCoreTime, 2, Kernel::kRegistryUnlimitedVersion, selected).error == KernelError::kOk, "不设上限选择失败");
        Expect(selected.version == 2, "不设上限时未取最高版本");
    }

    // 用例 7：区间内没有可用版本
    void CaseSelectNoSatisfyingVersion() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        ModuleExportEntry selected{};
        ExpectError(registry.SelectImport(kIfCoreTime, 2, 3, selected), KernelError::kNotFound, "区间不满足时未返回 kNotFound");
    }

    // 用例 8：区间参数非法（下限为零、下限大于上限）
    void CaseSelectInvalidRange() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        ModuleExportEntry selected{};
        ExpectError(registry.SelectImport(kIfCoreTime, 0, 1, selected), KernelError::kInvalidArgument, "下限为零未被拒绝");
        ExpectError(registry.SelectImport(kIfCoreTime, 3, 2, selected), KernelError::kInvalidArgument, "下限大于上限未被拒绝");
        ExpectError(registry.SelectImport(InterfaceId{}, 1, 1, selected), KernelError::kInvalidArgument, "全零接口标识未被拒绝");
    }

    // 用例 9：未定义接口（注册表里没有该标识）
    void CaseSelectUndefinedInterface() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        ModuleExportEntry selected{};
        ExpectError(registry.SelectImport(kIfDevModel, 1, 1, selected), KernelError::kNotFound, "未定义接口未被拒绝");
    }

    // 用例 10：使用槽数超过所选最高版本时拒绝，不回退到槽数足够的低版本
    void CaseSlotCountExceedsProvider() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        const ModuleExportEntry pageV2[] = { { kIfCorePage, 2, 3, &kFakeCorePage } };
        Expect(registry.RegisterExports(pageV2, 1, kModVfs).error == KernelError::kOk, "Core.Page v2 登记失败");
        void* target[5] = {};
        ModuleImportEntry import{ kIfCorePage, 1, 2, 4, target };
        ExpectError(registry.ResolveImport(import), KernelError::kInvalidArgument, "槽数超过最高版本未被拒绝");
        import.slotCount = 3;
        Expect(registry.ResolveImport(import).error == KernelError::kOk, "槽数等于提供者槽数时应成功");
    }

    // 用例 11：槽位回填逐槽复制函数指针，越界与空指针拒绝
    void CaseFillSlots() {
        const ModuleExportEntry entry{ kIfCorePage, 1, 5, &kFakeCorePage };
        void* target[5] = {};
        Expect(Kernel::InterfaceRegistry::FillSlots(entry, target, 5).error == KernelError::kOk, "槽位回填失败");
        Expect(target[0] == reinterpret_cast<void*>(&FakePageBytes), "槽 0 回填指针不对");
        Expect(target[1] == reinterpret_cast<void*>(&FakeAllocate), "槽 1 回填指针不对");
        Expect(target[2] == reinterpret_cast<void*>(&FakeFree), "槽 2 回填指针不对");
        Expect(target[3] == reinterpret_cast<void*>(&FakeFreePages), "槽 3 回填指针不对");
        Expect(target[4] == reinterpret_cast<void*>(&FakeManagedLimit), "槽 4 回填指针不对");
        ExpectError(Kernel::InterfaceRegistry::FillSlots(entry, target, 6), KernelError::kInvalidArgument, "回填槽数超界未被拒绝");
        ExpectError(Kernel::InterfaceRegistry::FillSlots(entry, nullptr, 1), KernelError::kInvalidArgument, "回填目标为空未被拒绝");
    }

    // 用例 12：一站式导入解析（选择 + 槽数校验 + 回填）
    void CaseResolveImport() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        void* target[3] = {};
        const ModuleImportEntry import{ kIfCorePage, 1, 1, 3, target };
        Expect(registry.ResolveImport(import).error == KernelError::kOk, "导入解析失败");
        Expect(target[0] == reinterpret_cast<void*>(&FakePageBytes), "导入回填槽 0 不对");
        Expect(target[1] == reinterpret_cast<void*>(&FakeAllocate), "导入回填槽 1 不对");
        Expect(target[2] == reinterpret_cast<void*>(&FakeFree), "导入回填槽 2 不对");
    }

    // 用例 13：导入声明的回填区为空、槽数为零与区间非法
    void CaseResolveImportErrors() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        void* target[3] = {};
        ExpectError(registry.ResolveImport(ModuleImportEntry{ kIfCorePage, 1, 1, 3, nullptr }), KernelError::kInvalidArgument, "回填区为空未被拒绝");
        ExpectError(registry.ResolveImport(ModuleImportEntry{ kIfCorePage, 1, 1, 0, target }), KernelError::kInvalidArgument, "槽数为零未被拒绝");
        ExpectError(registry.ResolveImport(ModuleImportEntry{ kIfCorePage, 2, 1, 3, target }), KernelError::kInvalidArgument, "区间倒置未被拒绝");
    }

    // 用例 14：闭包单例的提供者必须与生成头固定表一致
    void CaseClosureProviderMismatch() {
        Kernel::InterfaceRegistry registry;
        ExpectError(registry.RegisterExports(kFakeDevModelExports, 1, kModVfs), KernelError::kInvalidArgument, "闭包单例提供者不符未被拒绝");
        Expect(registry.RegisterExports(kFakeDevModelExports, 1, kModDevModel).error == KernelError::kOk, "闭包单例正确提供者登记失败");
        Expect(registry.RegisterExports(kFakeVfsExports, 3, kModVfs).error == KernelError::kOk, "Vfs 三个闭包单例登记失败");
    }

    // 用例 15：模块导出与内核核心导出冲突时按重复提供者拒绝
    void CaseModuleDuplicateWithCore() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        const ModuleExportEntry conflict[] = { { kIfCorePage, 1, 5, &kFakeCorePage } };
        ExpectError(registry.RegisterExports(conflict, 1, kModVfs), KernelError::kAlreadyExists, "内核核心与模块的合并唯一性未生效");
    }

    // 用例 16：VerifyAbi 核对 Core.Abi 槽 0/2 的实际返回值
    void CaseVerifyAbi() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        Expect(registry.VerifyAbi().error == KernelError::kOk, "Core.Abi 替身应通过核对");
        const CoreAbiSlotsV1* slots = static_cast<const CoreAbiSlotsV1*>(registry.Find(kIfCoreAbi, 1)->entry.slots);
        Expect(slots->Version() == Kernel::InterfaceRegistry::AbiVersion(), "槽 0 返回值与 AbiVersion 不一致");
        Expect(slots->DriveSdkVersion() == Kernel::InterfaceRegistry::DriveSdkVersion(), "槽 2 返回值与 DriveSdkVersion 不一致");
    }

    // 用例 17：Core.Abi 槽 0 或槽 2 返回值与生成头不一致时拒绝
    void CaseVerifyAbiMismatch() {
        Kernel::InterfaceRegistry registry;
        const ModuleExportEntry badVersionEntry[] = { { kIfCoreAbi, 1, 3, &kBadAbiVersionSlots } };
        Expect(registry.RegisterExports(badVersionEntry, 1, kModuleKernelCore).error == KernelError::kOk, "槽 0 错误的 Core.Abi 登记失败");
        ExpectError(registry.VerifyAbi(), KernelError::kVersionMismatch, "槽 0 返回值不一致未被拒绝");
        registry.Reset();
        const ModuleExportEntry badDriveSdkEntry[] = { { kIfCoreAbi, 1, 3, &kBadAbiDriveSdkSlots } };
        Expect(registry.RegisterExports(badDriveSdkEntry, 1, kModuleKernelCore).error == KernelError::kOk, "槽 2 错误的 Core.Abi 登记失败");
        ExpectError(registry.VerifyAbi(), KernelError::kVersionMismatch, "槽 2 返回值不一致未被拒绝");
    }

    // 用例 18：Core.Abi 未登记时核对失败
    void CaseVerifyAbiMissing() {
        Kernel::InterfaceRegistry registry;
        ExpectError(registry.VerifyAbi(), KernelError::kNotFound, "空注册表的 ABI 核对未返回 kNotFound");
        const ModuleExportEntry other[] = { { kIfCoreTime, 1, 3, &kFakeCoreTime } };
        Expect(registry.RegisterExports(other, 1, kModuleKernelCore).error == KernelError::kOk, "Core.Time 登记失败");
        ExpectError(registry.VerifyAbi(), KernelError::kNotFound, "缺 Core.Abi 时的核对未返回 kNotFound");
    }

    // 用例 19：检查点与回滚撤销本模块登记的导出
    void CaseMarkRollback() {
        Kernel::InterfaceRegistry registry;
        RegisterFakeKernelCore(registry);
        const uint32_t mark = registry.Mark();
        Expect(mark == kFakeKernelExportCount, "检查点位置不对");
        Expect(registry.RegisterExports(kFakeVfsExports, 3, kModVfs).error == KernelError::kOk, "Vfs 导出登记失败");
        Expect(registry.Count() == kFakeKernelExportCount + 3, "登记后条数不对");
        registry.Rollback(mark);
        Expect(registry.Count() == kFakeKernelExportCount, "回滚后条数不对");
        Expect(registry.Find(kIfFsVfs, 1) == nullptr, "回滚后仍能查到被撤销的导出");
        Expect(registry.RegisterExports(kFakeVfsExports, 3, kModVfs).error == KernelError::kOk, "回滚后重新登记失败");
    }

    // 用例 20：导出字段非法、空表与零条数
    void CaseInvalidExportFields() {
        Kernel::InterfaceRegistry registry;
        const ModuleExportEntry zeroId[] = { { InterfaceId{}, 1, 1, &kFakeCoreTime } };
        const ModuleExportEntry zeroVersion[] = { { kIfCoreTime, 0, 3, &kFakeCoreTime } };
        const ModuleExportEntry sentinelVersion[] = { { kIfCoreTime, Kernel::kRegistryUnlimitedVersion, 3, &kFakeCoreTime } };
        const ModuleExportEntry zeroSlots[] = { { kIfCoreTime, 2, 0, &kFakeCoreTime } };
        const ModuleExportEntry tooManySlots[] = { { kIfCoreTime, 2, 65, &kFakeCoreTime } };
        const ModuleExportEntry nullSlots[] = { { kIfCoreTime, 2, 3, nullptr } };
        ExpectError(registry.RegisterExports(zeroId, 1, kModVfs), KernelError::kInvalidArgument, "全零接口标识未被拒绝");
        ExpectError(registry.RegisterExports(zeroVersion, 1, kModVfs), KernelError::kInvalidArgument, "版本零未被拒绝");
        ExpectError(registry.RegisterExports(sentinelVersion, 1, kModVfs), KernelError::kInvalidArgument, "版本哨兵未被拒绝");
        ExpectError(registry.RegisterExports(zeroSlots, 1, kModVfs), KernelError::kInvalidArgument, "槽数为零未被拒绝");
        ExpectError(registry.RegisterExports(tooManySlots, 1, kModVfs), KernelError::kInvalidArgument, "槽数超过 64 未被拒绝");
        ExpectError(registry.RegisterExports(nullSlots, 1, kModVfs), KernelError::kInvalidArgument, "空槽表未被拒绝");
        ExpectError(registry.RegisterExports(nullptr, 1, kModVfs), KernelError::kInvalidArgument, "空表指针未被拒绝");
        ExpectError(registry.RegisterExports(zeroId, 0, kModVfs), KernelError::kInvalidArgument, "零条数未被拒绝");
        Expect(registry.Count() == 0, "非法登记写入了条目");
    }

    // 用例 21：容量上限拒绝超出登记
    void CaseCapacity() {
        Kernel::InterfaceRegistry registry;
        ModuleExportEntry bulk[Kernel::kRegistryMaxExports];
        for (uint32_t index = 0; index < Kernel::kRegistryMaxExports; ++index) bulk[index] = ModuleExportEntry{ kIfCoreMem, static_cast<uint16_t>(index + 1), 4, &kFakeCoreMem };
        Expect(registry.RegisterExports(bulk, static_cast<uint16_t>(Kernel::kRegistryMaxExports), kModVfs).error == KernelError::kOk, "填满容量失败");
        Expect(registry.Count() == Kernel::kRegistryMaxExports, "填满后条数不对");
        const ModuleExportEntry overflow[] = { { kIfCoreMem, static_cast<uint16_t>(Kernel::kRegistryMaxExports + 1), 4, &kFakeCoreMem } };
        ExpectError(registry.RegisterExports(overflow, 1, kModVfs), KernelError::kOutOfResources, "超出容量未被拒绝");
    }

    // 打印用法
    void Usage() { std::cout << "CheckRegistry — 接口注册表的宿主侧检查\n\n用法：CheckRegistry [-h|--help]\n退出码：0 全部用例通过；1 存在失败用例或参数错误\n"; }

    // 跑全部固定用例：数量与声明不符按内部错误处理
    int RunCases() {
        Case("登记：内核核心 10 个 Core.* 导出", CaseRegisterKernelCore);
        Case("提供者唯一：重复 (InterfaceId, Version) 拒绝且不覆盖", CaseRegisterDuplicateProvider);
        Case("登记：模块导出与内核核心合并", CaseRegisterModuleExports);
        Case("登记：整组原子，失败不留半套", CaseRegisterGroupAtomic);
        Case("登记：组内重复 (InterfaceId, Version)", CaseDuplicateInsideGroup);
        Case("版本选择：区间内取最高版本与上限哨兵", CaseSelectHighestVersion);
        Case("版本选择：区间不满足", CaseSelectNoSatisfyingVersion);
        Case("版本选择：区间非法", CaseSelectInvalidRange);
        Case("版本选择：未定义接口", CaseSelectUndefinedInterface);
        Case("槽位：使用槽数超过最高版本时拒绝且不回退", CaseSlotCountExceedsProvider);
        Case("槽位：回填逐槽指针与边界", CaseFillSlots);
        Case("槽位：一站式导入解析", CaseResolveImport);
        Case("槽位：导入声明非法字段", CaseResolveImportErrors);
        Case("提供者：闭包单例固定提供者", CaseClosureProviderMismatch);
        Case("提供者：模块与内核核心合并唯一", CaseModuleDuplicateWithCore);
        Case("ABI：VerifyAbi 核对 Core.Abi 槽 0/2", CaseVerifyAbi);
        Case("ABI：Core.Abi 返回值与生成头不一致", CaseVerifyAbiMismatch);
        Case("ABI：Core.Abi 未登记", CaseVerifyAbiMissing);
        Case("生命周期：Mark 与 Rollback", CaseMarkRollback);
        Case("登记：非法字段与空表", CaseInvalidExportFields);
        Case("登记：容量上限", CaseCapacity);
        if (g_ran != kCaseCount) Fail("内部用例数量不符：实跑 " + std::to_string(g_ran) + "，声明 " + std::to_string(kCaseCount));
        const unsigned passed = g_ran - g_failed;
        std::cout << "CheckRegistry: " << passed << "/" << g_ran << " cases passed" << std::endl;
        return g_failed == 0 ? 0 : 1;
    }
}

// 入口：只支持帮助选项，其余参数按用法错误处理
int main(int argc, char** argv) {
    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "-h" || argument == "--help") {
                Usage();
                return 0;
            }
            throw std::runtime_error("无法识别的参数：" + argument);
        }
        return RunCases();
    } catch (const std::exception& error) {
        std::cerr << "CheckRegistry: " << error.what() << '\n';
        return 1;
    }
}
