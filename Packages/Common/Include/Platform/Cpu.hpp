/* Cpu.hpp
    CPU 事实探测与早期控制：工作模式识别、CPUID 能力快照、A20 门开关

    调用环境是 32 位保护模式（Baleen Stub / Core 的 C++ 环境，平坦段）；实模式与虚拟 8086
    只作为 Mode 的识别结果，不作为本类的调用环境。模式提升（实模式 → 保护模式 → 长模式）
    尚未实现：Core 的交权模式与内存布局未定，带远跳转的切换留在各阶段汇编入口，本类只探测
    成员初值都是常量，静态实例不依赖启动运行库
*/
#pragma once
#include <stdint.h>

namespace Platform {
    // CPU 工作模式，按当前执行环境区分
    enum class CpuMode {
        Unknown,        // 尚未 Install，模式未知
        Real,           // 16 位实模式
        V8086,          // 虚拟 8086
        Protected16,    // 16 位保护模式
        Protected32,    // 32 位保护模式
        Long,           // IA-32e 长模式，当前代码段按 64 位运行
    };

    // CPU 能力快照与 A20 门：Install 探测一次，之后只读查询
    class Cpu {
    public:
        // 探测 CPU 能力并记录当前工作模式；可重复调用，无 CPUID 的机器上只记录工作模式
        void Install();

        // 当前工作模式；Install 之前为 CpuMode::Unknown
        CpuMode Mode() const { return m_mode; }
        // 是否支持 CPUID 指令
        bool HasCpuid() const { return m_hasCpuid; }
        // 是否支持物理地址扩展（PAE）
        bool HasPae() const { return m_hasPae; }
        // 是否支持 IA-32e 长模式
        bool HasLongMode() const { return m_hasLongMode; }
        // CPUID 基本功能号上限；无 CPUID 时为 0
        uint32_t MaxBasicLeaf() const { return m_maxBasicLeaf; }
        // CPUID 扩展功能号上限；无扩展功能号时为 0
        uint32_t MaxExtendedLeaf() const { return m_maxExtendedLeaf; }
        // 厂商标识串，12 个字符加结尾零；无 CPUID 时为空串
        const char* Vendor() const { return m_vendor; }

        // 打开 A20 并回读验证，已打开也返回 true：快速门无效时退回键盘控制器兜底，仍失败返回 false
        // 验证按平坦段访问物理 0x200 与 0x100200，调用方须保证数据段基址为 0 且覆盖 1MiB 以上
        bool EnableA20();

    private:
        CpuMode m_mode = CpuMode::Unknown;   // 最近一次 Install 记录的工作模式
        bool m_hasCpuid = false;             // 是否支持 CPUID
        bool m_hasPae = false;               // 是否支持 PAE
        bool m_hasLongMode = false;          // 是否支持长模式
        uint32_t m_maxBasicLeaf = 0;         // CPUID 基本功能号上限
        uint32_t m_maxExtendedLeaf = 0;      // CPUID 扩展功能号上限
        char m_vendor[13] = {};              // 厂商标识串，12 个字符加结尾零

        // 判定当前工作模式：PE、LMA 与 CS 描述符属性
        CpuMode DetectMode() const;
    };
}
