/* InterfaceRegistry.cpp
    内核接口注册表实现：导出登记的原子校验、最高版本选择与槽位回填

    登记先全组校验再写入：组内重复、与已登记表重复、字段非法或闭包单例提供者不符时
    整组拒绝，注册表保持调用前状态：提供者唯一，不允许半套登记
    版本选择：先取区间内最高版本，再在 ResolveImport 里检查使用槽数不超过所选版本的
    槽数，超过即拒绝，不回退到低版本
    槽位回填：ModuleExportEntry.slots 是槽位表的首地址，槽位表是函数指针数组的
    强类型视图，回填按 void* 指针逐槽复制（导出/回填/容器槽位表的唯一布局来源）
*/

#include <InterfaceRegistry.hpp>

namespace Kernel {
    namespace {
        // 成功结果：错误码为 kOk，原因为空
        RegistryResult Success() { return RegistryResult{ KernelError::kOk, nullptr }; }

        // 失败结果：固定英文原因，不带诊断编号与路径
        RegistryResult Failure(KernelError error, const char* reason) { return RegistryResult{ error, reason }; }

        // 判断两条导出是否指向同一 (InterfaceId, Version)
        bool SameInterfaceVersion(const ModuleExportEntry& left, const ModuleExportEntry& right) { return left.id == right.id && left.version == right.version; }

        // 查闭包单例的固定提供者：接口是闭包单例时返回期望的模块标识，否则返回空
        const ModuleId* ClosureProvider(const InterfaceId& id) {
            for (const ClosureProviderEntry& provider : kClosureProviders) if (provider.id == id) return &provider.module;
            return nullptr;
        }

        // 导出条目的字段范围：版本取值、槽数范围与全零标识非法
        bool ValidExport(const ModuleExportEntry& entry) {
            if (IsZero(entry.id)) return false;
            if (entry.version == 0 || entry.version == kRegistryUnlimitedVersion) return false;
            if (entry.slotCount == 0 || entry.slotCount > kRegistryMaxSlots) return false;
            return entry.slots != nullptr;
        }
    }

    void InterfaceRegistry::Reset() { m_count = 0; }

    RegistryResult InterfaceRegistry::RegisterExports(const ModuleExportEntry* entries, uint16_t count, const ModuleId& provider) {
        if (entries == nullptr) return Failure(KernelError::kInvalidArgument, "export table is null");
        if (count == 0) return Failure(KernelError::kInvalidArgument, "export table is empty");
        if (count > kRegistryMaxExports - m_count) return Failure(KernelError::kOutOfResources, "interface registry is full");
        for (uint16_t index = 0; index < count; ++index) {
            const ModuleExportEntry& entry = entries[index];
            if (!ValidExport(entry)) return Failure(KernelError::kInvalidArgument, "export entry is out of range");
            for (uint16_t previous = 0; previous < index; ++previous) {
                if (SameInterfaceVersion(entries[previous], entry)) return Failure(KernelError::kAlreadyExists, "export table contains a duplicate provider");
            }
            if (Find(entry.id, entry.version) != nullptr) return Failure(KernelError::kAlreadyExists, "duplicate provider for the interface version");
            const ModuleId* expected = ClosureProvider(entry.id);
            if (expected != nullptr && !(provider == *expected)) return Failure(KernelError::kInvalidArgument, "closure singleton provider doesn't match the contract table");
        }
        for (uint16_t index = 0; index < count; ++index) {
            m_exports[m_count].entry = entries[index];
            m_exports[m_count].provider = provider;
            ++m_count;
        }
        return Success();
    }

    RegistryResult InterfaceRegistry::SelectImport(const InterfaceId& id, uint16_t minVersion, uint16_t maxVersion, ModuleExportEntry& selected) const {
        if (IsZero(id)) return Failure(KernelError::kInvalidArgument, "import interface id is zero");
        if (minVersion == 0 || minVersion > maxVersion) return Failure(KernelError::kInvalidArgument, "import version range is invalid");
        const RegisteredExport* best = nullptr;
        for (uint32_t index = 0; index < m_count; ++index) {
            const RegisteredExport& candidate = m_exports[index];
            if (!(candidate.entry.id == id)) continue;
            if (candidate.entry.version < minVersion || candidate.entry.version > maxVersion) continue;
            if (best == nullptr || candidate.entry.version > best->entry.version) best = &candidate;
        }
        if (best == nullptr) return Failure(KernelError::kNotFound, "no provider version satisfies the import range");
        selected = best->entry;
        return Success();
    }

    RegistryResult InterfaceRegistry::FillSlots(const ModuleExportEntry& selected, void** targetSlots, uint16_t slotCount) {
        if (targetSlots == nullptr) return Failure(KernelError::kInvalidArgument, "target slot table is null");
        if (selected.slots == nullptr) return Failure(KernelError::kInvalidArgument, "provider slot table is null");
        if (slotCount == 0 || slotCount > selected.slotCount) return Failure(KernelError::kInvalidArgument, "slot count exceeds the provider");
        const void* const* source = reinterpret_cast<const void* const*>(selected.slots);
        for (uint16_t index = 0; index < slotCount; ++index) targetSlots[index] = const_cast<void*>(source[index]);
        return Success();
    }

    RegistryResult InterfaceRegistry::ResolveImport(const ModuleImportEntry& import) const {
        if (import.slots == nullptr) return Failure(KernelError::kInvalidArgument, "import slot table is null");
        if (import.slotCount == 0 || import.slotCount > kRegistryMaxSlots) return Failure(KernelError::kInvalidArgument, "import slot count is out of range");
        ModuleExportEntry selected{};
        const RegistryResult selection = SelectImport(import.id, import.minVersion, import.maxVersion, selected);
        if (selection.error != KernelError::kOk) return selection;
        if (selected.slotCount < import.slotCount) return Failure(KernelError::kInvalidArgument, "slot count exceeds the provider");
        return FillSlots(selected, static_cast<void**>(import.slots), import.slotCount);
    }

    RegistryResult InterfaceRegistry::VerifyAbi() const {
        const RegisteredExport* abi = Find(kIfCoreAbi, 1);
        if (abi == nullptr) return Failure(KernelError::kNotFound, "Core.Abi export isn't registered");
        if (abi->entry.slotCount < kCoreAbiSlotCountV1) return Failure(KernelError::kVersionMismatch, "Core.Abi has fewer slots than the contract block");
        const CoreAbiSlotsV1* slots = static_cast<const CoreAbiSlotsV1*>(abi->entry.slots);
        if (slots->Version == nullptr || slots->DriveSdkVersion == nullptr) return Failure(KernelError::kInvalidArgument, "Core.Abi slot implementation is null");
        if (slots->Version() != kKernelAbiVersion) return Failure(KernelError::kVersionMismatch, "Core.Abi slot 0 doesn't return kKernelAbiVersion");
        if (slots->DriveSdkVersion() != kDriveSdkVersion) return Failure(KernelError::kVersionMismatch, "Core.Abi slot 2 doesn't return kDriveSdkVersion");
        return Success();
    }

    uint32_t InterfaceRegistry::Mark() const { return m_count; }

    void InterfaceRegistry::Rollback(uint32_t mark) {
        if (mark < m_count) m_count = mark;
    }

    uint32_t InterfaceRegistry::Count() const { return m_count; }

    const RegisteredExport* InterfaceRegistry::Find(const InterfaceId& id, uint16_t version) const {
        for (uint32_t index = 0; index < m_count; ++index) {
            const ModuleExportEntry& entry = m_exports[index].entry;
            if (entry.id == id && entry.version == version) return &m_exports[index];
        }
        return nullptr;
    }
}
