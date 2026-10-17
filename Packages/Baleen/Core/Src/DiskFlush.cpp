/* DiskFlush.cpp
    写缓存刷新的实现：AHCI 优先，传统 IDE 兜底

    AHCI 路径只用固件已经建好的命令列表里的空闲槽（不改 PxCLB、不启停端口），因此不影响
    BIOS 之后的读盘；传统 IDE 路径先经 PCI 确认控制器在位，再碰固定端口。
    寄存器访问一律经 Platform 的原语：它们是外部函数调用，编译器不会把它优化掉或跨它重排，
    MMIO 与端口访问的顺序因此是稳的
*/

#include <DiskFlush.hpp>
#include <LayoutDescriptor.hpp>

#include <stddef.h>

// 平台原语：8 位与 32 位端口 I/O、绝对地址访问，实现见 Packages/Common/Src/Platform/Cpu.asm
extern "C" void _Cpu_Out8(uint16_t port, uint8_t value);
extern "C" uint8_t _Cpu_In8(uint16_t port);
extern "C" void _Cpu_Out32(uint16_t port, uint32_t value);
extern "C" uint32_t _Cpu_In32(uint16_t port);
extern "C" uint32_t _Cpu_Read32(uintptr_t address);
extern "C" void _Cpu_Write32(uintptr_t address, uint32_t value);

namespace Baleen {
    namespace Core {
        namespace DiskFlush {
            namespace {
                // PCI 配置空间的地址端口与数据端口
                constexpr uint16_t kConfigAddress = 0x0CF8;
                constexpr uint16_t kConfigData = 0x0CFC;
                // PCI 类码：大容量存储；子类：SATA（AHCI）与传统 IDE
                constexpr uint8_t kClassMassStorage = 0x01;
                constexpr uint8_t kSubclassSata = 0x06;
                constexpr uint8_t kSubclassIde = 0x01;
                constexpr uint8_t kInterfaceAhci = 0x01;
                // PCI 配置空间字段偏移
                constexpr uint8_t kPciVendorId = 0x00;
                constexpr uint8_t kPciClassCode = 0x08;
                constexpr uint8_t kPciBar5 = 0x24;
                constexpr uint8_t kPciBar6 = 0x28;
                // BAR 属性位：bit0 为 1 表示 I/O 空间；bit2 为 1 表示 64 位 BAR
                constexpr uint32_t kBarIoSpace = 0x1;
                constexpr uint32_t kBar64Bit = 0x4;
                constexpr uint32_t kBarAddressMask = 0xFFFFFFF0u;
                // Baleen 只扫总线 0 上的设备与功能
                constexpr uint32_t kPciDevices = 32;
                constexpr uint32_t kPciFunctions = 8;
                // AHCI 寄存器偏移，相对 ABAR
                constexpr uint32_t kRegPortsImplemented = 0x0C;
                constexpr uint32_t kPortBase = 0x100;
                constexpr uint32_t kPortStride = 0x80;
                // AHCI 端口寄存器偏移，相对端口基址
                constexpr uint32_t kPortCommandList = 0x00;
                constexpr uint32_t kPortInterruptStatus = 0x10;
                constexpr uint32_t kPortTaskFile = 0x20;
                constexpr uint32_t kPortSignature = 0x24;
                constexpr uint32_t kPortSataStatus = 0x28;
                constexpr uint32_t kPortSataActive = 0x34;
                constexpr uint32_t kPortCommandIssue = 0x38;
                // 命令槽数量与命令头长度
                constexpr uint32_t kCommandSlots = 32;
                constexpr uint32_t kCommandHeaderBytes = 32;
                // H2D 寄存器 FIS 的长度，单位 DWORD
                constexpr uint32_t kFisDwords = 5;
                // 设备存在且通信已建立（PxSSTS.DET = 3）
                constexpr uint32_t kDeviceReady = 0x03;
                // 端口签名：ATA 设备；ATAPI 是 0xEB140101，一律跳过
                constexpr uint32_t kSignatureAta = 0x00000101;
                // 中断状态的任务文件错误位与任务文件寄存器的错误位
                constexpr uint32_t kInterruptTaskFileError = 1u << 30;
                constexpr uint32_t kTaskFileError = 1u << 0;
                // 寄存器写 1 清位
                constexpr uint32_t kClearAll = 0xFFFFFFFFu;
                // 等待完成的上限：AHCI 命令是毫秒级，这个次数足够宽松；超时只报失败，不停机
                constexpr uint32_t kAhciWaitLimit = 20000000;

