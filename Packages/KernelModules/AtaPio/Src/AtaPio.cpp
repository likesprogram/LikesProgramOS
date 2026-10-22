/* AtaPio.cpp
    传统 ATA / ATAPI 的 PIO 只读实现：端口 I/O、IDENTIFY、READ SECTORS 与 ATAPI PACKET

    端口访问全部是内联汇编的 in/out；状态轮询用有界次数，超时返回失败，不静机
    ATAPI 的命令流程按 ATA-ATAPI 规范：PACKET 命令后先等 DRQ 再写 12 字节命令包，
    读数据阶段再等 DRQ 按字搬运，结束后等 BSY 清零并检查 ERR
    容量与型号来自 IDENTIFY（ATA）或 IDENTIFY PACKET + READ CAPACITY（ATAPI），
    IDENTIFY 返回的字符串是 16 位字内低字节在前，这里按字节顺序还原并去掉尾部空格
*/

#include "AtaPio.hpp"

namespace AtaPio {
    namespace {
        // 命令块寄存器偏移
        constexpr uint16_t kRegFeature = 1;
        constexpr uint16_t kRegSectorCount = 2;
        constexpr uint16_t kRegLbaLow = 3;
        constexpr uint16_t kRegLbaMid = 4;
        constexpr uint16_t kRegLbaHigh = 5;
        constexpr uint16_t kRegDrive = 6;
        constexpr uint16_t kRegStatus = 7;
        // 状态位
        constexpr uint8_t kStatusError = 0x01;
        constexpr uint8_t kStatusDrq = 0x08;
        constexpr uint8_t kStatusDeviceFault = 0x20;
        constexpr uint8_t kStatusBusy = 0x80;
        // 控制块：nIEN 置位表示不启用中断（本驱动只轮询）
        constexpr uint8_t kControlNoInterrupt = 0x02;
        // 命令
        constexpr uint8_t kCommandReadSectors = 0x20;
        constexpr uint8_t kCommandReadSectorsExt = 0x24;
        constexpr uint8_t kCommandIdentifyDevice = 0xEC;
        constexpr uint8_t kCommandIdentifyPacket = 0xA1;
        constexpr uint8_t kCommandPacket = 0xA0;
        // 设备寄存器：选盘用主/从，寻址用 LBA 模式位
        constexpr uint8_t kDriveMaster = 0xA0;
        constexpr uint8_t kDriveSlave = 0xB0;
        constexpr uint8_t kDriveLba = 0xE0;
        // 通道端口基址：主通道与从通道的命令块 / 控制块
        constexpr uint16_t kPrimaryCommand = 0x01F0;
        constexpr uint16_t kPrimaryControl = 0x03F6;
        constexpr uint16_t kSecondaryCommand = 0x0170;
        constexpr uint16_t kSecondaryControl = 0x0376;
        // 轮询上限：命令是毫秒级，这个次数在实机与 QEMU 上都足够宽松；超时只报失败
        constexpr uint32_t kPollLimit = 20000000;
        // IDENTIFY 返回 256 个 16 位字
        constexpr uint32_t kIdentifyWords = 256;
        // ATAPI 命令包长度：12 字节，即 6 个 16 位字
        constexpr uint32_t kPacketWords = 6;
        // 一次 ATAPI PIO 传输的字节数上限：字节计数寄存器只有 16 位
        constexpr uint32_t kAtapiPioLimit = 0xFFFE;
        // ATAPI READ(10) 一次读的扇区上限：计数是 16 位
        constexpr uint32_t kRead10SectorLimit = 0xFFFF;

        // —— 端口 I/O：ring 0 直接执行 in/out ——
        uint8_t In8(uint16_t port) {
            uint8_t value = 0;
            asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
            return value;
        }

        uint16_t In16(uint16_t port) {
            uint16_t value = 0;
            asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
            return value;
        }

