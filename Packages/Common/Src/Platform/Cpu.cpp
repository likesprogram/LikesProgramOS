/* Cpu.cpp
    CPU 事实探测与早期控制的实现：CPUID 能力快照、工作模式识别、A20 门开关

    纯汇编原语在 Cpu.asm：端口与绝对地址读写、标志与寄存器读取、CPUID、GDTR 读取
    依据（协议与接口事实，取自下列文档；协议未规定的取值由本项目选定，理由写在对应位置）：
      端口 0x92 的 A20 位与 INIT 位：Intel 82801DB（ICH4）数据手册 §9.7.3
      "PORT92—Fast A20 and Init Register"、Intel 82371AB（PIIX4）数据手册 §4.2.7.2 "P92"
      A20M# 把物理地址 bit20 强制为 0：Intel SDM 卷 3A §11.7.13.4
      8042 的命令 D1、输出端口 bit1（Gate A20）与 bit0（System reset）、状态口 64h 的位定义：
      IBM PC/AT 技术参考（1985）与 IBM PS/2 硬件接口技术参考（1988）
      CPUID 可用性与功能位编号：Intel SDM 卷 2A 的 CPUID 条目，长模式支持见卷 3A §5.2
      IA32_EFER（MSR 0C0000080H）的 LME 与 LMA：Intel SDM 卷 3A §2.2.1 Table 2-1
      段描述符的 L 与 D/B 位：Intel SDM 卷 3A §3.4.5 Figure 3-8
*/

#include <Platform/Cpu.hpp>

// Cpu.asm 与这里的标志寄存器读写、SGDT 结构与固定地址测试都是 32 位形式，64 位侧要另行实现
#if defined(__x86_64__)
#error "Platform::Cpu 的当前实现面向 32 位保护模式，不能按 64 位编译"
#endif

// Cpu.asm 提供的纯汇编原语，出入口约定是 cdecl：参数在栈上、调用方清栈
extern "C" void _Cpu_Out8(uint16_t port, uint8_t value);                            // 写 8 位 I/O 端口
extern "C" uint8_t _Cpu_In8(uint16_t port);                                         // 读 8 位 I/O 端口
extern "C" uint32_t _Cpu_Read32(uintptr_t address);                                 // 读绝对地址处的 32 位值
extern "C" void _Cpu_Write32(uintptr_t address, uint32_t value);                    // 写绝对地址处的 32 位值
extern "C" uint32_t _Cpu_ReadFlags();                                               // 读标志寄存器低 32 位
extern "C" void _Cpu_WriteFlags(uint32_t flags);                                    // 写标志寄存器低 32 位
extern "C" uint16_t _Cpu_ReadCr0();                                                 // 读 CR0 低 16 位
extern "C" uint16_t _Cpu_ReadCs();                                                  // 读当前 CS 选择子
extern "C" uint32_t _Cpu_ReadMsr(uint32_t index);                                   // 读 MSR 低 32 位
extern "C" void _Cpu_ReadGdtr(void* snapshot);                                      // 读 GDTR 到 6 字节快照
extern "C" void _Cpu_Cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);   // 执行 CPUID

namespace {
    // —— 端口与命令 ——
    constexpr uint16_t kPortIoDelay = 0x80;             // POST 口，只为制造总线延迟而写
    constexpr uint16_t kPortSystemControlA = 0x92;      // System Control Port A：ICH4 的 PORT92、PIIX4 的 P92
    constexpr uint16_t kPortKeyboardData = 0x60;        // 8042 数据口
    constexpr uint16_t kPortKeyboardCommand = 0x64;     // 8042 状态与控制命令口
    constexpr uint8_t kSystemControlA20 = 0x02;         // 端口 0x92 bit1：ALT_A20_GATE / FAST_A20，置 1 打开 A20
    constexpr uint8_t kSystemControlInit = 0x01;        // 端口 0x92 bit0：INIT_NOW / FAST_INIT，置 1 触发处理器 INIT
    constexpr uint8_t kStatusOutputFull = 0x01;         // 8042 状态口 bit0：输出缓冲满，此时才读 0x60
    constexpr uint8_t kStatusInputFull = 0x02;          // 8042 状态口 bit1：输入缓冲满，此时不能写 0x60 与 0x64
    constexpr uint8_t kKeyboardWriteOutputPort = 0xD1;  // 命令 0xD1：写输出端口，下一个写到 0x60 的字节进输出端口
    constexpr uint8_t kKeyboardA20On = 0xDF;            // 输出端口数据：bit1 置 1 打开 A20，bit0 保持 1（参考手册记载写 0 会复位处理器），其余位取沿用值

