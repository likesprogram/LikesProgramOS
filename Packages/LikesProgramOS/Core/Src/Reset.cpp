/* Reset.cpp
    等待与平台复位的实现：PIT 通道 2 标定 TSC、按 TSC 计时、有界忙等退避，
    ACPI RSDP → RSDT / XSDT → FADT 的 reset register 查找与写入门

    物理地址一律按低 4GiB 恒等映射直接读：引导器与内核的引导页表都保留这段恒等映射，
    FATAL 路径在换页表前后都能用同一套写法；所有表长度与校验先核对再访问，读不到就按
    “没有该路径”处理，不拿缺页换复位
*/

#include <Reset.hpp>

namespace Kernel {
    namespace {
        // PIT：1.193182 MHz；通道 2 由端口 0x61 的 bit0 门控，OUT 状态在 bit5
        constexpr uint16_t kPitChannel2 = 0x42;
        constexpr uint16_t kPitCommand = 0x43;
        constexpr uint16_t kPitPort61 = 0x61;
        constexpr uint32_t kPitHz = 1193182;
        // 标定时长：50 ms（一-shot 上限 65535 tick 约 54.9 ms，留余量）
        constexpr uint32_t kCalibrationMs = 50;
        // 一-shot 的轮询上限：真机与模拟器上都在几十次内结束，超限按 PIT 不可用处理
        constexpr uint32_t kPitPollLimit = 0x1000000;
        // TSC 标定结果：每毫秒的 TSC 数；0 与 false 表示时基不可用（静态零初始化，不进 .init_array）
        uint64_t s_ticksPerMs = 0;
        bool s_calibrated = false;
        // 0xCF9 复位控制寄存器
        constexpr uint16_t kResetControlPort = 0xCF9;
        constexpr uint8_t kResetSystemBit = 0x02;
        constexpr uint8_t kResetCpuBit = 0x04;
        // 有界忙等的上限：时基不可用时按毫秒数换算迭代次数，总迭代数封顶
        constexpr uint64_t kBusyIterationsPerMs = 20000;
        constexpr uint64_t kBusyIterationLimit = 200000000;
        // 低 4GiB 恒等映射的上界
        constexpr uint64_t kIdentityLimit = 0x100000000ull;
        // ACPI：RSDP 搜索范围与字段偏移
        constexpr uint16_t kBdaEbdaSegment = 0x40E;      // BDA 里的 EBDA 段地址
        constexpr uint32_t kEbdaScanBytes = 1024;        // EBDA 前 1 KiB
        constexpr uint32_t kRsdpAreaBegin = 0xE0000;     // BIOS ROM 区的 RSDP 搜索范围
        constexpr uint32_t kRsdpAreaEnd = 0x100000;
        constexpr uint32_t kRsdpChecksumBytesV0 = 20;    // ACPI 1.0 的 RSDP 校验范围
        constexpr uint8_t kRsdpRevisionOffset = 0x0F;    // 修订号：2 起有 XSDT
        constexpr uint32_t kRsdpRsdtOffset = 0x10;       // RSDT 物理地址（32 位）
        constexpr uint32_t kRsdpXsdtOffset = 0x18;       // XSDT 物理地址（64 位）
        constexpr uint32_t kRsdpXsdtChecksumBytes = 36;  // ACPI 2.0 起的 RSDP 校验范围
        // ACPI 表头与 FADT 字段偏移
        constexpr uint32_t kSdtLengthOffset = 0x04;      // 表长度
        constexpr uint32_t kSdtEntriesOffset = 0x24;     // RSDT / XSDT 的条目起点
        constexpr uint32_t kFadtResetRegisterOffset = 0x74;   // reset register 的 GAS
        constexpr uint32_t kFadtResetValueOffset = 0x80;      // reset value
        constexpr uint32_t kFadtMinBytes = 0x81;         // 至少覆盖到 reset value
        // 地址空间编号：0 系统内存、1 系统 I/O
        constexpr uint8_t kAddressSpaceMemory = 0;
        constexpr uint8_t kAddressSpaceIo = 1;

        // 写 8 位 I/O 端口
        void Out8(uint16_t port, uint8_t value) {
            asm volatile("OUTB %0, %1" : : "a"(value), "Nd"(port) : "memory");
        }

