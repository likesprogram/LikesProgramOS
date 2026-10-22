/* Ahci.cpp
    AHCI 读路径的实现：端口初始化、命令列表与 PRDT、IDENTIFY、READ DMA EXT、超时与端口复位

    MMIO 一律经 volatile 指针按 32 位访问，读改写之间不会被编译器折叠或重排
    命令完成用 PxCI 位轮询：软件置位、HBA 完成后清零；同时看 PxIS.TFES 与 PxTFD.ERR
    超时先尝试端口复位（COMRESET 序列），复位后再发一次；仍失败按 I/O 错误返回
    命令头 DW0/DW1 与 LBA/扇区换算复用 Common/Storage/AhciCommand.hpp（Baleen 刷盘路径同一份）
    数据经模块申请的一页弹跳缓冲搬运，单条命令的传输量不超过一页
*/

#include "Ahci.hpp"

#include <Libc.hpp>

#include <Storage/AhciCommand.hpp>

namespace Ahci {
    namespace {
        // 通用寄存器偏移
        constexpr uint32_t kRegGhc = 0x04;
        constexpr uint32_t kRegPortsImplemented = 0x0C;
        // GHC 位：AHCI 使能、复位
        constexpr uint32_t kGhcAhciEnable = 1u << 31;
        // 端口寄存器基址与步长
        constexpr uint32_t kRegPortBase = 0x100;
        constexpr uint32_t kRegPortStride = 0x80;
        // 端口寄存器偏移
        constexpr uint32_t kPortClb = 0x00;
        constexpr uint32_t kPortClbu = 0x04;
        constexpr uint32_t kPortFb = 0x08;
        constexpr uint32_t kPortFbu = 0x0C;
        constexpr uint32_t kPortInterruptStatus = 0x10;
        constexpr uint32_t kPortInterruptEnable = 0x14;
        constexpr uint32_t kPortCommand = 0x18;
        constexpr uint32_t kPortTaskFile = 0x20;
        constexpr uint32_t kPortSignature = 0x24;
        constexpr uint32_t kPortSataStatus = 0x28;
        constexpr uint32_t kPortSataControl = 0x2C;
        constexpr uint32_t kPortSataError = 0x30;
        constexpr uint32_t kPortCommandIssue = 0x38;
        // PxCMD 位
        constexpr uint32_t kCmdStart = 1u << 0;
        constexpr uint32_t kCmdFisReceiveEnable = 1u << 4;
        constexpr uint32_t kCmdFisReceiveRunning = 1u << 14;
        constexpr uint32_t kCmdCommandRunning = 1u << 15;
        // PxSSTS.DET 与设备签名
        constexpr uint32_t kDetMask = 0x0F;
        constexpr uint32_t kDetPresent = 0x03;
        constexpr uint32_t kSignatureAta = 0x00000101;
        // PxIS 的任务文件错误位与 PxTFD 的错误位
        constexpr uint32_t kInterruptTaskFileError = 1u << 30;
        constexpr uint32_t kTaskFileError = 0x01;
        // 命令表与命令头布局：CFIS 64 + ACMD 32 + 保留到 0x80，PRDT 从 0x80 起
        constexpr uint32_t kCommandHeaderBytes = 32;
        constexpr uint32_t kCommandTableBytes = 256;
        constexpr uint32_t kCommandTablePrdtOffset = 0x80;
        // PRDT 的 DBC 最高位：完成后置中断
        constexpr uint32_t kPrdtInterrupt = 1u << 31;
        // H2D 寄存器 FIS 长度：20 字节，命令表里按 64 字节对齐书写
        constexpr uint32_t kFisBytes = 64;
        // 轮询上限：命令与复位都是毫秒级，次数在实机与 QEMU 上都足够宽松
        constexpr uint32_t kPollLimit = 20000000;
        constexpr uint32_t kResetPollLimit = 2000000;
        // 端口复位的等待空转：没有时基，用有界空转近似 COMRESET 的保持时间
        constexpr uint32_t kResetSpin = 200000;
        // 命令：IDENTIFY DEVICE 与 READ DMA EXT
        constexpr uint8_t kCommandIdentify = 0xEC;
        constexpr uint8_t kCommandReadDmaExt = 0x25;
        // IDENTIFY 数据长度：一个逻辑扇区
        constexpr uint32_t kIdentifyBytes = 512;
        // IDENTIFY 返回 256 个 16 位字
        constexpr uint32_t kIdentifyWords = 256;