        void Out8(uint16_t port, uint8_t value) { asm volatile("outb %0, %1" : : "a"(value), "Nd"(port)); }

        void Out16(uint16_t port, uint16_t value) { asm volatile("outw %0, %1" : : "a"(value), "Nd"(port)); }

        // 从数据端口读 words 个字到字节缓冲：按小端摊开，不假设缓冲的 2 字节对齐
        void ReadWordsTo(uint16_t port, uint8_t* target, uint32_t words) {
            for (uint32_t index = 0; index < words; ++index) {
                const uint16_t word = In16(port);
                target[index * 2] = static_cast<uint8_t>(word & 0xFF);
                target[index * 2 + 1] = static_cast<uint8_t>(word >> 8);
            }
        }

        // 从数据端口读 words 个字到 16 位字数组（IDENTIFY 用）
        void ReadWords(uint16_t port, uint16_t* target, uint32_t words) {
            for (uint32_t index = 0; index < words; ++index) target[index] = In16(port);
        }

        // 写 12 字节 ATAPI 命令包：命令块数据端口按字写
        void WritePacket(uint16_t commandBase, const uint8_t* packet) {
            for (uint32_t index = 0; index < kPacketWords; ++index) {
                const uint16_t word = static_cast<uint16_t>(packet[index * 2]) |
                                      static_cast<uint16_t>(static_cast<uint16_t>(packet[index * 2 + 1]) << 8);
                Out16(commandBase, word);
            }
        }

        // 选盘后的 400ns 延迟：读控制块四次即可
        void DelaySelect(uint16_t controlBase) {
            for (uint32_t index = 0; index < 4; ++index) (void)In8(controlBase);
        }

        // 等 BSY 清零：超时返回 false
        bool WaitNotBusy(uint16_t commandBase) {
            for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
                if ((In8(commandBase + kRegStatus) & kStatusBusy) == 0) return true;
            }
            return false;
        }

        // 等 DRQ 置位且没有错误：超时或错误返回 false
        bool WaitDrq(uint16_t commandBase) {
            for (uint32_t waited = 0; waited < kPollLimit; ++waited) {
                const uint8_t status = In8(commandBase + kRegStatus);
                if ((status & kStatusBusy) != 0) continue;
                if ((status & (kStatusError | kStatusDeviceFault)) != 0) return false;
                if ((status & kStatusDrq) != 0) return true;
            }
            return false;
        }

        // 命令收尾：等 BSY 清零并确认没有错误
        bool FinishCommand(uint16_t commandBase) {
            if (!WaitNotBusy(commandBase)) return false;
            const uint8_t status = In8(commandBase + kRegStatus);
            return (status & (kStatusError | kStatusDeviceFault)) == 0;
        }

        // 选择一个盘位并等它退出忙态
        bool SelectDrive(uint16_t commandBase, uint16_t controlBase, uint32_t driveIndex) {
            Out8(controlBase, kControlNoInterrupt);
            Out8(commandBase + kRegDrive, static_cast<uint8_t>(driveIndex == 0 ? kDriveMaster : kDriveSlave));
            DelaySelect(controlBase);
            return WaitNotBusy(commandBase);
        }

        // 发 IDENTIFY 并读回 256 个字：失败返回 false（用 ERR/DF 与 DRQ 区分设备是否响应）
        bool Identify(uint16_t commandBase, uint16_t controlBase, uint8_t command, uint16_t* identity) {
            (void)controlBase;
            Out8(commandBase + kRegSectorCount, 0);
            Out8(commandBase + kRegLbaLow, 0);
            Out8(commandBase + kRegLbaMid, 0);
            Out8(commandBase + kRegLbaHigh, 0);
            Out8(commandBase + kRegStatus, command);
            if (!WaitNotBusy(commandBase)) return false;
            if ((In8(commandBase + kRegStatus) & (kStatusError | kStatusDeviceFault)) != 0) return false;
            if (!WaitDrq(commandBase)) return false;
            ReadWords(commandBase, identity, kIdentifyWords);
            return FinishCommand(commandBase);
        }