        // 读 8 位 I/O 端口
        uint8_t In8(uint16_t port) {
            uint8_t value;
            asm volatile("INB %1, %0" : "=a"(value) : "Nd"(port) : "memory");
            return value;
        }

        // 读时间戳计数器
        uint64_t ReadTsc() {
            uint32_t low = 0;
            uint32_t high = 0;
            asm volatile("RDTSC" : "=a"(low), "=d"(high));
            return (static_cast<uint64_t>(high) << 32) | low;
        }

        // 让出管线：忙等与轮询里降低空转开销
        void Pause() {
            asm volatile("PAUSE");
        }

        // 物理地址处的只读字节指针：只用于已核对在低 4GiB 内的地址
        // 经不透明寄存器阻断编译器的常量地址推断：直接按小常量地址下标会触发越界告警
        const volatile uint8_t* PhysicalBytes(uint64_t physical) {
            const volatile uint8_t* pointer = reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(physical));
            asm volatile("" : "+r"(pointer));
            return pointer;
        }

        // 一字节校验和为零：ACPI 表与 RSDP 都靠它判结构可信
        bool ChecksumValid(const volatile uint8_t* table, uint32_t bytes) {
            uint8_t sum = 0;
            for (uint32_t i = 0; i < bytes; ++i) sum = static_cast<uint8_t>(sum + table[i]);
            return sum == 0;
        }

        // 比较表签名：不依赖结尾零
        bool SignatureMatches(const volatile uint8_t* table, const char* signature, uint32_t bytes) {
            for (uint32_t i = 0; i < bytes; ++i) if (table[i] != static_cast<uint8_t>(signature[i])) return false;
            return true;
        }

        // 按小端读 32 位 / 64 位字段：表在物理内存里，逐字节读避免未对齐访问
        uint32_t Read32(const volatile uint8_t* field) {
            uint32_t value = 0;
            for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
            return value;
        }

        uint64_t Read64(const volatile uint8_t* field) {
            return static_cast<uint64_t>(Read32(field)) | (static_cast<uint64_t>(Read32(field + 4)) << 32);
        }

        // 一次 PIT 通道 2 一-shot：门控拉低 → 装载计数 → 门控拉高 → 轮询 OUT 变高
        // 轮询次数有上限；超限或参数为零返回 false，不把调用方挂死在等硬件上
        bool ProgramPitSlice(uint32_t ticks) {
            if (ticks == 0 || ticks > 0xFFFF) return false;
            const uint8_t control = In8(kPitPort61);
            // 门控拉低：停止计数并让 OUT 保持低；扬声器数据位保持原样
            Out8(kPitPort61, static_cast<uint8_t>(control & ~0x01));
            Out8(kPitCommand, 0xB0);   // 通道 2、先低后高、模式 0（计数到零 OUT 变高）、二进制
            Out8(kPitChannel2, static_cast<uint8_t>(ticks & 0xFF));
            Out8(kPitChannel2, static_cast<uint8_t>(ticks >> 8));
            // 门控拉高：计数开始
            Out8(kPitPort61, static_cast<uint8_t>((control & ~0x01) | 0x01));
            for (uint32_t poll = 0; poll < kPitPollLimit; ++poll) {
                if ((In8(kPitPort61) & 0x20) != 0) return true;
                Pause();
            }
            return false;
        }

        // 用 PIT 标定 TSC：返回是否得到可用的每毫秒计数
        bool CalibrateTsc() {
            if (s_calibrated) return s_ticksPerMs != 0;
            s_calibrated = true;
            const uint32_t ticks = static_cast<uint32_t>((static_cast<uint64_t>(kPitHz) * kCalibrationMs) / 1000);
            const uint64_t begin = ReadTsc();
            if (!ProgramPitSlice(ticks)) return false;
            const uint64_t end = ReadTsc();
            if (end <= begin) return false;
            s_ticksPerMs = (end - begin) / kCalibrationMs;
            return s_ticksPerMs != 0;
        }

        // 有界忙等：时基不可用时的退避，迭代次数与毫秒数成正比且封顶
        void BusyFallback(uint32_t milliseconds) {
            uint64_t iterations = static_cast<uint64_t>(milliseconds) * kBusyIterationsPerMs;
            if (iterations > kBusyIterationLimit) iterations = kBusyIterationLimit;
            for (uint64_t i = 0; i < iterations; ++i) Pause();
        }

