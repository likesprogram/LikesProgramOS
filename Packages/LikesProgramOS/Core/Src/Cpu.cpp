/* Cpu.cpp
    内核侧 CPU 事实探测的实现：CPUID 能力位与厂商标识串

    只读探测：不改控制寄存器、不开任何特性位；功能位只作事实记录，
    供诊断与后续阶段按需使用
*/

#include <Cpu.hpp>

namespace Kernel {
    namespace {
        // CPUID 的结果：四个 32 位寄存器
        struct CpuidResult {
            uint32_t eax;   // 主返回寄存器
            uint32_t ebx;   // 厂商串的前 4 字节等
            uint32_t ecx;   // 厂商串的后 4 字节等
            uint32_t edx;   // 能力位等
        };

        // 执行 CPUID；调用前须确认处理器支持它
        CpuidResult Cpuid(uint32_t leaf, uint32_t subleaf) {
            CpuidResult result{};
            asm volatile("CPUID" : "=a"(result.eax), "=b"(result.ebx), "=c"(result.ecx), "=d"(result.edx) : "a"(leaf), "c"(subleaf));
            return result;
        }

        // 探测是否支持 CPUID：翻转 EFLAGS 的 ID 位（bit 21）并回读，失败时恢复原值
        bool SupportsCpuid() {
            uint64_t before = 0;
            asm volatile("PUSHFQ; POP %0" : "=r"(before));
            const uint64_t flipped = before ^ (1ull << 21);
            asm volatile("PUSH %0; POPFQ" : : "r"(flipped));
            uint64_t readback = 0;
            asm volatile("PUSHFQ; POP %0" : "=r"(readback));
            asm volatile("PUSH %0; POPFQ" : : "r"(before));
            // 读回值与写入值的 ID 位不同才算该位可写；比较两位而不是直接看读回值，
            // 初始 ID 位为 1 的环境下判定同样成立
            return ((readback ^ before) & (1ull << 21)) != 0;
        }
    }

    bool Cpu::Install() {
        m_hasCpuid = SupportsCpuid();
        if (!m_hasCpuid) return false;

        // 厂商串按 EBX、EDX、ECX 的字节顺序拼出，即处理器实际写出的 12 个字符
        const CpuidResult basic = Cpuid(0, 0);
        const uint32_t vendorWords[3] = { basic.ebx, basic.edx, basic.ecx };
        for (uint32_t word = 0; word < 3; ++word) {
            for (uint32_t byte = 0; byte < 4; ++byte) {
                m_vendor[word * 4 + byte] = static_cast<char>((vendorWords[word] >> (8 * byte)) & 0xFF);
            }
        }

        if (basic.eax >= 1) {
            const CpuidResult features = Cpuid(1, 0);
            m_hasTsc = (features.edx & (1u << 4)) != 0;
            m_hasApic = (features.edx & (1u << 9)) != 0;
        }
        const CpuidResult extended = Cpuid(0x80000000u, 0);
        if (extended.eax >= 0x80000001u) {
            const CpuidResult extendedFeatures = Cpuid(0x80000001u, 0);
            m_hasLongMode = (extendedFeatures.edx & (1u << 29)) != 0;
            m_hasNx = (extendedFeatures.edx & (1u << 20)) != 0;
        }
        return true;
    }
}
