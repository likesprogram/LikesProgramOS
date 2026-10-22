/* DeviceModel.cpp
    DevModel 六槽实现：PCI 枚举结果的设备记录、资源查询、认领、启动候选与绑定事实

    Init 只做枚举与记录（可撤销、无外部资源）；Enumerate/Resources/Claim/Release/
    BootCandidates/Binding 都是同步、定长、无动态分配的实现，句柄是记录下标 + 1
    绑定登记约定见 DeviceModel.hpp：查询零初始化（bytes 为 0），登记/更新带结构长度与
    非零 state，清除带结构长度与零 state；未认领的设备拒绝登记绑定，避免绕过唯一控制者
*/

#include "DeviceModel.hpp"

namespace {
    // 类码里的主类与子类
    uint32_t ClassOf(uint32_t classCode) { return (classCode >> 16) & 0xFFu; }
    uint32_t SubclassOf(uint32_t classCode) { return (classCode >> 8) & 0xFFu; }

    // 是否大容量存储控制器：PCI 主类 0x01
    bool IsStorage(const DevModel::DeviceRecord& device) {
        return ClassOf(device.facts.classCode) == DevModel::kPciClassStorage;
    }

    // 启动候选判定：先限定存储控制器，再按线索过滤介质；线索回退时返回全部存储控制器
    bool IsBootCandidate(const DevModel::State& state, const DevModel::DeviceRecord& device) {
        if (!IsStorage(device)) return false;
        if (state.clueFallback != 0) return true;
        const uint32_t subclass = SubclassOf(device.facts.classCode);
        if (state.clueMedia == DevModel::kMediaHdd) {
            return subclass == DevModel::kPciSubclassIde || subclass == DevModel::kPciSubclassAta
                || subclass == DevModel::kPciSubclassSata || subclass == DevModel::kPciSubclassNvme;
        }
        if (state.clueMedia == DevModel::kMediaCdrom) {
            return subclass == DevModel::kPciSubclassIde || subclass == DevModel::kPciSubclassAta
                || subclass == DevModel::kPciSubclassSata;
        }
        return true;
    }

    // 输出结构长度核对：0 表示按 v1 全量写回，否则必须容得下 v1 结构
    template <typename Struct>
    bool AcceptOutput(const Struct* output) {
        return output->bytes == 0 || output->bytes >= sizeof(Struct);
    }

    // 写一行诊断：tag 用 INFO / WARN，文本是固定英文标记
    void Log(const DevModel::State& state, const char* tag, const char* text) {
        if (state.platform.logLine != nullptr) state.platform.logLine(state.platform.context, tag, text);
    }

    // 把契约结构复制到调用方缓冲并写回长度
    template <typename Struct>
    void WriteOutput(Struct* destination, const Struct& source) {
        *destination = source;
        destination->bytes = sizeof(Struct);
    }
}