        // 从 IDENTIFY 的 256 个字里还原型号文本：先按字内低字节在前展开，再去掉尾部空格
        void ReadModel(const uint16_t* identity, char* model) {
            uint32_t written = 0;
            for (uint32_t word = 27; word <= 46; ++word) {
                model[written++] = static_cast<char>(identity[word] & 0xFF);
                model[written++] = static_cast<char>((identity[word] >> 8) & 0xFF);
            }
            while (written > 0 && (model[written - 1] == ' ' || model[written - 1] == '\0')) --written;
            model[written] = '\0';
        }

        // 取 IDENTIFY 里的容量：优先 LBA48 的 48 位字，退回 LBA28 的 28 位字
        uint64_t IdentityCapacity(const uint16_t* identity, uint32_t& lba48) {
            const uint32_t support = static_cast<uint32_t>(identity[49]);
            lba48 = (support >> 10) & 1u;
            if (((support >> 9) & 1u) == 0) return 0;   // 不支持 LBA
            if (lba48 != 0) {
                uint64_t sectors = 0;
                sectors |= static_cast<uint64_t>(identity[100]);
                sectors |= static_cast<uint64_t>(identity[101]) << 16;
                sectors |= static_cast<uint64_t>(identity[102]) << 32;
                sectors |= static_cast<uint64_t>(identity[103]) << 48;
                if (sectors != 0) return sectors;
            }
            return static_cast<uint64_t>(identity[60]) | (static_cast<uint64_t>(identity[61]) << 16);
        }

        // 发一条 ATAPI 命令包并读回数据（PIO IN）；返回收到的字节数，失败返回 0
        uint32_t PacketCommandIn(uint16_t commandBase, uint16_t controlBase, uint32_t driveIndex,
                                 const uint8_t* packet, void* buffer, uint32_t bytes) {
            if (bytes == 0 || bytes > kAtapiPioLimit) return 0;
            if (!SelectDrive(commandBase, controlBase, driveIndex)) return 0;
            Out8(commandBase + kRegFeature, 0);   // PIO 传输
            Out8(commandBase + kRegLbaMid, static_cast<uint8_t>(bytes & 0xFF));          // 字节计数低
            Out8(commandBase + kRegLbaHigh, static_cast<uint8_t>((bytes >> 8) & 0xFF));  // 字节计数高
            Out8(commandBase + kRegStatus, kCommandPacket);
            if (!WaitDrq(commandBase)) return 0;
            WritePacket(commandBase, packet);
            if (!WaitDrq(commandBase)) return 0;
            ReadWordsTo(commandBase, static_cast<uint8_t*>(buffer), (bytes + 1) / 2);
            if (!FinishCommand(commandBase)) return 0;
            return bytes;
        }

        // 读 ATAPI 设备的容量：READ CAPACITY(10)，返回逻辑扇区数与扇区字节数
        bool PacketCapacity(uint16_t commandBase, uint16_t controlBase, uint32_t driveIndex,
                            uint64_t& sectors, uint32_t& sectorBytes) {
            const uint8_t packet[12] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            uint8_t data[8] = {};
            if (PacketCommandIn(commandBase, controlBase, driveIndex, packet, data, sizeof(data)) == 0) return false;
            const uint64_t lastLba = (static_cast<uint64_t>(data[0]) << 24) | (static_cast<uint64_t>(data[1]) << 16) |
                                     (static_cast<uint64_t>(data[2]) << 8) | static_cast<uint64_t>(data[3]);
            const uint32_t blockBytes = (static_cast<uint32_t>(data[4]) << 24) | (static_cast<uint32_t>(data[5]) << 16) |
                                        (static_cast<uint32_t>(data[6]) << 8) | static_cast<uint32_t>(data[7]);
            if (blockBytes == 0) return false;
            sectors = lastLba + 1;
            sectorBytes = blockBytes;
            return true;
        }