        // 按字节偏移访问 MMIO 的 32 位寄存器
        volatile uint32_t* At32(volatile uint32_t* base, uint32_t offset) {
            return reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base) + offset);
        }

        // 端口寄存器块首址
        volatile uint32_t* PortRegisters(const Controller& controller, uint32_t port) {
            return At32(controller.abar, kRegPortBase + port * kRegPortStride);
        }

        // 读端口寄存器
        uint32_t PortRead(const Controller& controller, uint32_t port, uint32_t offset) {
            return *At32(PortRegisters(controller, port), offset);
        }

        // 写端口寄存器
        void PortWrite(const Controller& controller, uint32_t port, uint32_t offset, uint32_t value) {
            *At32(PortRegisters(controller, port), offset) = value;
        }

        // 等某寄存器上的位全部清零
        bool WaitClear(const Controller& controller, uint32_t port, uint32_t offset, uint32_t mask, uint32_t limit) {
            for (uint32_t waited = 0; waited < limit; ++waited) {
                if ((PortRead(controller, port, offset) & mask) == 0) return true;
            }
            return false;
        }

        // 等某寄存器上的位全部置位
        bool WaitSet(const Controller& controller, uint32_t port, uint32_t offset, uint32_t mask, uint32_t limit) {
            for (uint32_t waited = 0; waited < limit; ++waited) {
                if ((PortRead(controller, port, offset) & mask) == mask) return true;
            }
            return false;
        }

        // 小端写 32 位到字节缓冲：PRDT 条目按字节拼
        void Put32(uint8_t* target, uint32_t value) {
            target[0] = static_cast<uint8_t>(value);
            target[1] = static_cast<uint8_t>(value >> 8);
            target[2] = static_cast<uint8_t>(value >> 16);
            target[3] = static_cast<uint8_t>(value >> 24);
        }

        // 清一段字节缓冲
        void ZeroBytes(uint8_t* target, uint32_t bytes) {
            for (uint32_t index = 0; index < bytes; ++index) target[index] = 0;
        }

        // 构造 H2D 寄存器 FIS：identify 不带 LBA/扇区，读命令带 LBA48 的 LBA 与扇区计数
        void BuildFis(uint8_t* fis, uint8_t command, uint64_t lba, uint32_t sectors, bool withTransfer) {
            ZeroBytes(fis, kFisBytes);
            fis[0] = 0x27;   // FIS 类型：H2D 寄存器
            fis[1] = 0x80;   // C = 1：这是一条命令
            fis[2] = command;
            fis[7] = 0x40;   // 设备寄存器：LBA 模式
            if (withTransfer) {
                fis[4] = static_cast<uint8_t>(lba);
                fis[5] = static_cast<uint8_t>(lba >> 8);
                fis[6] = static_cast<uint8_t>(lba >> 16);
                fis[8] = static_cast<uint8_t>(lba >> 24);
                fis[9] = static_cast<uint8_t>(lba >> 32);
                fis[10] = static_cast<uint8_t>(lba >> 40);
                fis[12] = static_cast<uint8_t>(sectors);
                fis[13] = static_cast<uint8_t>(sectors >> 8);
            }
            fis[15] = 0x80;   // 控制字节同样置 C
        }

        // 命令槽下发：写命令表与命令头、置 PxCI、轮询完成；失败返回 false
        bool IssueCommand(Controller& controller, uint32_t slot, const uint8_t* fis, uint32_t dataBytes) {
            if (slot >= kMaxCommandSlots) return false;
            // 命令表按 256 字节清空后再填：CFIS 与 PRDT 都在里面
            ZeroBytes(controller.commandTable, kCommandTableBytes);
            for (uint32_t index = 0; index < kFisBytes; ++index) controller.commandTable[index] = fis[index];
            uint32_t prdtEntries = 0;
            if (dataBytes != 0) {
                prdtEntries = 1;
                uint8_t* prdt = controller.commandTable + kCommandTablePrdtOffset;
                Put32(prdt + 0, static_cast<uint32_t>(controller.dataPhysical));
                Put32(prdt + 4, static_cast<uint32_t>(controller.dataPhysical >> 32));
                Put32(prdt + 8, 0);
                Put32(prdt + 12, kPrdtInterrupt | (dataBytes - 1));
            }
            // 命令头：DW0 由生成头同源的编码给出，DW2/DW3 是命令表物理地址
            uint8_t* header = controller.commandList + slot * kCommandHeaderBytes;
            ZeroBytes(header, kCommandHeaderBytes);
            const uint32_t header0 = Storage::AhciCommand::HeaderDw0(Storage::AhciCommand::kFisDwords, prdtEntries);
            const uint32_t header1 = Storage::AhciCommand::HeaderDw1();
            Put32(header + 0, header0);
            Put32(header + 4, header1);
            Put32(header + 8, static_cast<uint32_t>(controller.commandTablePhysical));
            Put32(header + 12, static_cast<uint32_t>(controller.commandTablePhysical >> 32));
            // 清中断状态后置本位发起命令，等 HBA 把该位清掉
            PortWrite(controller, controller.port, kPortInterruptStatus, 0xFFFFFFFFu);
            PortWrite(controller, controller.port, kPortCommandIssue, 1u << slot);
            if (!WaitClear(controller, controller.port, kPortCommandIssue, 1u << slot, kPollLimit)) return false;
            const uint32_t status = PortRead(controller, controller.port, kPortInterruptStatus);
            const uint32_t taskFile = PortRead(controller, controller.port, kPortTaskFile);
            PortWrite(controller, controller.port, kPortInterruptStatus, status);
            if ((status & kInterruptTaskFileError) != 0) return false;
            if ((taskFile & kTaskFileError) != 0) return false;
            return true;
        }

        // 端口复位：停端口 → COMRESET → 等设备重新就绪 → 重新装命令列表与 FIS 接收区
        bool ResetPort(Controller& controller) {
            const uint32_t port = controller.port;
            // 停端口：先清 ST 等 CR 清零，再清 FRE 等 FR 清零
            PortWrite(controller, port, kPortCommand, PortRead(controller, port, kPortCommand) & ~kCmdStart);
            (void)WaitClear(controller, port, kPortCommand, kCmdCommandRunning, kResetPollLimit);
            PortWrite(controller, port, kPortCommand, PortRead(controller, port, kPortCommand) & ~kCmdFisReceiveEnable);
            (void)WaitClear(controller, port, kPortCommand, kCmdFisReceiveRunning, kResetPollLimit);
            // COMRESET：PxSCTL.DET = 1 保持一段时间后再清零
            PortWrite(controller, port, kPortSataControl, 1u);
            for (uint32_t spin = 0; spin < kResetSpin; ++spin) asm volatile("pause");
            PortWrite(controller, port, kPortSataControl, 0u);
            bool ready = false;
            for (uint32_t waited = 0; waited < kResetPollLimit; ++waited) {
                if ((PortRead(controller, port, kPortSataStatus) & kDetMask) == kDetPresent) {
                    ready = true;
                    break;
                }
            }
            if (!ready) return false;
            // 重新装命令列表与 FIS 接收区，再启 FRE 与 ST
            PortWrite(controller, port, kPortClb, static_cast<uint32_t>(controller.commandListPhysical));
            PortWrite(controller, port, kPortClbu, static_cast<uint32_t>(controller.commandListPhysical >> 32));
            PortWrite(controller, port, kPortFb, static_cast<uint32_t>(controller.fisPhysical));
            PortWrite(controller, port, kPortFbu, static_cast<uint32_t>(controller.fisPhysical >> 32));
            PortWrite(controller, port, kPortSataError, 0xFFFFFFFFu);
            PortWrite(controller, port, kPortInterruptStatus, 0xFFFFFFFFu);
            PortWrite(controller, port, kPortInterruptEnable, 0u);
            PortWrite(controller, port, kPortCommand, PortRead(controller, port, kPortCommand) | kCmdFisReceiveEnable);
            if (!WaitSet(controller, port, kPortCommand, kCmdFisReceiveRunning, kResetPollLimit)) return false;
            PortWrite(controller, port, kPortCommand, PortRead(controller, port, kPortCommand) | kCmdStart);
            return WaitSet(controller, port, kPortCommand, kCmdCommandRunning, kResetPollLimit);
        }

        // 从 IDENTIFY 返回的字里取型号文本：字内低字节在前，去掉尾部空格与零
        void ReadModel(const uint16_t* identity, char* model) {
            uint32_t written = 0;
            for (uint32_t word = 27; word <= 46; ++word) {
                model[written++] = static_cast<char>(identity[word] & 0xFF);
                model[written++] = static_cast<char>((identity[word] >> 8) & 0xFF);
            }
            while (written > 0 && (model[written - 1] == ' ' || model[written - 1] == '\0')) --written;
            model[written] = '\0';
        }

        // 从 IDENTIFY 返回的字里取容量：优先 LBA48，退回 28 位
        uint64_t IdentitySectors(const uint16_t* identity) {
            const uint32_t support = identity[49];
            if (((support >> 9) & 1u) == 0) return 0;
            if (((support >> 10) & 1u) != 0) {
                uint64_t sectors = 0;
                sectors |= static_cast<uint64_t>(identity[100]);
                sectors |= static_cast<uint64_t>(identity[101]) << 16;
                sectors |= static_cast<uint64_t>(identity[102]) << 32;
                sectors |= static_cast<uint64_t>(identity[103]) << 48;
                if (sectors != 0) return sectors;
            }
            return static_cast<uint64_t>(identity[60]) | (static_cast<uint64_t>(identity[61]) << 16);
        }

        // IDENTIFY DEVICE：数据进弹跳页，再解析型号与容量
        bool Identify(Controller& controller) {
            uint8_t fis[kFisBytes];
            BuildFis(fis, kCommandIdentify, 0, 0, false);
            if (!IssueCommand(controller, 0, fis, kIdentifyBytes)) return false;
            uint16_t identity[kIdentifyWords] = {};
            memcpy(identity, controller.data, kIdentifyBytes);
            controller.sectors = IdentitySectors(identity);
            if (controller.sectors == 0) return false;
            ReadModel(identity, controller.model);
            return true;
        }

    }

    void StopPort(Controller& controller) {
        if (controller.abar == nullptr) return;
        const uint32_t port = controller.port;
        PortWrite(controller, port, kPortCommand, PortRead(controller, port, kPortCommand) & ~kCmdStart);
        (void)WaitClear(controller, port, kPortCommand, kCmdCommandRunning, kResetPollLimit);
        PortWrite(controller, port, kPortCommand, PortRead(controller, port, kPortCommand) & ~kCmdFisReceiveEnable);
        (void)WaitClear(controller, port, kPortCommand, kCmdFisReceiveRunning, kResetPollLimit);
    }

    bool Initialize(Controller& controller) {
        if (controller.abar == nullptr || controller.commandList == nullptr || controller.commandTable == nullptr ||
            controller.fis == nullptr || controller.data == nullptr || controller.pageBytes == 0) return false;
        // AHCI 使能
        const uint32_t ghc = *At32(controller.abar, kRegGhc);
        *At32(controller.abar, kRegGhc) = ghc | kGhcAhciEnable;
        // 找第一个接有 ATA 设备的端口
        const uint32_t ports = *At32(controller.abar, kRegPortsImplemented);
        bool found = false;
        for (uint32_t port = 0; port < kMaxPorts; ++port) {
            if ((ports & (1u << port)) == 0) continue;
            if ((PortRead(controller, port, kPortSataStatus) & kDetMask) != kDetPresent) continue;
            if (PortRead(controller, port, kPortSignature) != kSignatureAta) continue;
            controller.port = port;
            found = true;
            break;
        }
        if (!found) return false;
        // 停端口：清 ST 等 CR 清零，清 FRE 等 FR 清零
        PortWrite(controller, controller.port, kPortCommand, PortRead(controller, controller.port, kPortCommand) & ~kCmdStart);
        if (!WaitClear(controller, controller.port, kPortCommand, kCmdCommandRunning, kResetPollLimit)) return false;
        PortWrite(controller, controller.port, kPortCommand, PortRead(controller, controller.port, kPortCommand) & ~kCmdFisReceiveEnable);
        if (!WaitClear(controller, controller.port, kPortCommand, kCmdFisReceiveRunning, kResetPollLimit)) return false;
        // 清错误与中断状态，装命令列表与 FIS 接收区，不启用中断
        PortWrite(controller, controller.port, kPortSataError, 0xFFFFFFFFu);
        PortWrite(controller, controller.port, kPortInterruptStatus, 0xFFFFFFFFu);
        PortWrite(controller, controller.port, kPortInterruptEnable, 0u);
        PortWrite(controller, controller.port, kPortClb, static_cast<uint32_t>(controller.commandListPhysical));
        PortWrite(controller, controller.port, kPortClbu, static_cast<uint32_t>(controller.commandListPhysical >> 32));
        PortWrite(controller, controller.port, kPortFb, static_cast<uint32_t>(controller.fisPhysical));
        PortWrite(controller, controller.port, kPortFbu, static_cast<uint32_t>(controller.fisPhysical >> 32));
        // 先启 FIS 接收，再启命令处理
        PortWrite(controller, controller.port, kPortCommand, PortRead(controller, controller.port, kPortCommand) | kCmdFisReceiveEnable);
        if (!WaitSet(controller, controller.port, kPortCommand, kCmdFisReceiveRunning, kPollLimit)) return false;
        PortWrite(controller, controller.port, kPortCommand, PortRead(controller, controller.port, kPortCommand) | kCmdStart);
        if (!WaitSet(controller, controller.port, kPortCommand, kCmdCommandRunning, kPollLimit)) return false;
        controller.sectorBytes = kSectorBytes;
        // IDENTIFY 失败尝试一次端口复位再试
        if (!Identify(controller)) {
            if (!ResetPort(controller)) return false;
            if (!Identify(controller)) return false;
        }
        return true;
    }

    Storage::Error Read(Controller& controller, uint64_t offset, void* buffer, uint32_t bytes) {
        if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
        if (controller.abar == nullptr || controller.sectorBytes == 0) return Storage::Error::NotSupported;
        if ((offset & (controller.sectorBytes - 1)) != 0 || (bytes & (controller.sectorBytes - 1)) != 0) return Storage::Error::InvalidArgument;
        uint64_t lba = offset / controller.sectorBytes;
        uint32_t sectors = bytes / controller.sectorBytes;
        if (lba >= controller.sectors || sectors > controller.sectors - lba) return Storage::Error::InvalidArgument;
        // 单条命令不超过一页：按页分块，经弹跳页搬运
        const uint32_t sectorsPerCommand = controller.pageBytes / controller.sectorBytes;
        uint8_t* target = static_cast<uint8_t*>(buffer);
        while (sectors > 0) {
            uint32_t count = sectors;
            if (count > sectorsPerCommand) count = sectorsPerCommand;
            Storage::AhciCommand::Transfer transfer;
            if (!Storage::AhciCommand::PlanTransfer(lba * controller.sectorBytes, count * controller.sectorBytes, controller.sectorBytes, transfer)) {
                return Storage::Error::InvalidArgument;
            }
            uint8_t fis[kFisBytes];
            BuildFis(fis, kCommandReadDmaExt, transfer.lba, transfer.sectors, true);
            const uint32_t chunkBytes = count * controller.sectorBytes;
            if (!IssueCommand(controller, 0, fis, chunkBytes)) {
                // 超时或任务文件错误：复位端口后重试一次，仍失败按 I/O 错误上报
                if (!ResetPort(controller)) return Storage::Error::Io;
                if (!IssueCommand(controller, 0, fis, chunkBytes)) return Storage::Error::Io;
            }
            memcpy(target, controller.data, chunkBytes);
            target += chunkBytes;
            lba += count;
            sectors -= count;
        }
        return Storage::Error::Ok;
    }
}