namespace DevModel {
    KernelError Init(State& state, const Platform& platform) {
        if (platform.pci.read == nullptr || platform.pci.write == nullptr || platform.bootInfo == nullptr
            || platform.physical == nullptr || platform.logLine == nullptr) {
            return KernelError::kInvalidArgument;
        }
        if (state.initialized != 0) return KernelError::kOk;
        // 清空上一次的残留：记录、候选与线索都从零开始
        for (uint32_t index = 0; index < kMaxDevices; ++index) {
            state.devices[index] = DeviceRecord{};
            state.functions[index] = PciFunction{};
        }
        state.deviceCount = 0;
        state.storageCount = 0;
        state.candidateCount = 0;
        state.clueKind = 0;
        state.clueMedia = 0;
        state.clueDrive = kNoClueDrive;
        state.clueFallback = 0;
        state.platform = platform;
        const PciEnumeration enumeration = EnumeratePci(platform.pci, state.functions, kMaxDevices);
        state.deviceCount = enumeration.count;
        for (uint32_t index = 0; index < state.deviceCount; ++index) {
            const PciFunction& function = state.functions[index];
            DeviceRecord& device = state.devices[index];
            device.facts.bytes = sizeof(DeviceFacts);
            device.facts.deviceHandle = HandleOf(index);
            device.facts.busKind = kBusKindPci;
            device.facts.classCode = function.classCode;
            device.facts.vendorId = function.vendorId;
            device.facts.deviceId = function.deviceId;
            device.facts.busAddress = function.busAddress;
            device.resources.bytes = sizeof(DeviceResources);
            device.resources.barCount = function.barCount;
            for (uint32_t bar = 0; bar < function.barCount && bar < kDeviceMaxBars; ++bar) {
                device.resources.bars[bar] = function.bars[bar];
            }
            if (function.irq != kNoIrq) {
                device.resources.irqs[0].irq = function.irq;
                device.resources.irqs[0].flags = kIrqFlagPciLine;
                device.resources.irqCount = 1;
            }
            device.resources.dmaAddressBits = kDmaAddressBits;
        }
        // 启动线索：只有 BIOS 驱动器号可读时才按介质过滤，其余情况退回全量存储控制器
        const Boot::Info* info = platform.bootInfo(platform.context);
        if (info == nullptr) {
            Log(state, "WARN", kMarkerBootInfoMissing);
            return KernelError::kCorrupt;
        }
        state.clueKind = info->deviceKind;
        state.clueMedia = info->media;
        if (info->deviceKind == static_cast<uint32_t>(Boot::DeviceKind::BiosDrive)) {
            const uint8_t* drive = static_cast<const uint8_t*>(platform.physical(platform.context, info->devicePath, 1));
            if (info->devicePath == 0 || info->devicePathBytes < 1 || drive == nullptr) {
                state.clueFallback = 1;
            } else {
                state.clueDrive = drive[0];
            }
        } else {
            state.clueFallback = 1;
        }
        for (uint32_t index = 0; index < state.deviceCount; ++index) {
            DeviceRecord& device = state.devices[index];
            if (IsStorage(device)) ++state.storageCount;
            device.candidate = IsBootCandidate(state, device) ? 1 : 0;
            if (device.candidate != 0) ++state.candidateCount;
        }
        if (enumeration.truncated != 0) Log(state, "WARN", kMarkerTableFull);
        Log(state, "INFO", kMarkerEnumeration);
        Log(state, state.clueFallback != 0 ? "WARN" : "INFO",
            state.clueFallback != 0 ? kMarkerClueFallback : kMarkerClueMatched);
        state.initialized = 1;
        return KernelError::kOk;
    }

    void Abort(State& state) {
        for (uint32_t index = 0; index < state.deviceCount; ++index) {
            state.devices[index].claimed = 0;
            state.devices[index].bound = 0;
            state.devices[index].binding = BindingFacts{};
        }
        state.deviceCount = 0;
        state.storageCount = 0;
        state.candidateCount = 0;
        state.initialized = 0;
    }

    KernelError Enumerate(State& state, uint32_t* cursor, DeviceFacts* facts) {
        if (cursor == nullptr || facts == nullptr) return KernelError::kInvalidArgument;
        if (!AcceptOutput(facts)) return KernelError::kInvalidArgument;
        if (*cursor >= state.deviceCount) return KernelError::kNotFound;
        const DeviceRecord& device = state.devices[*cursor];
        WriteOutput(facts, device.facts);
        *cursor = *cursor + 1;
        return KernelError::kOk;
    }

    KernelError Resources(State& state, uint32_t deviceHandle, DeviceResources* resources) {
        if (resources == nullptr) return KernelError::kInvalidArgument;
        if (!AcceptOutput(resources)) return KernelError::kInvalidArgument;
        if (!ValidHandle(state, deviceHandle)) return KernelError::kNotFound;
        WriteOutput(resources, state.devices[deviceHandle - 1].resources);
        return KernelError::kOk;
    }

    KernelError Claim(State& state, uint32_t deviceHandle, uint32_t flags, uint32_t* claimHandle) {
        if (claimHandle == nullptr) return KernelError::kInvalidArgument;
        if (flags != kClaimFlagsNone) return KernelError::kInvalidArgument;
        if (!ValidHandle(state, deviceHandle)) return KernelError::kNotFound;
        DeviceRecord& device = state.devices[deviceHandle - 1];
        if (device.claimed != 0) return KernelError::kBusy;
        device.claimed = 1;
        *claimHandle = deviceHandle;
        return KernelError::kOk;
    }

