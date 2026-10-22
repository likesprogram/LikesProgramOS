/* DriverRegistry.cpp
    Drv.Loader 四槽实现：驱动登记、通配匹配、绑定 / 启动 / 停止与绑定记录同步

    绑定前先经 Dev.Model.Claim 认领设备唯一控制权，绑定与启动分别同步一条 BindingFacts：
    Bound 时 ops / context 为空，Start 成功后写回驱动的 ops 与实例 context，Stop 时清除；
    任何一步失败都按同一顺序回收：清除绑定事实 → 归还认领 → 释放绑定记录
    Start 失败按「失败回收路径也走 Stop」的口径回滚，返回映射后的 KernelError；
    Stop 回调无返回值，调用后无论绑定状态如何都完成回收
    本文件不打印、不停机：只经注入的 Core.Log 行写槽输出固定英文标记
*/

#include "DriverRegistry.hpp"

#include <Abi.hpp>
#include <Libc.hpp>

namespace {
    // 驱动描述是否同一份：注册与绑定都按字段比较，不依赖指针地址
    bool SameDriver(const DriverDesc& left, const DriverDesc& right) {
        return left.driveSdkVersion == right.driveSdkVersion && left.opsKind == right.opsKind
            && left.busKind == right.busKind && left.classCode == right.classCode
            && left.vendorId == right.vendorId && left.deviceId == right.deviceId
            && left.flags == right.flags && left.start == right.start && left.stop == right.stop
            && left.ops == right.ops && left.context == right.context;
    }

    // 驱动描述与设备事实是否匹配：通配字段不参与比较
    bool Matches(const DriverDesc& description, const DeviceFacts& facts) {
        if (description.busKind != DrvLoader::kBusKindAny && description.busKind != facts.busKind) return false;
        if (description.classCode != DrvLoader::kClassCodeAny && description.classCode != facts.classCode) return false;
        if (description.vendorId != DrvLoader::kVendorIdAny && description.vendorId != facts.vendorId) return false;
        if (description.deviceId != DrvLoader::kDeviceIdAny && description.deviceId != facts.deviceId) return false;
        return true;
    }

    // 驱动描述字段范围核对：通配值或落在字段宽度内
    bool ValidDescriptionFields(const DriverDesc& description) {
        if (description.classCode != DrvLoader::kClassCodeAny && description.classCode > 0xFFFFFFu) return false;
        if (description.vendorId != DrvLoader::kVendorIdAny && description.vendorId > 0xFFFFu) return false;
        if (description.deviceId != DrvLoader::kDeviceIdAny && description.deviceId > 0xFFFFu) return false;
        return true;
    }

    // 写一行诊断：tag 用 INFO / WARN，文本是固定英文标记
    void Log(const DrvLoader::State& state, const char* tag, const char* text) {
        if (state.logLine != nullptr) state.logLine(state.context, tag, text);
    }

    // 按绑定句柄找记录下标；找不到返回 kMaxBindings
    uint32_t FindBinding(const DrvLoader::State& state, uint32_t bindHandle) {
        for (uint32_t index = 0; index < DrvLoader::kMaxBindings; ++index) {
            const DrvLoader::BindingRecord& binding = state.bindings[index];
            if (binding.used != 0 && binding.bindHandle == bindHandle) return index;
        }
        return DrvLoader::kMaxBindings;
    }

    // 按驱动描述找已登记记录下标；找不到返回 kMaxDrivers
    uint32_t FindDriver(const DrvLoader::State& state, const DriverDesc* description) {
        for (uint32_t index = 0; index < DrvLoader::kMaxDrivers; ++index) {
            const DrvLoader::DriverRecord& driver = state.drivers[index];
            if (driver.used != 0 && SameDriver(driver.desc, *description)) return index;
        }
        return DrvLoader::kMaxDrivers;
    }

    // 按设备句柄从 Dev.Model 取事实：句柄不存在返回 kNotFound
    KernelError LookupDevice(const DrvLoader::State& state, uint32_t deviceHandle, DeviceFacts& facts) {
        uint32_t cursor = 0;
        for (uint32_t step = 0; step < 256; ++step) {
            facts = DeviceFacts{};
            const KernelError error = state.devModel->Enumerate(&cursor, &facts);
            if (error == KernelError::kNotFound) return KernelError::kNotFound;
            if (error != KernelError::kOk) return error;
            if (facts.deviceHandle == deviceHandle) return KernelError::kOk;
        }
        return KernelError::kCorrupt;
    }