        // ACPI 表的 FADT reset register：地址、地址空间与写入值
        struct ResetRegister {
            uint64_t address = 0;   // GAS 地址
            uint8_t space = 0;      // 0 系统内存、1 系统 I/O
            uint8_t value = 0;      // 写入值
        };

        // 在表头里核对长度并返回可读的物理表：长度为零、越出低 4GiB、校验和不符都返回 0
        // 只按低 4GiB 恒等映射访问：UEFI 上少数固件把 XSDT / FADT 放在 4GiB 以上，这类表按
        // “该路径不可用”处理，复位仍有 0xCF9 主路径，不拿缺页换复位
        uint32_t TableBytes(uint64_t physical) {
            if (physical == 0 || physical >= kIdentityLimit) return 0;
            if (kIdentityLimit - physical < kSdtEntriesOffset) return 0;
            const volatile uint8_t* const table = PhysicalBytes(physical);
            const uint32_t bytes = Read32(table + kSdtLengthOffset);
            if (bytes < kSdtEntriesOffset || bytes > kIdentityLimit - physical) return 0;
            if (!ChecksumValid(table, bytes)) return 0;
            return bytes;
        }

        // 从 RSDT / XSDT 里找 FADT 并解出 reset register；没有可用表返回 false
        bool FindFadtReset(uint64_t root, bool xsdt, ResetRegister& reset) {
            const uint32_t rootBytes = TableBytes(root);
            if (rootBytes < kSdtEntriesOffset) return false;
            const volatile uint8_t* const table = PhysicalBytes(root);
            const uint32_t entryBytes = xsdt ? 8 : 4;
            const uint32_t count = (rootBytes - kSdtEntriesOffset) / entryBytes;
            for (uint32_t index = 0; index < count; ++index) {
                const uint64_t entry = xsdt ? Read64(table + kSdtEntriesOffset + index * entryBytes) : Read32(table + kSdtEntriesOffset + index * entryBytes);
                const uint32_t fadtBytes = TableBytes(entry);
                if (fadtBytes < kFadtMinBytes) continue;
                const volatile uint8_t* const fadt = PhysicalBytes(entry);
                if (!SignatureMatches(fadt, "FACP", 4)) continue;
                // reset register 是 12 字节的 GAS：地址空间、位宽、位偏移、访问长度、64 位地址
                const volatile uint8_t* const gas = fadt + kFadtResetRegisterOffset;
                const uint8_t space = gas[0];
                const uint8_t bitWidth = gas[1];
                const uint8_t accessBytes = gas[3];
                const uint64_t address = Read64(gas + 4);
                if (bitWidth != 8 || accessBytes != 1 || address == 0) return false;
                if (space != kAddressSpaceMemory && space != kAddressSpaceIo) return false;
                if (space == kAddressSpaceMemory && address >= kIdentityLimit) return false;
                if (space == kAddressSpaceIo && address > 0xFFFF) return false;
                reset.address = address;
                reset.space = space;
                reset.value = fadt[kFadtResetValueOffset];
                return true;
            }
            return false;
        }

        // 在给定物理范围内按 16 字节对齐找 RSDP：签名与校验和都要成立
        bool FindRsdp(uint64_t begin, uint64_t bytes, uint64_t& rsdp) {
            if (bytes < 20 || begin >= kIdentityLimit || bytes > kIdentityLimit - begin) return false;
            const volatile uint8_t* const area = PhysicalBytes(begin);
            for (uint64_t offset = 0; offset + 20 <= bytes; offset += 16) {
                const volatile uint8_t* const candidate = area + offset;
                if (!SignatureMatches(candidate, "RSD PTR ", 8)) continue;
                const uint8_t revision = candidate[kRsdpRevisionOffset];
                const uint32_t checksumBytes = revision >= 2 ? kRsdpXsdtChecksumBytes : kRsdpChecksumBytesV0;
                if (offset + checksumBytes > bytes) continue;
                if (!ChecksumValid(candidate, checksumBytes)) continue;
                rsdp = begin + offset;
                return true;
            }
            return false;
        }

