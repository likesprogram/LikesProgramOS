/* Module.cpp
    Optional 夹具模块：可选运行期模块失败/成功两版注入

    默认（未定义 OPTIONAL_INIT_OK）：核对 KernelAbiContext 后返回 kCorrupt，内核运行期装载
    对可选模块只输出 WARN、不重启，本模块是这条路径的注入载体
    定义 OPTIONAL_INIT_OK 的构建（见 ../Makefile.ok）：同一入口返回 kOk，作为「可选模块
    成功」的对照证据
    不申请资源、不发布导出、不启用中断与线程，Abort 由 Common/ModuleEntry.cpp 的弱默认
    实现承担；只进验证变体集，不随正式发行集交付
*/

#include <ModuleAbi.hpp>

// Init 入口：先核对上下文，再按构建变体返回失败或成功
// 失败注入只发生在上下文合法时，避免把「参数非法」与「模块主动失败」混成一类
extern "C" KernelError _Module_Init(const KernelAbiContext* context) {
    if (context == nullptr) return KernelError::kInvalidArgument;
    if (context->bytes < kKernelAbiContextMinBytes) return KernelError::kInvalidArgument;
#ifdef OPTIONAL_INIT_OK
    return KernelError::kOk;
#else
    return KernelError::kCorrupt;
#endif
}