        // 识别一个盘位：先按 ATAPI 试 IDENTIFY PACKET，再退回 IDENTIFY DEVICE
        bool ProbeDrive(uint16_t commandBase, uint16_t controlBase, uint32_t driveIndex, Drive& drive) {
            uint16_t identity[kIdentifyWords] = {};
            if (!SelectDrive(commandBase, controlBase, driveIndex)) return false;
            uint32_t kind = kKindNone;
            if (Identify(commandBase, controlBase, kCommandIdentifyPacket, identity)) {
                kind = kKindAtapi;
            } else {
                if (!SelectDrive(commandBase, controlBase, driveIndex)) return false;
                if (!Identify(commandBase, controlBase, kCommandIdentifyDevice, identity)) return false;
                kind = kKindAta;
            }
            drive.commandBase = commandBase;
            drive.controlBase = controlBase;
            drive.driveIndex = driveIndex;
            drive.kind = kind;
            ReadModel(identity, drive.model);
            if (kind == kKindAtapi) {
                uint64_t sectors = 0;
                uint32_t sectorBytes = 0;
                if (!PacketCapacity(commandBase, controlBase, driveIndex, sectors, sectorBytes)) return false;
                drive.sectors = sectors;
                drive.sectorBytes = sectorBytes;
                drive.lba48 = 0;
            } else {
                const uint64_t sectors = IdentityCapacity(identity, drive.lba48);
                if (sectors == 0) return false;
                drive.sectors = sectors;
                drive.sectorBytes = 512;
            }
            return true;
        }

        // ATA PIO 读一段：按设备扇区逐扇区搬运
        bool ReadAta(const Drive& drive, uint64_t lba, uint32_t sectors, uint8_t* buffer) {
            const uint16_t commandBase = drive.commandBase;
            if (!SelectDrive(commandBase, drive.controlBase, drive.driveIndex)) return false;
            const bool extended = drive.lba48 != 0 || lba >= 0x10000000ull;
            if (extended) {
                // LBA48：先写计数高与 LBA 高段，再写计数低与 LBA 低段
                Out8(commandBase + kRegFeature, 0);
                Out8(commandBase + kRegSectorCount, static_cast<uint8_t>(sectors >> 8));
                Out8(commandBase + kRegLbaLow, static_cast<uint8_t>(lba >> 24));
                Out8(commandBase + kRegLbaMid, static_cast<uint8_t>(lba >> 32));
                Out8(commandBase + kRegLbaHigh, static_cast<uint8_t>(lba >> 40));
                Out8(commandBase + kRegSectorCount, static_cast<uint8_t>(sectors & 0xFF));
                Out8(commandBase + kRegLbaLow, static_cast<uint8_t>(lba & 0xFF));
                Out8(commandBase + kRegLbaMid, static_cast<uint8_t>(lba >> 8));
                Out8(commandBase + kRegLbaHigh, static_cast<uint8_t>(lba >> 16));
                Out8(commandBase + kRegDrive, kDriveLba);
                Out8(commandBase + kRegStatus, kCommandReadSectorsExt);
            } else {
                Out8(commandBase + kRegSectorCount, static_cast<uint8_t>(sectors & 0xFF));
                Out8(commandBase + kRegLbaLow, static_cast<uint8_t>(lba & 0xFF));
                Out8(commandBase + kRegLbaMid, static_cast<uint8_t>(lba >> 8));
                Out8(commandBase + kRegLbaHigh, static_cast<uint8_t>(lba >> 16));
                Out8(commandBase + kRegDrive,
                     static_cast<uint8_t>(kDriveLba | static_cast<uint8_t>(drive.driveIndex * 0x10) | static_cast<uint8_t>((lba >> 24) & 0x0F)));
                Out8(commandBase + kRegStatus, kCommandReadSectors);
            }
            const uint32_t wordsPerSector = drive.sectorBytes / 2;
            for (uint32_t sector = 0; sector < sectors; ++sector) {
                if (!WaitDrq(commandBase)) return false;
                ReadWordsTo(commandBase, buffer, wordsPerSector);
                buffer += drive.sectorBytes;
            }
            return FinishCommand(commandBase);
        }

