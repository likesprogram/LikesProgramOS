/* BlockRegistry.cpp
    Block.Registry v1 的实现：设备登记、句柄查询、引用计数与偏移视图子设备

    根设备保存登记时的 ops 与 context，子设备保存父句柄与字节区间；
    子设备与根设备共用 DeviceHandle 命名空间，
    Acquire 返回可直接发 I/O 的 ops 表与实例 context
    子设备的对齐与越界判定与 Storage::SubDevice 同源（起点按父设备逻辑块对齐、长度非零、
    区间不越界、64 位加法不回绕），子设备的块大小 / 可写 / 刷新原语能力随父设备；
    本层不识别分区、不验证区间连续性、不做缓存与重试
*/

#include <ModuleAbi.hpp>

#include <VfsCore.hpp>

namespace {
    // 一个块设备登记项：根设备保存原始 ops/context，子设备保存父句柄与区间
    struct DeviceEntry {
        bool used;                  // 是否占用
        bool root;                  // 是否根设备（false 为子设备）
        DeviceHandle handle;        // 本设备句柄
        const BlockDeviceOps* ops;  // 根设备的原始 ops；子设备为空
        void* context;              // 根设备的实例上下文；子设备为空
        DeviceHandle parent;        // 子设备的父句柄；根设备为 0
        uint64_t begin;             // 子设备区间起点，按父设备逻辑块对齐
        uint64_t bytes;             // 子设备区间长度
        uint32_t refCount;          // 引用计数
    };

    // 设备登记表：固定上界，耗尽返回 NoSpace
    DeviceEntry g_devices[VfsModule::kMaxDevices];
    // 子设备句柄分配游标：从高位段递增，跳过已占用值
    DeviceHandle g_nextChildHandle = VfsModule::kChildHandleBase;

    // 查找登记表下标；未登记返回 kMaxDevices
    uint32_t FindIndex(DeviceHandle handle) {
        if (handle == 0) return VfsModule::kMaxDevices;
        for (uint32_t index = 0; index < VfsModule::kMaxDevices; ++index) {
            if (g_devices[index].used && g_devices[index].handle == handle) return index;
        }
        return VfsModule::kMaxDevices;
    }

    // 取登记项；未登记返回空
    DeviceEntry* FindEntry(DeviceHandle handle) {
        const uint32_t index = FindIndex(handle);
        return index < VfsModule::kMaxDevices ? &g_devices[index] : nullptr;
    }

    // 分配一个空闲登记项；表满返回空
    DeviceEntry* AllocateEntry() {
        for (uint32_t index = 0; index < VfsModule::kMaxDevices; ++index) {
            if (!g_devices[index].used) return &g_devices[index];
        }
        return nullptr;
    }

    // 逻辑块大小是否合法：2 的幂且不小于 512
    bool ValidBlockBytes(uint32_t blockBytes) { return blockBytes >= 512 && (blockBytes & (blockBytes - 1)) == 0; }

    // 沿父链找到根登记项；根设备的 ops 必须非空
    const DeviceEntry* RootOf(const DeviceEntry& entry) {
        const DeviceEntry* current = &entry;
        for (uint32_t depth = 0; depth <= VfsModule::kMaxDevices; ++depth) {
            if (current->root) return current;
            current = FindEntry(current->parent);
            if (current == nullptr) return nullptr;
        }
        return nullptr;
    }

    // 设备视图的逻辑块大小；根链路不完整返回 0
    uint32_t BlockBytesOf(const DeviceEntry& entry) {
        const DeviceEntry* const root = RootOf(entry);
        if (root == nullptr || root->ops == nullptr || root->ops->BlockBytes == nullptr) return 0;
        return root->ops->BlockBytes(root->context);
    }

    // 设备视图的容量：根设备取原始 ops，子设备取区间长度
    uint64_t CapacityOf(const DeviceEntry& entry) {
        if (!entry.root) return entry.bytes;
        const DeviceEntry* const root = RootOf(entry);
        if (root == nullptr || root->ops == nullptr || root->ops->CapacityBytes == nullptr) return 0;
        return root->ops->CapacityBytes(root->context);
    }

    // 子设备区间检查：对齐、零长度、越界与回绕；成功返回 Ok
    Storage::Error ChildRange(const DeviceEntry& entry, uint64_t offset, uint32_t bytes) {
        const uint32_t blockBytes = BlockBytesOf(entry);
        if (!ValidBlockBytes(blockBytes)) return Storage::Error::InvalidArgument;
        if (bytes == 0) return Storage::Error::InvalidArgument;
        if ((offset % blockBytes) != 0) return Storage::Error::InvalidArgument;
        if ((bytes % blockBytes) != 0) return Storage::Error::InvalidArgument;
        if (offset > entry.bytes) return Storage::Error::InvalidArgument;
        if (bytes > entry.bytes - offset) return Storage::Error::InvalidArgument;
        return Storage::Error::Ok;
    }