                // 传统 IDE 的固定端口：主通道与从通道的命令块、控制块
                // 控制块的基址就是替代状态寄存器：读它取状态，写它设设备控制；命令块的偏移习惯不适用
                constexpr uint16_t kPrimaryCommand = 0x01F0;
                constexpr uint16_t kPrimaryAltStatus = 0x03F6;
                constexpr uint16_t kSecondaryCommand = 0x0170;
                constexpr uint16_t kSecondaryAltStatus = 0x0376;
                // 命令块内的寄存器偏移：选盘与状态（写状态寄存器即命令寄存器）
                constexpr uint16_t kRegDriveHead = 6;
                constexpr uint16_t kRegStatus = 7;
                // 状态位：忙、设备故障与错误
                constexpr uint8_t kStatusBusy = 0x80;
                constexpr uint8_t kStatusDeviceFault = 0x20;
                constexpr uint8_t kStatusError = 0x01;
                // ATA 命令：FLUSH CACHE 与 IDENTIFY DEVICE
                constexpr uint8_t kCommandFlushCache = 0xE7;
                constexpr uint8_t kCommandReadDmaExt = 0x25;   // READ DMA EXT：身份核验用
                constexpr uint32_t kCommandTablePrdt = 0x80;   // 命令表里 PRDT 的偏移
                constexpr uint32_t kPrdtEntryBytes = 16;
                constexpr uint32_t kDmaByteLimit = 0x10000;    // 一次 DMA 的字节上限（PRDT 一个条目）
                constexpr uint8_t kCommandIdentify = 0xEC;
                // IDENTIFY DEVICE 返回的字数：512 字节
                constexpr uint32_t kIdentifyWords = 256;
                // 选盘的驱动器/磁头寄存器取值：主盘与从盘
                constexpr uint8_t kDriveMaster = 0xA0;
                constexpr uint8_t kDriveSlave = 0xB0;
                // 传统 IDE 的等待上限：命令是毫秒级，这个次数足够宽松
                constexpr uint32_t kAtaWaitLimit = 2000000;
                // 选盘之后读替代状态的次数：填满 ATA 规范的 400ns 等待
                constexpr uint32_t kSelectDelayReads = 4;

                // 命令表：128 字节对齐；刷新只用 CFIS 那一段，身份核验的读还要一个 PRDT 条目
                // 布局：CFIS 64 + ACMD 32 + 保留 16 + PRDT（每条目 16 字节，从 0x80 起）
                constexpr uint32_t kCommandTableBytes = 256;
                alignas(128) uint8_t s_commandTable[kCommandTableBytes];
                // 身份核验的读回缓冲：与布局描述块等长
                uint8_t s_compare[Baleen::LayoutDescriptor::kBytes];
                // 往字节缓冲写 32 位小端：PRDT 条目按字节拼
                void Put32(uint8_t* target, uint32_t value) {
                    target[0] = static_cast<uint8_t>(value);
                    target[1] = static_cast<uint8_t>(value >> 8);
                    target[2] = static_cast<uint8_t>(value >> 16);
                    target[3] = static_cast<uint8_t>(value >> 24);
                }

                // 两段字节是否逐字节相同
                bool SameBytes(const uint8_t* left, const uint8_t* right, uint32_t bytes) {
                    for (uint32_t i = 0; i < bytes; ++i) if (left[i] != right[i]) return false;
                    return true;
                }


                // 读 PCI 配置空间的 32 位
                uint32_t PciRead32(uint32_t device, uint32_t function, uint8_t offset) {
                    const uint32_t address = 0x80000000u | (device << 11) | (function << 8) | (offset & 0xFC);
                    _Cpu_Out32(kConfigAddress, address);
                    return _Cpu_In32(kConfigData);
                }