    // —— 等待与测试轮数（本项目选定，都设上限，不做无限等待）——
    constexpr uint32_t kKeyboardWaitLoops = 100000;   // 8042 输入缓冲为空的等待轮数上限，每轮含一次端口延迟
    constexpr uint32_t kA20ShortLoops = 32;           // 使能之前的短测试轮数：多次取样，避免一次总线假象就下结论
    constexpr uint32_t kA20LongLoops = 2097152;       // 使能之后的长测试轮数：等线路生效，总量级约到秒

    // —— A20 测试点（本项目选定）——
    constexpr uintptr_t kA20TestLow = 0x200;       // 低端：IVT 的 int 0x80 向量，本项目引导期不使用该向量
    constexpr uintptr_t kA20TestHigh = 0x100200;   // 高端：与低端相差 bit20，A20 关闭时该位被强制为 0，两者落在同一单元

    // —— 标志位与特性位 ——
    constexpr uint32_t kFlagsVirtual8086 = 1u << 17;   // EFLAGS 的 VM 位
    constexpr uint32_t kFlagsId = 1u << 21;            // EFLAGS 的 ID 位，可翻转即支持 CPUID
    constexpr uint16_t kCr0Protected = 0x0001;         // CR0 的 PE 位，落在低 16 位内
    constexpr uint32_t kFeaturePae = 1u << 6;          // CPUID.01H:EDX bit 6
    constexpr uint32_t kFeatureLongMode = 1u << 29;    // CPUID.80000001H:EDX bit 29
    constexpr uint32_t kMsrEfer = 0xC0000080;          // IA32_EFER
    constexpr uint32_t kEferLongModeActive = 1u << 10; // EFER 的 LMA 位，只读
    constexpr uint8_t kDescriptorLong = 0x20;          // 段描述符字节 6 的 bit5：L，即第二双字的 bit21
    constexpr uint8_t kDescriptorDefault32 = 0x40;     // 段描述符字节 6 的 bit6：D/B，即第二双字的 bit22

    // 端口延迟：对 POST 口写一次制造总线周期，让相邻端口操作之间有确定间隔
    inline void IoDelay() { _Cpu_Out8(kPortIoDelay, 0); }

    // 把厂商标识的三个双字按 EBX、EDX、ECX 顺序拼成 12 个字符并补结尾零
    void FillVendor(char* out, uint32_t ebx, uint32_t edx, uint32_t ecx) {
        const uint32_t words[3] = { ebx, edx, ecx };
        for (uint32_t i = 0; i < 3; ++i) {
            for (uint32_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<char>((words[i] >> (8 * j)) & 0xFF);
        }
        out[12] = '\0';
    }

    // ID 位可翻转即支持 CPUID：386 上该位不存在，写进去也读不回来
    bool ProbeCpuidSupport() {
        const uint32_t original = _Cpu_ReadFlags();
        _Cpu_WriteFlags(original ^ kFlagsId);
        const uint32_t flipped = _Cpu_ReadFlags();
        _Cpu_WriteFlags(original);   // 无论结果如何都恢复原值
        return ((original ^ flipped) & kFlagsId) != 0;
    }

    // 读取当前 CS 描述符字节 6 的 L 位与 D/B 位（第二双字的 bit21 与 bit22）；描述符不在 GDT 内时返回 false
    bool ReadCodeSegmentFlags(bool& longCode, bool& default32) {
        // 取 GDT 的线性基址与限长；保护模式下 GDTR 必然有效
        struct TableRegister {
            uint16_t Limit;   // 限长，字节数减一
            uint32_t Base;    // 线性基址
        } __attribute__((packed));
        TableRegister gdtr;
        _Cpu_ReadGdtr(&gdtr);
        const uint32_t offset = static_cast<uint32_t>(_Cpu_ReadCs() & 0xFFF8);   // 选择子高位是表索引
        if (offset + 8 > static_cast<uint32_t>(gdtr.Limit) + 1) return false;
        const uint8_t flags = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(gdtr.Base))[offset + 6];
        longCode = (flags & kDescriptorLong) != 0;
        default32 = (flags & kDescriptorDefault32) != 0;
        return true;
    }

    // 判定 A20 是否已打开：把探测值写入低端测试点，再读 A20 关闭时与它同一单元的高端测试点，
    // 两者不等说明 A20 已打开。短测试用于使能之前，长测试用于使能之后等线路生效。
    // 测试点内容写前保存、退出时恢复，因此不改变引导环境中固件留下的数据
    bool TestA20(uint32_t loops) {
        const uint32_t saved = _Cpu_Read32(kA20TestLow);
        uint32_t probe = 0;   // 从固定值起算，循环轮数即写入次数，不依赖读回的内容
        bool enabled = false;
        for (uint32_t i = 0; i < loops; ++i) {
            _Cpu_Write32(kA20TestLow, ++probe);
            IoDelay();   // 两次访问之间留出确定间隔
            if (_Cpu_Read32(kA20TestHigh) != probe) { enabled = true; break; }
        }
        _Cpu_Write32(kA20TestLow, saved);
        return enabled;
    }

