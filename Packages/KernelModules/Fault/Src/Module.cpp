/* Module.cpp
    Fault 夹具模块：必需运行期模块 Init 失败注入

    只做一件事：核对内核传入的 KernelAbiContext，然后故意返回 kCorrupt。
    内核运行期装载在必需模块 Init 失败时走 FATAL + 屏幕 + 等待 + 复位路径，本模块是
    这条路径的注入载体；不申请资源、不发布导出、不启用中断与线程，Abort 由
    Common/ModuleEntry.cpp 的弱默认实现承担
    只进验证变体集，不随正式发行集交付
*/

#include <ModuleAbi.hpp>

// Init 入口：先核对上下文再返回失败
// 失败注入只发生在上下文合法时，避免把「参数非法」与「模块主动失败」混成一类，
// 装载器的 detail 与编号按返回码区分
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
    return KernelError::kCorrupt;
}