                // 扫 PCI：找到指定类与子类的控制器时返回 true
                bool ControllerPresent(uint8_t subclass) {
                    for (uint32_t device = 0; device < kPciDevices; ++device) {
                        for (uint32_t function = 0; function < kPciFunctions; ++function) {
                            if ((PciRead32(device, function, kPciVendorId) & 0xFFFF) == 0xFFFF) continue;
                            const uint32_t classCode = PciRead32(device, function, kPciClassCode);
                            if (((classCode >> 24) & 0xFF) != kClassMassStorage) continue;
                            if (((classCode >> 16) & 0xFF) != subclass) continue;
                            return true;
                        }
                    }
                    return false;
                }

                // —— AHCI ——

                // 端口是否接有可刷新的 ATA 设备且有空闲命令槽；空槽编号由 slot 带回
                bool PortFlushable(uint32_t base, uint32_t& slot) {
                    if ((_Cpu_Read32(base + kPortSataStatus) & 0x0F) != kDeviceReady) return false;
                    if (_Cpu_Read32(base + kPortSignature) != kSignatureAta) return false;
                    const uint32_t issued = _Cpu_Read32(base + kPortCommandIssue);
                    const uint32_t active = _Cpu_Read32(base + kPortSataActive);
                    for (uint32_t candidate = 0; candidate < kCommandSlots; ++candidate) {
                        const uint32_t bit = 1u << candidate;
                        if ((issued & bit) == 0 && (active & bit) == 0) {
                            slot = candidate;
                            return true;
                        }
                    }
                    return false;
                }

                // 在一个 AHCI 端口上下发 FLUSH CACHE 并等待完成；成功返回 true
                bool IssueAhciFlush(uint32_t base, uint32_t slot) {
                    const uint32_t listAddress = _Cpu_Read32(base + kPortCommandList);
                    if (listAddress == 0) return false;

                    // 命令表：H2D 寄存器 FIS，命令寄存器 = FLUSH CACHE（0xE7）；没有数据，PRDT 为空
                    for (uint32_t i = 0; i < sizeof(s_commandTable); ++i) s_commandTable[i] = 0;
                    s_commandTable[0] = 0x27;    // FIS 类型：H2D 寄存器
                    s_commandTable[1] = 0x80;    // C = 1：这是一条命令
                    s_commandTable[2] = kCommandFlushCache;
                    s_commandTable[15] = 0x80;   // 控制字节同样置 C
                    const uint32_t tableAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_commandTable));

                    // 命令头：用固件列表里的空闲槽，不重设 PxCLB
                    volatile uint32_t* header = reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(listAddress + slot * kCommandHeaderBytes));
                    header[0] = kFisDwords << 16;   // CFL = 5；W、P、A 与 PMP 全 0
                    header[1] = 0;                  // PRDTL = 0
                    header[2] = tableAddress;       // CTBA 低 32 位
                    header[3] = 0;                  // CTBA 高 32 位

                    // 清旧的完成位，再置本位发起命令，然后等该位清零
                    _Cpu_Write32(base + kPortInterruptStatus, kClearAll);
                    _Cpu_Write32(base + kPortCommandIssue, 1u << slot);
                    uint32_t waited = 0;
                    while ((_Cpu_Read32(base + kPortCommandIssue) & (1u << slot)) != 0) {
                        if (++waited >= kAhciWaitLimit) return false;
                    }
                    const uint32_t status = _Cpu_Read32(base + kPortInterruptStatus);
                    const uint32_t taskFile = _Cpu_Read32(base + kPortTaskFile);
                    _Cpu_Write32(base + kPortInterruptStatus, status);
                    if ((status & kInterruptTaskFileError) != 0) return false;
                    if ((taskFile & kTaskFileError) != 0) return false;
                    return true;
                }