    // 等到 8042 输入缓冲为空，可以写命令或数据；输出缓冲满时先读走 0x60 的数据再继续等
    // 两个缓冲都空才返回 true，等待轮数达到上限返回 false（没有控制器的机器上不无限等）
    bool WaitKeyboardBufferEmpty() {
        for (uint32_t i = 0; i < kKeyboardWaitLoops; ++i) {
            IoDelay();
            const uint8_t status = _Cpu_In8(kPortKeyboardCommand);
            if ((status & kStatusOutputFull) != 0) { IoDelay(); (void)_Cpu_In8(kPortKeyboardData); }
            else if ((status & kStatusInputFull) == 0) return true;
        }
        return false;
    }

    // 键盘控制器兜底：命令 0xD1 写输出端口，接着把 0xDF 写进 0x60 置起 A20
    // 序列到此为止：F0-FF 在参考手册里是脉冲输出端口命令，不参与打开 A20，成败由调用方的回读验证判定
    bool EnableA20Keyboard() {
        if (!WaitKeyboardBufferEmpty()) return false;
        _Cpu_Out8(kPortKeyboardCommand, kKeyboardWriteOutputPort);
        if (!WaitKeyboardBufferEmpty()) return false;
        _Cpu_Out8(kPortKeyboardData, kKeyboardA20On);
        return WaitKeyboardBufferEmpty();
    }

    // 快速门：把 System Control Port A 的 bit1 置 1；bit0 置 1 会触发处理器 INIT，写回前必须清零
    void EnableA20FastGate() {
        uint8_t value = _Cpu_In8(kPortSystemControlA);
        value = static_cast<uint8_t>((value | kSystemControlA20) & ~kSystemControlInit);
        _Cpu_Out8(kPortSystemControlA, value);
    }
}

namespace Platform {
    void Cpu::Install() {
        // 重复调用时先清干净，避免上一次的探测结果残留
        m_hasCpuid = false;
        m_hasPae = false;
        m_hasLongMode = false;
        m_maxBasicLeaf = 0;
        m_maxExtendedLeaf = 0;
        m_vendor[0] = '\0';

        uint32_t eax = 0;
        uint32_t ebx = 0;
        uint32_t ecx = 0;
        uint32_t edx = 0;
        m_hasCpuid = ProbeCpuidSupport();
        if (m_hasCpuid) {
            _Cpu_Cpuid(0, &eax, &ebx, &ecx, &edx);
            m_maxBasicLeaf = eax;
            FillVendor(m_vendor, ebx, edx, ecx);
            if (m_maxBasicLeaf >= 1) {
                _Cpu_Cpuid(1, &eax, &ebx, &ecx, &edx);
                m_hasPae = (edx & kFeaturePae) != 0;
            }
            // 扩展功能号上限：支持扩展叶时最高位为 1，数值偏小表示这台机器没有扩展叶
            _Cpu_Cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
            if (eax >= 0x80000000) {
                m_maxExtendedLeaf = eax;
                if (m_maxExtendedLeaf >= 0x80000001) {
                    _Cpu_Cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
                    m_hasLongMode = (edx & kFeatureLongMode) != 0;
                }
            }
        }

        m_mode = DetectMode();   // 长模式判定要读 EFER，因此放在能力快照之后
    }

    bool Cpu::EnableA20() {
        // 固件或补丁可能已经打开 A20，先做短测试
        if (TestA20(kA20ShortLoops)) return true;

        EnableA20FastGate();
        if (TestA20(kA20LongLoops)) return true;

        // 快速门没有接线的老机器由键盘控制器控制 A20
        if (!EnableA20Keyboard()) return false;
        return TestA20(kA20LongLoops);
    }

    CpuMode Cpu::DetectMode() const {
        // PE 用 SMSW 读 CR0，实模式下也能执行，不必先假定当前模式
        if ((_Cpu_ReadCr0() & kCr0Protected) == 0) return (_Cpu_ReadFlags() & kFlagsVirtual8086) != 0 ? CpuMode::V8086 : CpuMode::Real;

        // LMA 是长模式的权威标志；不支持长模式的机器上 EFER 不存在，不能读
        if (m_hasLongMode && (_Cpu_ReadMsr(kMsrEfer) & kEferLongModeActive) != 0) {
            bool longCode = false;   // 长模式下 L 位表示 64 位代码段，为 0 则是 32 位兼容模式
            bool default32 = false;
            return ReadCodeSegmentFlags(longCode, default32) && longCode ? CpuMode::Long : CpuMode::Protected32;
        }

        // 非长模式：CS 描述符的 D/B 位区分 32 位与 16 位代码段，读不到描述符时按 32 位处理
        bool longCode = false;
        bool default32 = true;
        if (!ReadCodeSegmentFlags(longCode, default32)) return CpuMode::Protected32;
        return default32 ? CpuMode::Protected32 : CpuMode::Protected16;
    }
}
