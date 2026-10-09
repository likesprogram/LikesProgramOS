/* Cpu.hpp
    内核侧 CPU 事实快照：入口后探测一次，记录厂商串与长模式、TSC、APIC、NX 能力

    不复用 Platform::Cpu：那份实现按 32 位保护模式编译，EFLAGS、SGDT 与固定地址测试
    在 64 位下有编译期拒绝；内核侧的探测是独立的一份，只读事实，不改控制寄存器
*/

#pragma once
#include <stdint.h>

namespace Kernel {
    // CPU 能力快照：Install 探测一次，之后只读查询
    class Cpu {
    public:
        // 探测厂商串与能力位；可重复调用，无 CPUID 时只留下空串与全假能力位
        bool Install();
        // 是否支持 CPUID 指令
        bool HasCpuid() const { return m_hasCpuid; }
        // 是否支持 IA-32e 长模式
        bool HasLongMode() const { return m_hasLongMode; }
        // 是否支持时间戳计数器
        bool HasTsc() const { return m_hasTsc; }
        // 是否支持本地 APIC
        bool HasApic() const { return m_hasApic; }
        // 是否支持不可执行位（NX）
        bool HasNx() const { return m_hasNx; }
        // 厂商标识串，12 个字符加结尾零；未探测时为空串
        const char* Vendor() const { return m_vendor; }

    private:
        bool m_hasCpuid = false;      // 是否支持 CPUID
        bool m_hasLongMode = false;   // 是否支持长模式
        bool m_hasTsc = false;        // 是否支持时间戳计数器
        bool m_hasApic = false;       // 是否支持本地 APIC
        bool m_hasNx = false;         // 是否支持不可执行位
        char m_vendor[13] = {};       // 厂商标识串，12 个字符加结尾零
    };
}
