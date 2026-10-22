/* Module.cpp
    Hello 模块：模块骨架样板的最小实现

    只做一件事：核对内核传入的 KernelAbiContext 并记录上下文事实；不申请资源、不发布导出、
    不启用中断与线程——这是 Init“可撤销准备”语义的最小实现
    四段都有内容，骨架复制后可直接复核容器段表：
      .rodata 放逻辑名
      .data   放自描述表，持有指向只读段的指针，构建侧因此产出 R_X86_64_RELATIVE
      .bss    放上下文事实，验证装载器映射后的清零与可写段
    后续模块在 Src 里加实现，并按 Module.mk 的 CONTRACT_EXPORTS / CONTRACT_IMPORTS 声明
    导出与导入；本文件演示导入表：Core.Log 的 4 个槽由装载器在 Init 前回填，
    Init 里按槽调用（槽为空表示未回填，不调用）
*/

#include <ModuleAbi.hpp>

#include <stdint.h>

#ifndef MODULE_LOGICAL_NAME
#error "MODULE_LOGICAL_NAME 未定义：模块包必须经 Common/ModuleRules.mk 构建"
#endif

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
}

// 导入回填区：Core.Log v1 的 4 个槽，零初始化落 .bss；装载器按导入表回填函数指针
// 宏参数是稳定名去点后的写法（Core.Log -> CoreLog），与生成头符号 _Contract_Import_CoreLog_v1 对应
LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};

// Init 入口：只读上下文、演示经导入槽写一行日志，不登记任何资源，因此总是成功
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    g_contextBytes = context->bytes;
    g_contextVersion = context->version;
    if (g_selfDescription.nameBytes == 0) return KernelError::kCorrupt;
    // 演示导入用法：装载器在 Init 前回填槽位；未回填时槽为空，不调用避免空指针
    if (_Contract_Import_CoreLog_v1.Write != nullptr) _Contract_Import_CoreLog_v1.Write("Hello: module init ok");
    return KernelError::kOk;
}

// Abort 入口：Init 没登记资源，撤销是空操作；强符号覆盖 ModuleEntry.cpp 的弱默认实现
extern "C" KernelError _Module_Abort(const KernelAbiContext* context) {
    (void)context;
    return KernelError::kOk;
}