                // 经 AHCI 读一段介质：READ DMA EXT + 一个 PRDT 条目，轮询等待，超时有界
                // 只读一个空闲槽；此阶段没有分页，缓冲的线性地址就是 DMA 要的物理地址
                bool IssueAhciRead(uint32_t base, uint32_t slot, uint64_t byteOffset, uint8_t* buffer, uint32_t bytes) {
                    if (bytes == 0 || bytes > kDmaByteLimit) return false;
                    const uint32_t listAddress = _Cpu_Read32(base + kPortCommandList);
                    if (listAddress == 0) return false;
                    const uint64_t lba = byteOffset / 512;
                    if ((byteOffset % 512) != 0 || (bytes % 512) != 0) return false;

                    for (uint32_t i = 0; i < sizeof(s_commandTable); ++i) s_commandTable[i] = 0;
                    // H2D 寄存器 FIS（20 字节）：READ DMA EXT，LBA48，扇区数按 512 字节计
                    // 字段顺序按 ATA 规范：命令、特征低、LBA 7:0 / 15:8 / 23:16、设备、LBA 31:24 / 39:32 / 47:40、
                    // 特征高、扇区数低 / 高、ICC、控制——设备寄存器在 byte 7，LBA 的第 4 个字节在 byte 8
                    const uint32_t sectors = bytes / 512;
                    s_commandTable[0] = 0x27;                                  // FIS 类型：H2D 寄存器
                    s_commandTable[1] = 0x80;                                  // C = 1：这是一条命令
                    s_commandTable[2] = kCommandReadDmaExt;                    // 命令
                    s_commandTable[3] = 0;                                     // 特征低
                    s_commandTable[4] = static_cast<uint8_t>(lba);             // LBA 7:0
                    s_commandTable[5] = static_cast<uint8_t>(lba >> 8);        // LBA 15:8
                    s_commandTable[6] = static_cast<uint8_t>(lba >> 16);       // LBA 23:16
                    s_commandTable[7] = 0x40;                                  // 设备寄存器：LBA 模式
                    s_commandTable[8] = static_cast<uint8_t>(lba >> 24);       // LBA 31:24
                    s_commandTable[9] = static_cast<uint8_t>(lba >> 32);       // LBA 39:32
                    s_commandTable[10] = static_cast<uint8_t>(lba >> 40);      // LBA 47:40
                    s_commandTable[11] = 0;                                    // 特征高
                    s_commandTable[12] = static_cast<uint8_t>(sectors);        // 扇区数低
                    s_commandTable[13] = static_cast<uint8_t>(sectors >> 8);   // 扇区数高
                    s_commandTable[14] = 0;                                    // ICC
                    s_commandTable[15] = 0x80;                                 // 控制
                    // PRDT：一个条目，DBC 是"字节数 - 1"，最高位要求完成后置中断
                    const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer));
                    Put32(s_commandTable + kCommandTablePrdt + 0, address);
                    Put32(s_commandTable + kCommandTablePrdt + 4, 0);
                    Put32(s_commandTable + kCommandTablePrdt + 8, 0);
                    Put32(s_commandTable + kCommandTablePrdt + 12, (bytes - 1) | 0x80000000u);

                    const uint32_t tableAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_commandTable));
                    volatile uint32_t* header = reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(listAddress + slot * kCommandHeaderBytes));
                    header[0] = kFisDwords << 16;   // CFL = 5；W = 0 表示设备→主机
                    header[1] = 1;                  // PRDTL = 1
                    header[2] = tableAddress;
                    header[3] = 0;

                    _Cpu_Write32(base + kPortInterruptStatus, kClearAll);
                    _Cpu_Write32(base + kPortCommandIssue, 1u << slot);
                    uint32_t waited = 0;
                    while ((_Cpu_Read32(base + kPortCommandIssue) & (1u << slot)) != 0) {
                        if (++waited >= kAhciWaitLimit) return false;
                    }
                    const uint32_t status = _Cpu_Read32(base + kPortInterruptStatus);
                    const uint32_t taskFile = _Cpu_Read32(base + kPortTaskFile);
                    _Cpu_Write32(base + kPortInterruptStatus, status);
                    if ((status & kInterruptTaskFileError) != 0) return false;
                    if ((taskFile & kTaskFileError) != 0) return false;
                    return true;
                }

                // 扫全部 AHCI 控制器：issue 为假时只探测，不下发命令
                Result ScanAhci(bool issue) {
                    bool anyPort = false;
                    bool flushed = false;
                    for (uint32_t device = 0; device < kPciDevices; ++device) {
                        for (uint32_t function = 0; function < kPciFunctions; ++function) {
                            if ((PciRead32(device, function, kPciVendorId) & 0xFFFF) == 0xFFFF) continue;
                            const uint32_t classCode = PciRead32(device, function, kPciClassCode);
                            if (((classCode >> 24) & 0xFF) != kClassMassStorage) continue;
                            if (((classCode >> 16) & 0xFF) != kSubclassSata) continue;
                            if (((classCode >> 8) & 0xFF) != kInterfaceAhci) continue;

                            const uint32_t bar = PciRead32(device, function, kPciBar5);
                            if ((bar & kBarIoSpace) != 0) continue;
                            // ABAR 落在 4GiB 以上的平台本阶段不处理：没有可刷新的端口，按不支持返回
                            if ((bar & kBar64Bit) != 0 && PciRead32(device, function, kPciBar6) != 0) continue;
                            const uint32_t abar = bar & kBarAddressMask;
                            if (abar == 0) continue;

                            const uint32_t implemented = _Cpu_Read32(abar + kRegPortsImplemented);
                            for (uint32_t port = 0; port < kCommandSlots; ++port) {
                                if ((implemented & (1u << port)) == 0) continue;
                                const uint32_t base = abar + kPortBase + port * kPortStride;
                                uint32_t slot = 0;
                                if (!PortFlushable(base, slot)) continue;
                                anyPort = true;
                                if (!issue) continue;
                                if (IssueAhciFlush(base, slot)) flushed = true;
                            }
                        }
                    }
                    if (flushed) return Result::Succeeded;
                    if (anyPort && issue) return Result::Failed;
                    if (anyPort) return Result::Succeeded;
                    return Result::Unsupported;
                }

                // —— 传统 IDE ——

                // 等到 BSY 清零；超时返回 false
                bool WaitNotBusy(uint16_t altStatus) {
                    for (uint32_t i = 0; i < kAtaWaitLimit; ++i) {
                        if ((_Cpu_In8(altStatus) & kStatusBusy) == 0) return true;
                    }
                    return false;
                }

                // 选中一个盘并读状态：0xFF 表示总线悬空、0x00 表示没有设备，两者都按不在位处理
                // 选盘之后连读替代状态，填满 ATA 规范要求的状态更新等待
                bool DrivePresent(uint16_t command, uint16_t altStatus, uint8_t driveHead) {
                    if (!WaitNotBusy(altStatus)) return false;
                    _Cpu_Out8(command + kRegDriveHead, driveHead);
                    for (uint32_t i = 0; i < kSelectDelayReads; ++i) (void)_Cpu_In8(altStatus);
                    const uint8_t status = _Cpu_In8(command + kRegStatus);
                    return status != 0xFF && status != 0x00;
                }

                // 是否是一块 ATA 盘：IDENTIFY DEVICE（0xEC）只有 ATA 设备认，ATAPI 以 ERR 拒绝
                // 用命令甄别而不是读签名寄存器：签名会被上一条命令的寄存器内容盖掉，命令不会
                bool AtaDevice(uint16_t command, uint16_t altStatus, uint8_t driveHead) {
                    if (!DrivePresent(command, altStatus, driveHead)) return false;
                    _Cpu_Out8(command + 2, 0);   // 扇区计数：IDENTIFY 取 0
                    _Cpu_Out8(command + 3, 0);
                    _Cpu_Out8(command + 4, 0);
                    _Cpu_Out8(command + 5, 0);
                    _Cpu_Out8(command + kRegStatus, kCommandIdentify);
                    if (!WaitNotBusy(altStatus)) return false;
                    const uint8_t status = _Cpu_In8(command + kRegStatus);
                    if ((status & (kStatusError | kStatusDeviceFault)) != 0) return false;
                    // 读走 512 字节的结果把 DRQ 清掉：数据口是 16 位，按 8 位读两下取一个字
                    for (uint32_t i = 0; i < kIdentifyWords * 2; ++i) (void)_Cpu_In8(command);
                    return true;
                }

                // 在一个通道的一个 ATA 盘上下发 FLUSH CACHE；成功返回 true
                bool IssueAtaFlush(uint16_t command, uint16_t altStatus, uint8_t driveHead) {
                    if (!AtaDevice(command, altStatus, driveHead)) return false;
                    _Cpu_Out8(command + kRegStatus, kCommandFlushCache);
                    if (!WaitNotBusy(altStatus)) return false;
                    return (_Cpu_In8(command + kRegStatus) & (kStatusError | kStatusDeviceFault)) == 0;
                }

                // 扫传统 IDE：只有在 PCI 上确认存在 IDE 控制器时才碰固定端口
                Result ScanLegacy(bool issue) {
                    if (!ControllerPresent(kSubclassIde)) return Result::Unsupported;
                    const uint16_t commands[2] = { kPrimaryCommand, kSecondaryCommand };
                    const uint16_t altStatuses[2] = { kPrimaryAltStatus, kSecondaryAltStatus };
                    const uint8_t drives[2] = { kDriveMaster, kDriveSlave };
                    bool anyDrive = false;
                    bool flushed = false;
                    for (uint32_t channel = 0; channel < 2; ++channel) {
                        for (uint32_t index = 0; index < 2; ++index) {
                            // 探测与刷新用同一条甄别：ATAPI（光盘）不参与，否则会把没有刷新语义的设备算进来
                            if (!AtaDevice(commands[channel], altStatuses[channel], drives[index])) continue;
                            anyDrive = true;
                            if (!issue) continue;
                            if (IssueAtaFlush(commands[channel], altStatuses[channel], drives[index])) flushed = true;
                        }
                    }
                    if (flushed) return Result::Succeeded;
                    if (anyDrive && issue) return Result::Failed;
                    if (anyDrive) return Result::Succeeded;
                    return Result::Unsupported;
                }
            }

            const char* PathText(Path path) {
                switch (path) {
                    case Path::Ahci: return "AHCI";
                    case Path::LegacyAta: return "legacy ATA";
                    case Path::None: return "none";
                }
                return "none";
            }

            const char* ResultText(Result result) {
                switch (result) {
                    case Result::Unsupported: return "no flushable port";
                    case Result::Succeeded: return "flushed";
                    case Result::Failed: return "the flush command failed";
                }
                return "unknown";
            }

            Path Probe() {
                if (ScanAhci(false) != Result::Unsupported) return Path::Ahci;
                if (ScanLegacy(false) != Result::Unsupported) return Path::LegacyAta;
                return Path::None;
            }

            bool CoversMedium(uint64_t byteOffset, const uint8_t* expected, uint32_t bytes) {
                if (expected == nullptr || bytes == 0 || bytes > sizeof(s_compare)) return false;
                // 只走 AHCI：传统 IDE 路径没有做身份核验的机制，按"未覆盖"返回（保守方向，绝不谎报覆盖）
                for (uint32_t device = 0; device < kPciDevices; ++device) {
                    for (uint32_t function = 0; function < kPciFunctions; ++function) {
                        if ((PciRead32(device, function, kPciVendorId) & 0xFFFF) == 0xFFFF) continue;
                        const uint32_t classCode = PciRead32(device, function, kPciClassCode);
                        if (((classCode >> 24) & 0xFF) != kClassMassStorage) continue;
                        if (((classCode >> 16) & 0xFF) != kSubclassSata) continue;
                        if (((classCode >> 8) & 0xFF) != kInterfaceAhci) continue;
                        const uint32_t bar = PciRead32(device, function, kPciBar5);
                        if ((bar & kBarIoSpace) != 0) continue;
                        if ((bar & kBar64Bit) != 0 && PciRead32(device, function, kPciBar6) != 0) continue;
                        const uint32_t abar = bar & kBarAddressMask;
                        if (abar == 0) continue;
                        const uint32_t implemented = _Cpu_Read32(abar + kRegPortsImplemented);
                        for (uint32_t port = 0; port < kCommandSlots; ++port) {
                            if ((implemented & (1u << port)) == 0) continue;
                            const uint32_t base = abar + kPortBase + port * kPortStride;
                            uint32_t slot = 0;
                            if (!PortFlushable(base, slot)) continue;
                            for (uint32_t i = 0; i < bytes; ++i) s_compare[i] = 0;
                            if (!IssueAhciRead(base, slot, byteOffset, s_compare, bytes)) continue;
                            // 读得回来且逐字节相同：这个端口上的设备就是启动介质
                            if (SameBytes(s_compare, expected, bytes)) return true;
                        }
                    }
                }
                return false;
            }

            Result Flush() {
                // AHCI 优先：现代平台与 q35 默认路径都走它；失败也不再退到传统 IDE（两者不会并存于同一块盘）
                const Result ahci = ScanAhci(true);
                if (ahci != Result::Unsupported) return ahci;
                return ScanLegacy(true);
            }
        }
    }
}