    // 把绑定记录同步给 DevModel：state 非零是登记 / 更新，零是清除
    KernelError SyncBinding(DrvLoader::State& state, const DrvLoader::BindingRecord& binding,
        const DriverDesc& description, uint32_t bindingState) {
        BindingFacts facts{};
        facts.bytes = sizeof(BindingFacts);
        facts.bindHandle = binding.bindHandle;
        facts.deviceHandle = binding.deviceHandle;
        facts.driverModule = binding.driverModule;
        facts.state = bindingState;
        if (bindingState == DrvLoader::kBindingStateStarted) {
            facts.ops = static_cast<const BlockDeviceOps*>(description.ops);
            facts.context = description.context;
        }
        return state.devModel->Binding(binding.deviceHandle, &facts);
    }

    // 清除一条绑定的 DevModel 事实并归还认领，然后释放记录
    void DropBinding(DrvLoader::State& state, uint32_t index) {
        DrvLoader::BindingRecord& binding = state.bindings[index];
        BindingFacts facts{};
        facts.bytes = sizeof(BindingFacts);
        facts.bindHandle = binding.bindHandle;
        facts.deviceHandle = binding.deviceHandle;
        facts.state = 0;
        state.devModel->Binding(binding.deviceHandle, &facts);
        state.devModel->Release(binding.deviceHandle);
        if (binding.used != 0) --state.bindingCount;
        binding = DrvLoader::BindingRecord{};
    }

    // 分配一个绑定句柄：从 1 起递增，跳过 0
    uint32_t NextBindHandle(DrvLoader::State& state) {
        for (;;) {
            const uint32_t handle = state.nextBindHandle;
            state.nextBindHandle = (handle == 0xFFFFFFFFu) ? 1u : handle + 1u;
            if (handle != 0) return handle;
        }
    }
}

namespace DrvLoader {
    KernelError Init(State& state, const DevModelSlotsV1* devModel,
        void (*logLine)(void* context, const char* tag, const char* text), void* context) {
        if (devModel == nullptr || logLine == nullptr) return KernelError::kInvalidArgument;
        if (devModel->Enumerate == nullptr || devModel->Release == nullptr || devModel->Binding == nullptr
            || devModel->Claim == nullptr) {
            return KernelError::kCorrupt;
        }
        for (uint32_t index = 0; index < kMaxDrivers; ++index) state.drivers[index] = DriverRecord{};
        for (uint32_t index = 0; index < kMaxBindings; ++index) state.bindings[index] = BindingRecord{};
        state.driverCount = 0;
        state.bindingCount = 0;
        state.nextBindHandle = 1;
        state.externalAttempts = 0;
        state.devModel = devModel;
        state.logLine = logLine;
        state.context = context;
        Log(state, "INFO", kMarkerReady);
        return KernelError::kOk;
    }

    void Abort(State& state) {
        for (uint32_t index = 0; index < kMaxBindings; ++index) {
            if (state.bindings[index].used == 0) continue;
            const BindingRecord binding = state.bindings[index];
            if (binding.state == kBindingStateStarted) {
                const DriverRecord& driver = state.drivers[binding.driverIndex];
                if (driver.desc.stop != nullptr) driver.desc.stop(driver.desc.context);
            }
            DropBinding(state, index);
        }
        for (uint32_t index = 0; index < kMaxDrivers; ++index) state.drivers[index] = DriverRecord{};
        state.driverCount = 0;
        state.bindingCount = 0;
        state.devModel = nullptr;
        state.logLine = nullptr;
        state.context = nullptr;
    }

    KernelError RegisterDriver(State& state, const DriverDesc* description) {
        if (description == nullptr) return KernelError::kInvalidArgument;
        if (state.devModel == nullptr) return KernelError::kState;
        if (description->bytes < sizeof(DriverDesc)) return KernelError::kInvalidArgument;
        if (description->driveSdkVersion != kDriveSdkVersion) return KernelError::kVersionMismatch;
        if (description->opsKind != kOpsKindBlockDevice) return KernelError::kNotSupported;
        if (description->flags != 0) return KernelError::kInvalidArgument;
        if (description->start == nullptr || description->stop == nullptr || description->ops == nullptr) {
            return KernelError::kInvalidArgument;
        }
        if (!ValidDescriptionFields(*description)) return KernelError::kInvalidArgument;
        if (FindDriver(state, description) != kMaxDrivers) return KernelError::kAlreadyExists;
        if (state.driverCount >= kMaxDrivers) return KernelError::kOutOfResources;
        for (uint32_t index = 0; index < kMaxDrivers; ++index) {
            if (state.drivers[index].used != 0) continue;
            state.drivers[index].desc = *description;
            state.drivers[index].used = 1;
            ++state.driverCount;
            return KernelError::kOk;
        }
        return KernelError::kOutOfResources;
    }

