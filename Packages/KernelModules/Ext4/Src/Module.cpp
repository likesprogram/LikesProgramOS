/* Module.cpp
    Ext4 模块：只读 Ext4 文件系统实现，向 Fs.Vfs 登记自己（逻辑名/版本保持 1.0.0）

    职责边界：本模块只解析卷格式并回答读取请求（Format/Mount/Unmount/Caps/VolumeIdentity/
    Stat/Open/Read/ReadDirectory），不导出任何单例接口（CONTRACT_EXPORTS 为空）——文件系统
    实例不是接口实例，登记由 Vfs 的 Fs.Vfs 统一持有
    Init 只做可撤销准备：核对上下文与自己 ModuleId 一致、核对 Fs.Vfs 导入已回填，
    然后把 FileSystemOps 表与模块内静态卷实例交给 RegisterFileSystem；不挂载、不读卷
    Mount 由 Vfs 的 Boot.Volume 在候选设备上调用，失败返回 Storage::Error，
    由 Vfs 汇总为 E-KRN-7x
    Fs.Vfs v1 没有注销槽：登记只可能在 Init 成功路径上发生，装载器不会对成功的 Init 调
    Abort；Init 失败在登记之前返回，因此 Abort 没有可撤销的资源，是空操作
    导入回填区由装载器在 Init 之前按导入表写入；模块内其它静态缓冲都零初始化，不产生
    全局动态初始化（.init_array 由链接期 --orphan-handling=error 拦下）
*/

#include <ModuleAbi.hpp>

#include <Ext4Ops.hpp>
#include <Ext4Volume.hpp>

#include <stdint.h>

#ifndef MODULE_LOGICAL_NAME
#error "MODULE_LOGICAL_NAME 未定义：模块包必须经 Common/ModuleRules.mk 构建"
#endif

// Fs.Vfs v1 的导入回填区：8 槽，装载器在 Init 前写入 Vfs 提供的函数指针
// 这里定义落 .bss，零初始化；地址与槽位偏移由 PackModule 从 ELF 符号表取
LP_CONTRACT_IMPORT(FsVfs, 1, FsVfsSlotsV1) = {};

// Core.Log v1 的导入回填区：4 槽，Init 失败的稳定原因经它写到内核日志
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};

namespace {
    // 逻辑名：只读段；参与 ModuleId 派生，与 Module.mk 的 MODULE_LOGICAL_NAME 同源
    const char kLogicalName[] = MODULE_LOGICAL_NAME;

    // 模块自描述表：可写段；logicalName 指向 .rodata，构建侧必须产出 R_X86_64_RELATIVE
    struct SelfDescription {
        const char* logicalName;   // 逻辑名字符串
        uint32_t nameBytes;        // 逻辑名字节数，不含结尾零
    };

    // 自描述表实例；used 保证 -O2 下不被当作无用变量删掉，指针初值因此留在 .data
    SelfDescription g_selfDescription __attribute__((used)) = { kLogicalName, sizeof(kLogicalName) - 1 };

    // 最近一次 Init 看到的上下文事实：BSS，装载器映射后清零
    volatile uint32_t g_contextBytes;     // KernelAbiContext.bytes
    volatile uint16_t g_contextVersion;   // KernelAbiContext.version
    // Fs.Vfs 登记回来的文件系统标识；0 表示未登记
    FileSystemId g_fileSystemId;

    // 写一条固定日志：Core.Log 的 Line 槽已回填才写，不把诊断失败变成启动失败
    void LogLine(const char* text) {
        if (_Contract_Import_CoreLog_v1.Line == nullptr) return;
        _Contract_Import_CoreLog_v1.Line("Ext4", text);
    }
}

// Init 入口：静态校验 + 向 Fs.Vfs 登记；不挂载、不读卷，失败可撤销（登记前没有资源）
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    g_contextBytes = context->bytes;
    g_contextVersion = context->version;
    // 静态校验：装载器给的 ModuleId 必须就是本模块（生成头常量与 BootClosure.def 同源）
    if (!(context->moduleId == kModExt4)) return KernelError::kCorrupt;
    if (g_selfDescription.nameBytes == 0) return KernelError::kCorrupt;
    // 导入槽必须已被装载器回填：未回填说明导入解析没有做，直接拒绝而不是空指针调用
    if (_Contract_Import_FsVfs_v1.RegisterFileSystem == nullptr) {
        LogLine("the Fs.Vfs import isn't bound");
        return KernelError::kState;
    }
    FileSystemId id = 0;
    const Storage::Error error = _Contract_Import_FsVfs_v1.RegisterFileSystem(KernelModules::Ext4::FileSystemOpsTable(), KernelModules::Ext4::FileSystemContext(), &id);
    if (error != Storage::Error::Ok) {
        LogLine("registering the ext4 file system failed");
        return KernelErrorFromStorage(error);
    }
    if (id == 0) return KernelError::kCorrupt;
    g_fileSystemId = id;
    return KernelError::kOk;
}

// Abort 入口：Init 只做了 Fs.Vfs 登记（成功路径不会被 Abort），没有其它资源可撤销
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    return KernelError::kOk;
}