    // 子设备回调上下文：登记项指针（登记表为静态数组，地址稳定）
    Storage::Error ChildRead(void* context, uint64_t offset, void* buffer, uint32_t bytes) {
        const DeviceEntry& entry = *static_cast<DeviceEntry*>(context);
        if (buffer == nullptr) return Storage::Error::InvalidArgument;
        if (ChildRange(entry, offset, bytes) != Storage::Error::Ok) return Storage::Error::InvalidArgument;
        const DeviceEntry* const root = RootOf(entry);
        if (root == nullptr || root->ops == nullptr || root->ops->Read == nullptr) return Storage::Error::NotSupported;
        return root->ops->Read(root->context, entry.begin + offset, buffer, bytes);
    }

    // 子设备写：对齐与范围检查后转发根设备；只读设备由根设备返回 ReadOnly
    Storage::Error ChildWrite(void* context, uint64_t offset, const void* buffer, uint32_t bytes) {
        const DeviceEntry& entry = *static_cast<DeviceEntry*>(context);
        if (buffer == nullptr) return Storage::Error::InvalidArgument;
        if (ChildRange(entry, offset, bytes) != Storage::Error::Ok) return Storage::Error::InvalidArgument;
        const DeviceEntry* const root = RootOf(entry);
        if (root == nullptr || root->ops == nullptr || root->ops->Write == nullptr) return Storage::Error::NotSupported;
        return root->ops->Write(root->context, entry.begin + offset, buffer, bytes);
    }

    // 子设备块大小：随根设备
    uint32_t ChildBlockBytes(void* context) { return BlockBytesOf(*static_cast<DeviceEntry*>(context)); }

    // 子设备容量：区间长度
    uint64_t ChildCapacityBytes(void* context) { return static_cast<DeviceEntry*>(context)->bytes; }

    // 子设备可写：随根设备
    uint32_t ChildWritable(void* context) {
        const DeviceEntry* const root = RootOf(*static_cast<DeviceEntry*>(context));
        if (root == nullptr || root->ops == nullptr || root->ops->Writable == nullptr) return 0;
        return root->ops->Writable(root->context);
    }

    // 子设备刷新：根设备没有刷新原语时按无缓存返回 Ok，不调用空回调
    Storage::Error ChildFlush(void* context) {
        const DeviceEntry* const root = RootOf(*static_cast<DeviceEntry*>(context));
        if (root == nullptr || root->ops == nullptr) return Storage::Error::InvalidArgument;
        if ((root->ops->capabilities & kBlockCapabilityFlushPrimitive) == 0) return Storage::Error::Ok;
        if (root->ops->Flush == nullptr) return Storage::Error::Ok;
        return root->ops->Flush(root->context);
    }

    // 子设备调用表：capabilities.bit0 表达根设备是否有刷新原语
    const BlockDeviceOps kChildOpsFlush = {
        1, &ChildBlockBytes, &ChildCapacityBytes, &ChildWritable, &ChildRead, &ChildWrite, &ChildFlush};
    const BlockDeviceOps kChildOpsNoFlush = {
        0, &ChildBlockBytes, &ChildCapacityBytes, &ChildWritable, &ChildRead, &ChildWrite, nullptr};
}

namespace VfsModule {
    void ResetBlockRegistry() {
        for (uint32_t index = 0; index < kMaxDevices; ++index) g_devices[index] = DeviceEntry{};
        g_nextChildHandle = kChildHandleBase;
    }

    Storage::Error Publish(DeviceHandle handle, const BlockDeviceOps* ops, void* context, uint32_t flags) {
        if (flags != 0) return Storage::Error::InvalidArgument;
        if (handle == 0 || ops == nullptr) return Storage::Error::InvalidArgument;
        if (FindIndex(handle) < kMaxDevices) return Storage::Error::InvalidArgument;
        if (ops->BlockBytes == nullptr || ops->CapacityBytes == nullptr || ops->Writable == nullptr
            || ops->Read == nullptr || ops->Write == nullptr) {
            return Storage::Error::InvalidArgument;
        }
        if ((ops->capabilities & kBlockCapabilityFlushPrimitive) != 0 && ops->Flush == nullptr) {
            return Storage::Error::InvalidArgument;
        }
        if (!ValidBlockBytes(ops->BlockBytes(context))) return Storage::Error::InvalidArgument;
        DeviceEntry* const entry = AllocateEntry();
        if (entry == nullptr) return Storage::Error::NoSpace;
        entry->used = true;
        entry->root = true;
        entry->handle = handle;
        entry->ops = ops;
        entry->context = context;
        entry->parent = 0;
        entry->begin = 0;
        entry->bytes = 0;
        entry->refCount = 0;
        return Storage::Error::Ok;
    }