    KernelError Bind(State& state, uint32_t deviceHandle, const DriverDesc* description, uint32_t* bindHandle) {
        if (description == nullptr || bindHandle == nullptr) return KernelError::kInvalidArgument;
        if (state.devModel == nullptr) return KernelError::kState;
        const uint32_t driverIndex = FindDriver(state, description);
        if (driverIndex == kMaxDrivers) return KernelError::kNotFound;
        DeviceFacts facts{};
        if (const KernelError error = LookupDevice(state, deviceHandle, facts); error != KernelError::kOk) return error;
        if (!Matches(state.drivers[driverIndex].desc, facts)) return KernelError::kNotFound;
        // 一格设备同一时刻只允许一条绑定
        for (uint32_t index = 0; index < kMaxBindings; ++index) {
            if (state.bindings[index].used != 0 && state.bindings[index].deviceHandle == deviceHandle) {
                return KernelError::kBusy;
            }
        }
        uint32_t claimHandle = 0;
        const KernelError claimError = state.devModel->Claim(deviceHandle, kClaimFlagsNone, &claimHandle);
        if (claimError != KernelError::kOk) return claimError;
        // 找空绑定槽：原顺序稳定，句柄递增
        uint32_t slot = kMaxBindings;
        for (uint32_t index = 0; index < kMaxBindings; ++index) {
            if (state.bindings[index].used == 0) {
                slot = index;
                break;
            }
        }
        if (slot == kMaxBindings) {
            state.devModel->Release(deviceHandle);
            return KernelError::kOutOfResources;
        }
        BindingRecord& binding = state.bindings[slot];
        binding = BindingRecord{};
        binding.bindHandle = NextBindHandle(state);
        binding.deviceHandle = deviceHandle;
        binding.driverIndex = driverIndex;
        binding.state = kBindingStateBound;
        binding.driverModule = ModuleId{};
        binding.used = 1;
        ++state.bindingCount;
        const KernelError syncError = SyncBinding(state, binding, state.drivers[driverIndex].desc, kBindingStateBound);
        if (syncError != KernelError::kOk) {
            DropBinding(state, slot);
            return syncError;
        }
        *bindHandle = binding.bindHandle;
        return KernelError::kOk;
    }

    KernelError Start(State& state, uint32_t bindHandle) {
        if (state.devModel == nullptr) return KernelError::kState;
        const uint32_t index = FindBinding(state, bindHandle);
        if (index == kMaxBindings) return KernelError::kNotFound;
        BindingRecord& binding = state.bindings[index];
        if (binding.state == kBindingStateStarted) return KernelError::kState;
        const DriverRecord& driver = state.drivers[binding.driverIndex];
        if (driver.desc.start == nullptr) return KernelError::kNotSupported;
        const Storage::Error startResult = driver.desc.start(driver.desc.context);
        if (startResult != Storage::Error::Ok) {
            const KernelError error = KernelErrorFromStorage(startResult);
            DropBinding(state, index);
            return error;
        }
        binding.state = kBindingStateStarted;
        const KernelError syncError = SyncBinding(state, binding, driver.desc, kBindingStateStarted);
        if (syncError != KernelError::kOk) {
            DropBinding(state, index);
            return syncError;
        }
        return KernelError::kOk;
    }

    KernelError Stop(State& state, uint32_t bindHandle) {
        if (state.devModel == nullptr) return KernelError::kState;
        const uint32_t index = FindBinding(state, bindHandle);
        if (index == kMaxBindings) return KernelError::kNotFound;
        const BindingRecord binding = state.bindings[index];
        if (binding.state == kBindingStateStarted) {
            const DriverRecord& driver = state.drivers[binding.driverIndex];
            if (driver.desc.stop != nullptr) driver.desc.stop(driver.desc.context);
        }
        DropBinding(state, index);
        return KernelError::kOk;
    }

    KernelError LoadExternal(State& state, const char* path) {
        (void)path;
        ++state.externalAttempts;
        Log(state, "WARN", kMarkerExternalUnsupported);
        return KernelError::kNotSupported;
    }
}
