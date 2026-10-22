/* ModuleAbi.hpp
    模块侧契约入口：转发生成头，模块源码只 include 本头

    生成头就绪后本文件不再手写 KernelError / ModuleId / KernelAbiContext 副本，
    而是转发 Packages/Common/Out/Contract 的生成头（经 Common/Contract.hpp 聚合）：
      KernelError、ModuleId、KernelAbiContext、三入口声明、槽位结构与导入/导出表宏
    入口符号的 default 可见性由生成头 Entry.hpp 的三入口声明给出，模块源文件的定义继承
    该属性；模块编译用 -fvisibility=hidden，漏掉可见性会让 PackModule 取不到入口符号
    字节布局（KernelAbiContext 32 字节与各字段偏移）由生成头 Entry.hpp 的 static_assert 钉住
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

// 模块可读上下文的最小长度：至少要有 bytes 字段本身，更短时连长度都不可信
constexpr uint32_t kKernelAbiContextMinBytes = 4;
