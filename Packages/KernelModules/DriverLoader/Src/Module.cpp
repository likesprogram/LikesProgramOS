/* Module.cpp
    DriverLoader 模块入口与契约表：导入 Core.Log / Core.Mem / Dev.Model，导出 Drv.Loader v1 四槽

    本文件只做接线：日志转发到 Core.Log 行写槽，四个槽表项转发到 DriverRegistry 的免费函数，
    Init 里把 Dev.Model 导入槽表交给注册表并写 ready 标记，Abort 停止全部绑定并归还认领
    导入回填区零初始化落 .bss，装载器在 Init 前按导入表回填；导出槽表非 const 落可写段
    .drv 装载路径当前不接线：DriverLoader 没有对外槽位承载它，LoadExternal 只在宿主检查里
    被直接调用，返回 kNotSupported 并写固定 WARN 行
*/

#include <ModuleAbi.hpp>

#include "DriverRegistry.hpp"

namespace {
    // 模块状态：驱动与绑定表，静态持有，零初始化、无动态构造
    DrvLoader::State s_state;
    // Core.Log 槽 1 的适配：写 [ tag ] text 一行
    void LogLineThunk(void* context, const char* tag, const char* text) {
        (void)context;
        _Contract_Import_CoreLog_v1.Line(tag, text);
    }
    // Drv.Loader 四个槽的转发：状态只有一份，槽回调直接操作它
    KernelError SlotRegisterDriver(const DriverDesc* description) {
        return DrvLoader::RegisterDriver(s_state, description);
    }
    KernelError SlotBind(uint32_t deviceHandle, const DriverDesc* description, uint32_t* bindHandle) {
        return DrvLoader::Bind(s_state, deviceHandle, description, bindHandle);
    }
    KernelError SlotStart(uint32_t bindHandle) {
        return DrvLoader::Start(s_state, bindHandle);
    }
    KernelError SlotStop(uint32_t bindHandle) {
        return DrvLoader::Stop(s_state, bindHandle);
    }
}

// 导入回填区：符号名与槽表类型来自生成头，装载器按导入表回填
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMem, 1, CoreMemSlotsV1) = {};
LP_CONTRACT_IMPORT(DevModel, 1, DevModelSlotsV1) = {};

// 导出槽表：Drv.Loader v1 四槽，符号名 _Contract_Export_DrvLoader_v1
LP_CONTRACT_EXPORT(DrvLoader, 1, DrvLoaderSlotsV1) = {&SlotRegisterDriver, &SlotBind, &SlotStart, &SlotStop};

extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    if (_Contract_Import_CoreLog_v1.Line == nullptr || _Contract_Import_CoreMem_v1.Zero == nullptr) {
        return KernelError::kCorrupt;
    }
    if (_Contract_Import_DevModel_v1.Enumerate == nullptr || _Contract_Import_DevModel_v1.Claim == nullptr
        || _Contract_Import_DevModel_v1.Release == nullptr || _Contract_Import_DevModel_v1.Binding == nullptr) {
        return KernelError::kCorrupt;
    }
    _Contract_Import_CoreMem_v1.Zero(&s_state, sizeof(s_state));
    return DrvLoader::Init(s_state, &_Contract_Import_DevModel_v1, &LogLineThunk, nullptr);
}

extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    DrvLoader::Abort(s_state);
    return KernelError::kOk;
}
