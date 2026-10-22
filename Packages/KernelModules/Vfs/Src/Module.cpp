/* Module.cpp
    Vfs 模块入口与契约表：提供 Block.Registry / Fs.Vfs / Boot.Volume 三个闭包单例

    当前实现替换玩具骨架：Init 只复位注册表与句柄表并核对导入回填区（可撤销、不申请
    资源），驱动经 Block.Registry.Publish 登记设备、文件系统经 Fs.Vfs.RegisterFileSystem
    登记实现，系统卷挂载由内核编排在闭包全部 Init 后调用 Boot.Volume.MountSystemVolume
    导入表由装载器在 Init 前回填；本文只检查必需槽非空，不做二次绑定
    导出槽表必须落可写段（PackModule 逐槽读 RELATIVE 重定位），初始化器只放模块内函数地址
*/

#include <ModuleAbi.hpp>

#include <VfsCore.hpp>

namespace {
    // 模块是否已完成 Init：Init 之后槽位调用才可依赖注册表
    bool g_initialized = false;

    // 导入回填区是否已由装载器填好：本模块后续槽调用依赖这些函数指针
    bool ImportsReady() {
        return _Contract_Import_CoreBoot_v1.Info != nullptr
            && _Contract_Import_CoreLog_v1.Write != nullptr
            && _Contract_Import_CoreLog_v1.Line != nullptr
            && _Contract_Import_CoreMem_v1.Copy != nullptr
            && _Contract_Import_CoreMem_v1.Zero != nullptr
            && _Contract_Import_CoreMem_v1.Compare != nullptr
            && _Contract_Import_CorePage_v1.PageBytes != nullptr
            && _Contract_Import_DevModel_v1.Enumerate != nullptr
            && _Contract_Import_DevModel_v1.Binding != nullptr;
    }
}

// Init 入口：核对上下文与导入面，复位注册表；不申请资源，失败可从 Abort 撤销
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < static_cast<uint32_t>(sizeof(KernelAbiContext))) return KernelError::kInvalidArgument;
    if (!(context->moduleId == kModVfs)) return KernelError::kInvalidArgument;
    if (!ImportsReady()) return KernelError::kNotSupported;
    VfsModule::Reset();
    // 唯一登记点是本模块内部的块设备层，从 Dev.Model 绑定事实逐个 Publish
    const KernelError registration = VfsModule::RegisterDevicesFromBindings();
    if (registration != KernelError::kOk) {
        VfsModule::Reset();
        return registration;
    }
    g_initialized = true;
    return KernelError::kOk;
}

// Abort 入口：清空注册表与句柄表，回到未初始化状态
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    VfsModule::Reset();
    g_initialized = false;
    return KernelError::kOk;
}

// 契约导入回填区：装载器按导入表回填，模块在 Init 之后按槽调用
LP_CONTRACT_IMPORT(CoreBoot, 1, CoreBootSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
LP_CONTRACT_IMPORT(CoreMem, 1, CoreMemSlotsV1) = {};
LP_CONTRACT_IMPORT(CorePage, 1, CorePageSlotsV1) = {};
LP_CONTRACT_IMPORT(DevModel, 1, DevModelSlotsV1) = {};

// 闭包单例导出槽表：提供者唯一，槽序由生成头 Slots.hpp 钉住
LP_CONTRACT_EXPORT(BlockRegistry, 1, BlockRegistrySlotsV1) = {
    &VfsModule::Publish, &VfsModule::CreateSubDevice, &VfsModule::Acquire, &VfsModule::Release};

LP_CONTRACT_EXPORT(FsVfs, 1, FsVfsSlotsV1) = {
    &VfsModule::RegisterFileSystem, &VfsModule::Mount, &VfsModule::Unmount,
    &VfsModule::Stat, &VfsModule::Open, &VfsModule::Read, &VfsModule::Close, &VfsModule::ReadDirectory};

LP_CONTRACT_EXPORT(BootVolume, 1, BootVolumeSlotsV1) = {&VfsModule::MountSystemVolume};

namespace VfsModule {
    void Reset() {
        ResetBlockRegistry();
        ResetVfsTables();
        g_initialized = false;
    }

    bool Initialized() { return g_initialized; }
}