        // 按 ACPI 路径复位：找 RSDP（先 EBDA 后 BIOS ROM 区）→ RSDT / XSDT → FADT → 写 reset register
        // 返回 true 表示复位命令已下发（正常情况下不再返回），false 表示本路径不可用
        bool ResetThroughAcpi() {
            uint64_t rsdp = 0;
            // EBDA 段地址在 BDA 的 0x40E：段地址为零表示没有 EBDA
            const volatile uint8_t* const ebdaSegmentBytes = PhysicalBytes(0x40E);
            const uint64_t ebda = static_cast<uint64_t>(static_cast<uint16_t>(ebdaSegmentBytes[0] | (ebdaSegmentBytes[1] << 8))) << 4;
            if (ebda != 0 && ebda < kIdentityLimit && FindRsdp(ebda, kEbdaScanBytes, rsdp)) {
                // 已找到
            } else if (!FindRsdp(kRsdpAreaBegin, kRsdpAreaEnd - kRsdpAreaBegin, rsdp)) {
                return false;
            }
            const volatile uint8_t* const rsdpBytes = PhysicalBytes(rsdp);
            const uint8_t revision = rsdpBytes[kRsdpRevisionOffset];
            uint64_t root = 0;
            bool xsdt = false;
            if (revision >= 2) {
                root = Read64(rsdpBytes + kRsdpXsdtOffset);
                xsdt = root != 0;
            }
            if (!xsdt) root = Read32(rsdpBytes + kRsdpRsdtOffset);
            if (root == 0) return false;
            ResetRegister reset{};
            if (!FindFadtReset(root, xsdt, reset)) return false;
            if (reset.space == kAddressSpaceIo) {
                Out8(static_cast<uint16_t>(reset.address), reset.value);
            } else {
                *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(reset.address)) = reset.value;
            }
            return true;
        }
    }

    namespace {
        // 按时间源等待：返回是否按时基等待完成
        bool WaitWithTsc(uint32_t milliseconds) {
            if (milliseconds == 0) return true;
            if (!CalibrateTsc()) {
                BusyFallback(milliseconds);
                return false;
            }
            uint32_t remaining = milliseconds;
            while (remaining > 0) {
                // 分片等待：每片 1000 ms 以内，乘法不会把本次的 TSC 差值推出 64 位
                const uint32_t slice = remaining > 1000 ? 1000 : remaining;
                remaining -= slice;
                const uint64_t delta = s_ticksPerMs * slice;
                const uint64_t begin = ReadTsc();
                while (ReadTsc() - begin < delta) Pause();
            }
            return true;
        }

        // 走 0xCF9 与 ACPI 两条复位路径：能返回说明机器没有被复位
        bool RequestPlatformReset() {
            // 主路径：0xCF9 先 SYS_RST 再 RST_CPU|SYS_RST；两次写之间给芯片组一个读端口的时间
            Out8(kResetControlPort, kResetSystemBit);
            for (uint32_t spin = 0; spin < 1000; ++spin) Pause();
            Out8(kResetControlPort, static_cast<uint8_t>(kResetCpuBit | kResetSystemBit));
            for (uint32_t spin = 0; spin < 1000; ++spin) Pause();
            // 辅路径：ACPI FADT 的 reset register
            ResetThroughAcpi();
            // 给复位一点生效时间；能走到这里说明机器没有被复位
            WaitWithTsc(20);
            return false;
        }
    }

    // TSC 每毫秒计数：首次调用现场按 PIT 标定一次，失败返回 0
    // 标定只有这一份实现（上面的 CalibrateTsc）：Core.Time 槽 1 经 _Kernel_TicksPerMs 取，
    // 不在别处再标定一遍，也不改标定参数
    uint64_t TicksPerMsCalibrated() {
        if (!CalibrateTsc()) return 0;
        return s_ticksPerMs;
    }
}

extern "C" bool _Kernel_WaitMs(uint32_t milliseconds) { return Kernel::WaitWithTsc(milliseconds); }

extern "C" uint64_t _Kernel_TicksPerMs() { return Kernel::TicksPerMsCalibrated(); }

extern "C" bool _Kernel_Reset() { return Kernel::RequestPlatformReset(); }