    KernelError Release(State& state, uint32_t deviceHandle) {
        if (!ValidHandle(state, deviceHandle)) return KernelError::kNotFound;
        DeviceRecord& device = state.devices[deviceHandle - 1];
        if (device.bound != 0) return KernelError::kBusy;
        if (device.claimed == 0) return KernelError::kState;
        device.claimed = 0;
        return KernelError::kOk;
    }

    KernelError BootCandidates(State& state, uint32_t* cursor, uint32_t* deviceHandle) {
        if (cursor == nullptr || deviceHandle == nullptr) return KernelError::kInvalidArgument;
        for (uint32_t index = *cursor; index < state.deviceCount; ++index) {
            if (state.devices[index].candidate == 0) continue;
            *deviceHandle = HandleOf(index);
            *cursor = index + 1;
            return KernelError::kOk;
        }
        return KernelError::kNotFound;
    }

    KernelError Register(State& state, const DeviceFacts* facts, uint32_t* deviceHandle) {
        if (facts == nullptr || deviceHandle == nullptr) return KernelError::kInvalidArgument;
        if (state.initialized == 0) return KernelError::kState;
        if (facts->bytes < sizeof(DeviceFacts)) return KernelError::kInvalidArgument;
        if (facts->busKind == 0) return KernelError::kInvalidArgument;
        if (facts->classCode > 0xFFFFFFu) return KernelError::kInvalidArgument;
        if (facts->vendorId > 0xFFFFu || facts->deviceId > 0xFFFFu) return KernelError::kInvalidArgument;
        // 同一总线上的同一总线地址只能有一条记录
        for (uint32_t index = 0; index < state.deviceCount; ++index) {
            const DeviceRecord& device = state.devices[index];
            if (device.facts.busKind == facts->busKind && device.facts.busAddress == facts->busAddress) {
                return KernelError::kAlreadyExists;
            }
        }
        if (state.deviceCount >= kMaxDevices) return KernelError::kOutOfResources;
        DeviceRecord& device = state.devices[state.deviceCount];
        device = DeviceRecord{};
        device.facts = *facts;
        device.facts.bytes = sizeof(DeviceFacts);
        device.facts.deviceHandle = HandleOf(state.deviceCount);
        device.resources.bytes = sizeof(DeviceResources);
        device.resources.dmaAddressBits = kDmaAddressBits;
        if (IsStorage(device)) ++state.storageCount;
        device.candidate = IsBootCandidate(state, device) ? 1 : 0;
        if (device.candidate != 0) ++state.candidateCount;
        *deviceHandle = device.facts.deviceHandle;
        ++state.deviceCount;
        return KernelError::kOk;
    }

    KernelError Binding(State& state, uint32_t deviceHandle, BindingFacts* facts) {
        if (facts == nullptr) return KernelError::kInvalidArgument;
        if (!ValidHandle(state, deviceHandle)) return KernelError::kNotFound;
        DeviceRecord& device = state.devices[deviceHandle - 1];
        // 查询模式：零初始化的调用方把 bytes 留 0；未绑定返回 kNotFound
        if (facts->bytes == 0) {
            if (device.bound == 0) return KernelError::kNotFound;
            WriteOutput(facts, device.binding);
            return KernelError::kOk;
        }
        // 登记 / 清除模式：长度必须正好是 v1 结构大小
        if (facts->bytes != sizeof(BindingFacts)) return KernelError::kInvalidArgument;
        if (device.claimed == 0) return KernelError::kState;
        if (facts->state == 0) {
            device.bound = 0;
            device.binding = BindingFacts{};
            return KernelError::kOk;
        }
        device.binding = *facts;
        device.binding.bytes = sizeof(BindingFacts);
        device.binding.deviceHandle = deviceHandle;
        device.bound = 1;
        return KernelError::kOk;
    }
}
