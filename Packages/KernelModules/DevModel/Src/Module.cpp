/* Module.cpp
    DevModel 模块入口与契约表：导入 Core.Boot / Core.Log / Core.Mem，导出 Dev.Model v1 六槽与 v2 七槽

    本文件只做接线：平台回调转发到导入槽，六个槽表项转发到 DeviceModel 的免费函数，
    Init 只调用 DevModel::Init（枚举 + 记录，可撤销），Abort 清空认领与绑定
    导入回填区零初始化落 .bss，装载器在 Init 前按导入表把提供者函数指针写进 8 字节槽；
    导出槽表非 const 落可写段，PackModule 逐槽读 RELATIVE 重定位取代码偏移
    逻辑名与版本由 Module.mk 的 -D 传入，不重复写死
*/

#include <ModuleAbi.hpp>

#include "DeviceModel.hpp"
#include "Pci.hpp"

namespace {
    // 模块状态：设备记录与平台注入，静态持有，无动态初始化（全部零初始化）
    DevModel::State s_state;
    // Core.Boot 槽 0 的适配：交权事实由内核持有，模块只读
    const Boot::Info* BootInfoThunk(void* context) {
        (void)context;
        return _Contract_Import_CoreBoot_v1.Info();
    }
    // Core.Boot 槽 1 的适配：把物理地址换算成可读指针
    const void* PhysicalThunk(void* context, uint64_t physical, uint64_t bytes) {
        (void)context;
        return _Contract_Import_CoreBoot_v1.Physical(physical, bytes);
    }
    // Core.Log 槽 1 的适配：写 [ tag ] text 一行
    void LogLineThunk(void* context, const char* tag, const char* text) {
        (void)context;
        _Contract_Import_CoreLog_v1.Line(tag, text);
    }
    // Dev.Model 六个槽的转发：状态只有一份，槽回调直接操作它
    KernelError SlotEnumerate(uint32_t* cursor, DeviceFacts* facts) {
        return DevModel::Enumerate(s_state, cursor, facts);
    }
    KernelError SlotResources(uint32_t deviceHandle, DeviceResources* resources) {
        return DevModel::Resources(s_state, deviceHandle, resources);
    }
    KernelError SlotClaim(uint32_t deviceHandle, uint32_t flags, uint32_t* claimHandle) {
        return DevModel::Claim(s_state, deviceHandle, flags, claimHandle);
    }
    KernelError SlotRelease(uint32_t deviceHandle) {
        return DevModel::Release(s_state, deviceHandle);
    }
    KernelError SlotBootCandidates(uint32_t* cursor, uint32_t* deviceHandle) {
        return DevModel::BootCandidates(s_state, cursor, deviceHandle);
    }
    KernelError SlotBinding(uint32_t deviceHandle, BindingFacts* facts) {
        return DevModel::Binding(s_state, deviceHandle, facts);
    }
    // v2 槽 6：登记外部枚举发现的设备
    KernelError SlotRegister(const DeviceFacts* facts, uint32_t* deviceHandle) {
        return DevModel::Register(s_state, facts, deviceHandle);
    }
}

// 导入回填区：符号名与槽表类型来自生成头，装载器按导入表回填
LP_CONTRACT_IMPORT(CoreBoot, 1, CoreBootSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMem, 1, CoreMemSlotsV1) = {};

// 导出槽表：Dev.Model v1 六槽，符号名 _Contract_Export_DevModel_v1
LP_CONTRACT_EXPORT(DevModel, 1, DevModelSlotsV1) = {
    &SlotEnumerate, &SlotResources, &SlotClaim, &SlotRelease, &SlotBootCandidates, &SlotBinding};

// 导出槽表：Dev.Model v2 七槽（v1 六槽原序 + Register），符号名 _Contract_Export_DevModel_v2
// v1 消费者（Vfs）继续用 v1 槽表；v2 只追加登记槽，不改既有槽位
LP_CONTRACT_EXPORT(DevModel, 2, DevModelSlotsV2) = {
    &SlotEnumerate, &SlotResources, &SlotClaim, &SlotRelease, &SlotBootCandidates, &SlotBinding, &SlotRegister};

extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    if (_Contract_Import_CoreBoot_v1.Info == nullptr || _Contract_Import_CoreBoot_v1.Physical == nullptr
        || _Contract_Import_CoreLog_v1.Line == nullptr || _Contract_Import_CoreMem_v1.Zero == nullptr) {
        return KernelError::kCorrupt;
    }
    _Contract_Import_CoreMem_v1.Zero(&s_state, sizeof(s_state));
    DevModel::Platform platform;
    platform.pci = DevModel::KernelPciAccess();
    platform.bootInfo = &BootInfoThunk;
    platform.physical = &PhysicalThunk;
    platform.logLine = &LogLineThunk;
    platform.context = nullptr;
    return DevModel::Init(s_state, platform);
}

extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    DevModel::Abort(s_state);
    return KernelError::kOk;
}