        // ATAPI 读一段：READ(10) 或 READ(12)，逐命令搬运
        bool ReadAtapi(const Drive& drive, uint64_t lba, uint32_t sectors, uint8_t* buffer) {
            const uint32_t maxSectorsPerPacket = kAtapiPioLimit / drive.sectorBytes;
            while (sectors > 0) {
                uint32_t count = sectors;
                if (count > maxSectorsPerPacket) count = maxSectorsPerPacket;
                uint8_t packet[12] = {};
                if (count <= kRead10SectorLimit && lba <= 0xFFFFFFFFull) {
                    packet[0] = 0x28;   // READ(10)
                    packet[2] = static_cast<uint8_t>(lba >> 24);
                    packet[3] = static_cast<uint8_t>(lba >> 16);
                    packet[4] = static_cast<uint8_t>(lba >> 8);
                    packet[5] = static_cast<uint8_t>(lba);
                    packet[7] = static_cast<uint8_t>(count >> 8);
                    packet[8] = static_cast<uint8_t>(count);
                } else {
                    packet[0] = 0xA8;   // READ(12)
                    packet[2] = static_cast<uint8_t>(lba >> 24);
                    packet[3] = static_cast<uint8_t>(lba >> 16);
                    packet[4] = static_cast<uint8_t>(lba >> 8);
                    packet[5] = static_cast<uint8_t>(lba);
                    packet[6] = static_cast<uint8_t>(count >> 24);
                    packet[7] = static_cast<uint8_t>(count >> 16);
                    packet[8] = static_cast<uint8_t>(count >> 8);
                    packet[9] = static_cast<uint8_t>(count);
                }
                const uint32_t bytes = count * drive.sectorBytes;
                if (PacketCommandIn(drive.commandBase, drive.controlBase, drive.driveIndex, packet, buffer, bytes) != bytes) return false;
                buffer += bytes;
                lba += count;
                sectors -= count;
            }
            return true;
        }
    }

    uint32_t Probe(Drive* drives, uint32_t capacity) {
        if (drives == nullptr || capacity == 0) return 0;
        const uint16_t commandBases[2] = {kPrimaryCommand, kSecondaryCommand};
        const uint16_t controlBases[2] = {kPrimaryControl, kSecondaryControl};
        uint32_t found = 0;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            for (uint32_t driveIndex = 0; driveIndex < 2; ++driveIndex) {
                if (found >= capacity) return found;
                Drive drive;
                if (!ProbeDrive(commandBases[channel], controlBases[channel], driveIndex, drive)) continue;
                drives[found] = drive;
                ++found;
            }
        }
        return found;
    }

    Storage::Error Read(const Drive& drive, uint64_t offset, void* buffer, uint32_t bytes) {
        if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
        if (drive.kind == kKindNone || drive.sectorBytes == 0) return Storage::Error::NotSupported;
        if ((offset & (drive.sectorBytes - 1)) != 0 || (bytes & (drive.sectorBytes - 1)) != 0) return Storage::Error::InvalidArgument;
        const uint64_t lba = offset / drive.sectorBytes;
        const uint32_t sectors = bytes / drive.sectorBytes;
        if (lba >= drive.sectors || sectors > drive.sectors - lba) return Storage::Error::InvalidArgument;
        const bool ok = drive.kind == kKindAta
                            ? ReadAta(drive, lba, sectors, static_cast<uint8_t*>(buffer))
                            : ReadAtapi(drive, lba, sectors, static_cast<uint8_t*>(buffer));
        return ok ? Storage::Error::Ok : Storage::Error::Io;
    }
}
