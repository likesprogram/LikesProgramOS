/* ModuleEntry.cpp
    模块入口的弱符号默认实现：_Module_Activate 与 _Module_Abort 的缺省行为

    只有 Module.mk 打开 MODULE_ENTRY_DEFAULTS 时本文件才参与链接；模块包定义同名强符号
    即覆盖，链接器按强符号优先，不产生重复定义错误
    _Module_Init 不提供默认实现：容器要求 EntryInit 非零且真实存在，默认空实现会让
    “忘了写 Init”在链接期静默通过，那正是装载器要在运行期拒绝的模块
    默认 Abort 返回成功（没有可撤销的资源）；默认 Activate 返回 kNotSupported
    （当前不调用 Activate，后续阶段由模块自己实现）

    符号可见性来自 ModuleAbi.hpp 的 default 声明，nm 里是 W（弱全局）而不是未定义符号，
    nm -u 不会残留 _Module_* 下划线符号
*/

#include <ModuleAbi.hpp>

extern "C" {
    __attribute__((weak)) KernelError _Module_Activate(const KernelAbiContext* context) {
        (void)context;
        return KernelError::kNotSupported;
    }

    __attribute__((weak)) KernelError _Module_Abort(const KernelAbiContext* context) {
        (void)context;
        return KernelError::kOk;
    }
}