    Storage::Error CreateSubDevice(DeviceHandle parent, uint64_t begin, uint64_t bytes, uint32_t flags, DeviceHandle* child) {
        if (flags != 0 || child == nullptr) return Storage::Error::InvalidArgument;
        const DeviceEntry* const parentEntry = FindEntry(parent);
        if (parentEntry == nullptr) return Storage::Error::NotFound;
        const uint32_t blockBytes = BlockBytesOf(*parentEntry);
        if (!ValidBlockBytes(blockBytes)) return Storage::Error::InvalidArgument;
        if ((begin % blockBytes) != 0) return Storage::Error::InvalidArgument;
        if (bytes == 0) return Storage::Error::InvalidArgument;
        const uint64_t capacity = CapacityOf(*parentEntry);
        if (begin >= capacity) return Storage::Error::InvalidArgument;
        if (bytes > capacity - begin) return Storage::Error::InvalidArgument;
        // 子设备句柄从高位段分配；句柄空间耗尽返回 NoSpace
        DeviceHandle handle = 0;
        while (g_nextChildHandle != 0) {
            const DeviceHandle candidate = g_nextChildHandle++;
            if (FindIndex(candidate) < kMaxDevices) continue;
            handle = candidate;
            break;
        }
        if (handle == 0) return Storage::Error::NoSpace;
        DeviceEntry* const entry = AllocateEntry();
        if (entry == nullptr) return Storage::Error::NoSpace;
        entry->used = true;
        entry->root = false;
        entry->handle = handle;
        entry->ops = nullptr;
        entry->context = nullptr;
        entry->parent = parent;
        entry->begin = begin;
        entry->bytes = bytes;
        entry->refCount = 0;
        *child = handle;
        return Storage::Error::Ok;
    }

    Storage::Error Acquire(DeviceHandle handle, const BlockDeviceOps** ops, void** context) {
        if (ops == nullptr || context == nullptr) return Storage::Error::InvalidArgument;
        DeviceEntry* const entry = FindEntry(handle);
        if (entry == nullptr) return Storage::Error::NotFound;
        ++entry->refCount;
        if (entry->root) {
            *ops = entry->ops;
            *context = entry->context;
            return Storage::Error::Ok;
        }
        const DeviceEntry* const root = RootOf(*entry);
        const bool hasFlush = root != nullptr && root->ops != nullptr
            && (root->ops->capabilities & kBlockCapabilityFlushPrimitive) != 0;
        *ops = hasFlush ? &kChildOpsFlush : &kChildOpsNoFlush;
        *context = entry;
        return Storage::Error::Ok;
    }

    Storage::Error Release(DeviceHandle handle) {
        DeviceEntry* const entry = FindEntry(handle);
        if (entry == nullptr) return Storage::Error::NotFound;
        if (entry->refCount == 0) return Storage::Error::InvalidArgument;
        --entry->refCount;
        return Storage::Error::Ok;
    }

    bool DeviceExists(DeviceHandle handle) { return FindIndex(handle) < kMaxDevices; }

    uint32_t ReferenceCount(DeviceHandle handle) {
        const DeviceEntry* const entry = FindEntry(handle);
        return entry != nullptr ? entry->refCount : 0;
    }

    uint32_t RootDevices(DeviceHandle* handles, uint32_t maxHandles) {
        if (handles == nullptr || maxHandles == 0) return 0;
        uint32_t count = 0;
        for (uint32_t index = 0; index < kMaxDevices && count < maxHandles; ++index) {
            if (!g_devices[index].used || !g_devices[index].root) continue;
            handles[count++] = g_devices[index].handle;
        }
        return count;
    }

    KernelError RegisterDevicesFromBindings() {
        if (_Contract_Import_DevModel_v1.Enumerate == nullptr || _Contract_Import_DevModel_v1.Binding == nullptr) {
            return KernelError::kNotSupported;
        }
        uint32_t cursor = 0;
        for (;;) {
            DeviceFacts deviceFacts{};
            deviceFacts.bytes = static_cast<uint32_t>(sizeof(DeviceFacts));
            const KernelError enumerated = _Contract_Import_DevModel_v1.Enumerate(&cursor, &deviceFacts);
            if (enumerated == KernelError::kNotFound) break;
            if (enumerated != KernelError::kOk) return KernelError::kCorrupt;
            // 查询模式：Dev.Model 约定 bytes == 0 才是查询，非零 bytes 走其它分支不返回事实
            BindingFacts binding{};
            const KernelError status = _Contract_Import_DevModel_v1.Binding(deviceFacts.deviceHandle, &binding);
            // 未绑定的设备跳过：驱动可能尚未 Start，不把它当作失败
            if (status == KernelError::kNotFound) continue;
            if (status != KernelError::kOk) return KernelError::kCorrupt;
            // kOk 不代表拿到了事实：查询模式必须回填长度、ops 与 context，缺一即视为内核状态损坏
            if (binding.bytes < sizeof(BindingFacts) || binding.state != kBindingStateStarted || binding.ops == nullptr) return KernelError::kCorrupt;
            // 已登记的设备跳过：Init 可重入，重复拉取不覆盖先登记的 ops
            if (DeviceExists(deviceFacts.deviceHandle)) continue;
            // 唯一调用点：本模块内部的块设备层按 Dev.Model 绑定事实登记
            const Storage::Error published = Publish(deviceFacts.deviceHandle, binding.ops, binding.context, 0);
            if (published != Storage::Error::Ok) return KernelError::kCorrupt;
        }
        return KernelError::kOk;
    }
}
