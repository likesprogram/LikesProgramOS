/* ControllerPath.cpp
    自建控制器路径的实现：设备路径 → PCI 设备 → 类码 / BAR → AHCI 或 NVMe 只读访问

    控制器读逻辑直接编入内核侧的 Ahci.cpp / Nvme.cpp（同一份源码、同一份命令编码），
    本文件只做 UEFI 平台接入：设备路径与 PciIo、PCI 配置空间、BAR、DMA 页与错误阶段
    不写介质、不申报刷新原语；失败不静默回退，调用方按阶段映射 E-UEFI-100..104
*/

#include <ControllerPath.hpp>

#include <Libc.hpp>

namespace {
    using namespace Baleen::Uefi;

    // 设备路径扫描上限：PciRoot/Pci 路径通常只有几十字节，给足余量后拒绝异常长路径
    constexpr uint32_t kPciPathBytes = 256;

    // MMIO 读一个 32 位寄存器：context 是 BAR 的虚拟地址
    uint32_t MmioRead32(void* context, uint32_t offset) {
        return *reinterpret_cast<volatile uint32_t*>(static_cast<volatile uint8_t*>(context) + offset);
    }

    // MMIO 写一个 32 位寄存器
    void MmioWrite32(void* context, uint32_t offset, uint32_t value) {
        *reinterpret_cast<volatile uint32_t*>(static_cast<volatile uint8_t*>(context) + offset) = value;
    }

    // 读 PCI 配置空间的 32 位字段
    bool ReadConfig32(PciIoProtocol* pciIo, uint32_t offset, uint32_t& value) {
        if (pciIo == nullptr || pciIo->pciRead == nullptr) return false;
        value = 0;
        return !IsError(pciIo->pciRead(pciIo, kPciIoWidthUint32, offset, 1, &value));
    }

    // 读 PCI 配置空间的 16 位字段
    bool ReadConfig16(PciIoProtocol* pciIo, uint32_t offset, uint16_t& value) {
        if (pciIo == nullptr || pciIo->pciRead == nullptr) return false;
        value = 0;
        return !IsError(pciIo->pciRead(pciIo, kPciIoWidthUint16, offset, 1, &value));
    }

    // 写 PCI 配置空间的 16 位字段
    bool WriteConfig16(PciIoProtocol* pciIo, uint32_t offset, uint16_t value) {
        if (pciIo == nullptr || pciIo->pciWrite == nullptr) return false;
        uint16_t current = value;
        return !IsError(pciIo->pciWrite(pciIo, kPciIoWidthUint16, offset, 1, &current));
    }

    // 设备路径节点长度：小端 16 位
    uint32_t NodeBytes(const DevicePath* node) {
        return static_cast<uint32_t>(node->length[0]) | (static_cast<uint32_t>(node->length[1]) << 8);
    }
}

namespace Baleen {
    namespace Uefi {
        Storage::Error ControllerBlockDevice::Fail(ControllerStage stage, const char* detail) {
            m_stage = stage;
            m_detail = detail;
            return Storage::Error::NotSupported;
        }

        const char* ControllerBlockDevice::ControllerName() const {
            if (m_ahci.abar != nullptr) return "AHCI";
            if (m_nvme.registers.read != nullptr) return "NVME";
            return "";
        }

        Storage::Error ControllerBlockDevice::Write(uint64_t offset, const void* buffer, uint32_t bytes) {
            (void)offset;
            (void)buffer;
            (void)bytes;
            return Storage::Error::ReadOnly;
        }

        Storage::Error ControllerBlockDevice::Flush() { return Storage::Error::ReadOnly; }

        bool ControllerBlockDevice::AllocatePages(BootServices* services, uint32_t pages, uint8_t*& pointer, uint64_t& physical) {
            pointer = nullptr;
            physical = 0;
            if (services == nullptr || pages == 0) return false;
            if (IsError(services->allocatePages(kAllocateAnyPages, kMemoryLoaderData, pages, &physical))) return false;
            if (physical == 0) return false;
            pointer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(physical));
            // 清空后再用：队列或命令表里的上电残留不能被当成有效条目
            memset(pointer, 0, pages * static_cast<uint32_t>(kPageBytes));
            return true;
        }

        ControllerStage ControllerBlockDevice::FindPciDevice(BootServices* services, EfiHandle deviceHandle, PciIoProtocol*& pciIo) {
            pciIo = nullptr;
            DevicePath* path = nullptr;
            if (IsError(services->handleProtocol(deviceHandle, &kDevicePathProtocolGuid, reinterpret_cast<void**>(&path))) || path == nullptr) {
                m_detail = "the loaded image has no device path";
                return ControllerStage::DevicePath;
            }
            // 走一遍设备路径：确认路径里存在 PCI 硬件节点；节点长度同时做上界检查
            const DevicePath* node = path;
            const DevicePath* pciNode = nullptr;
            uint32_t consumed = 0;
            for (uint32_t step = 0; step < 32; ++step) {
                if (node->type == kDevicePathEndType && node->subType == kDevicePathEndSubType) break;
                const uint32_t bytes = NodeBytes(node);
                if (bytes < sizeof(DevicePath) || consumed + bytes > kPciPathBytes) return ControllerStage::DevicePath;
                if (node->type == kDevicePathHardwareType && node->subType == kDevicePathPciSubType) pciNode = node;
                consumed += bytes;
                node = reinterpret_cast<const DevicePath*>(reinterpret_cast<const uint8_t*>(node) + bytes);
            }
            if (pciNode == nullptr) {
                m_detail = "the device path has no PCI node";
                return ControllerStage::DevicePath;
            }
            // 用固件给出的完整设备路径找最长前缀：PciIo 装在 PCI 控制器句柄上，
            // 所以盘 / 分区路径匹配到控制器句柄为止；调用会推进 pointer 到未匹配部分
            DevicePath* pointer = path;
            EfiHandle handle = nullptr;
            if (IsError(services->locateDevicePath(&kPciIoProtocolGuid, &pointer, &handle)) || handle == nullptr) {
                m_detail = "the PCI controller handle isn't locatable";
                return ControllerStage::PciIo;
            }
            if (IsError(services->handleProtocol(handle, &kPciIoProtocolGuid, reinterpret_cast<void**>(&pciIo))) || pciIo == nullptr) {
                m_detail = "the boot controller has no PCI I/O protocol";
                return ControllerStage::PciIo;
            }
            return ControllerStage::None;
        }

        bool ControllerBlockDevice::ReadBar(PciIoProtocol* pciIo, uint32_t barIndex, uint64_t minimumBytes, uint64_t& begin, uint64_t& bytes) {
            begin = 0;
            bytes = 0;
            if (pciIo == nullptr || pciIo->getBarAttributes == nullptr || m_services == nullptr) return false;
            void* raw = nullptr;
            if (IsError(pciIo->getBarAttributes(pciIo, barIndex, nullptr, &raw)) || raw == nullptr) return false;
            const PciBarResource* const resource = static_cast<const PciBarResource*>(raw);
            bool usable = resource->resourceType == 0 && resource->rangeBytes >= minimumBytes && resource->rangeBytes != 0;
            if (usable) {
                // BAR 基址 = 地址范围下界 + 地址翻译偏移；翻译偏移通常为 0，按规范相加
                if (resource->minimum > 0xFFFFFFFFFFFFFFFFull - resource->translation) usable = false;
                else begin = resource->minimum + resource->translation;
                bytes = resource->rangeBytes;
            }
            m_services->freePool(raw);
            return usable && begin != 0;
        }

        bool ControllerBlockDevice::InitializeAhci(BootServices* services) {
            uint8_t* commandList = nullptr;
            uint8_t* fis = nullptr;
            uint8_t* commandTable = nullptr;
            uint8_t* data = nullptr;
            uint64_t commandListPhysical = 0;
            uint64_t fisPhysical = 0;
            uint64_t commandTablePhysical = 0;
            uint64_t dataPhysical = 0;
            // 四页一轮分配：任一失败即按阶段 Init 报出，已分配页不回收（失败路径会停机，不继续）
            if (!AllocatePages(services, 1, commandList, commandListPhysical) ||
                !AllocatePages(services, 1, fis, fisPhysical) ||
                !AllocatePages(services, 1, commandTable, commandTablePhysical) ||
                !AllocatePages(services, 1, data, dataPhysical)) return false;
            m_ahci.abar = m_bar;
            m_ahci.commandList = commandList;
            m_ahci.fis = fis;
            m_ahci.commandTable = commandTable;
            m_ahci.data = data;
            m_ahci.commandListPhysical = commandListPhysical;
            m_ahci.fisPhysical = fisPhysical;
            m_ahci.commandTablePhysical = commandTablePhysical;
            m_ahci.dataPhysical = dataPhysical;
            m_ahci.pageBytes = static_cast<uint32_t>(kPageBytes);
            if (!Ahci::Initialize(m_ahci)) return false;
            if (m_ahci.sectorBytes == 0 || m_ahci.sectors == 0) return false;
            m_blockBytes = m_ahci.sectorBytes;
            m_capacityBytes = m_ahci.sectors * m_ahci.sectorBytes;
            for (uint32_t i = 0; i < 40 && m_ahci.model[i] != '\0'; ++i) m_model[i] = m_ahci.model[i];
            m_model[40] = '\0';
            return true;
        }

        bool ControllerBlockDevice::InitializeNvme(BootServices* services) {
            uint8_t* adminSq = nullptr;
            uint8_t* adminCq = nullptr;
            uint8_t* ioSq = nullptr;
            uint8_t* ioCq = nullptr;
            uint8_t* data = nullptr;
            uint64_t adminSqPhysical = 0;
            uint64_t adminCqPhysical = 0;
            uint64_t ioSqPhysical = 0;
            uint64_t ioCqPhysical = 0;
            uint64_t dataPhysical = 0;
            if (!AllocatePages(services, 1, adminSq, adminSqPhysical) ||
                !AllocatePages(services, 1, adminCq, adminCqPhysical) ||
                !AllocatePages(services, 1, ioSq, ioSqPhysical) ||
                !AllocatePages(services, 1, ioCq, ioCqPhysical) ||
                !AllocatePages(services, 1, data, dataPhysical)) return false;
            m_nvme.registers.read = MmioRead32;
            m_nvme.registers.write = MmioWrite32;
            m_nvme.registers.context = reinterpret_cast<void*>(const_cast<uint32_t*>(m_bar));
            m_nvme.adminSq = adminSq;
            m_nvme.adminCq = adminCq;
            m_nvme.ioSq = ioSq;
            m_nvme.ioCq = ioCq;
            m_nvme.data = data;
            m_nvme.adminSqPhysical = adminSqPhysical;
            m_nvme.adminCqPhysical = adminCqPhysical;
            m_nvme.ioSqPhysical = ioSqPhysical;
            m_nvme.ioCqPhysical = ioCqPhysical;
            m_nvme.dataPhysical = dataPhysical;
            m_nvme.pageBytes = static_cast<uint32_t>(kPageBytes);
            const Nvme::Result result = Nvme::Initialize(m_nvme);
            if (result != Nvme::Result::Ok) {
                switch (result) {
                    case Nvme::Result::NotSupported: m_detail = "the NVMe controller isn't supported"; break;
                    case Nvme::Result::Corrupt: m_detail = "the NVMe identify data is invalid"; break;
                    case Nvme::Result::Timeout: m_detail = "the NVMe controller timed out"; break;
                    default: m_detail = "the NVMe controller reported an error"; break;
                }
                return false;
            }
            if (m_nvme.lbaSize == 0 || m_nvme.blocks == 0) return false;
            m_blockBytes = m_nvme.lbaSize;
            m_capacityBytes = m_nvme.blocks * m_nvme.lbaSize;
            for (uint32_t i = 0; i < 40 && m_nvme.model[i] != '\0'; ++i) m_model[i] = m_nvme.model[i];
            m_model[40] = '\0';
            return true;
        }

        Storage::Error ControllerBlockDevice::Bind(BootServices* services, EfiHandle deviceHandle) {
            m_stage = ControllerStage::None;
            m_detail = nullptr;
            m_services = services;
            m_pciIo = nullptr;
            m_bar = nullptr;
            m_barBytes = 0;
            m_blockBytes = 0;
            m_capacityBytes = 0;
            m_model[0] = '\0';
            if (services == nullptr || deviceHandle == nullptr) return Fail(ControllerStage::DevicePath, "there is no loaded-image device handle");

            // 1. 设备路径 → PCI 控制器句柄 → PCI I/O 协议
            const ControllerStage locateStage = FindPciDevice(services, deviceHandle, m_pciIo);
            if (locateStage == ControllerStage::DevicePath) return Fail(locateStage, m_detail != nullptr ? m_detail : "the loaded image device path has no PCI controller");
            if (locateStage == ControllerStage::PciIo) return Fail(locateStage, m_detail != nullptr ? m_detail : "the boot controller has no PCI I/O protocol");

            // 2. 类码判定：AHCI 或 NVMe
            uint32_t classValue = 0;
            if (!ReadConfig32(m_pciIo, kPciClassRegister, classValue)) return Fail(ControllerStage::Class, "the PCI class register is unreadable");
            const uint8_t classCode = static_cast<uint8_t>(classValue >> kPciClassShift);
            const uint8_t subclass = static_cast<uint8_t>(classValue >> kPciSubclassShift);
            const uint8_t progIf = static_cast<uint8_t>(classValue >> kPciProgIfShift);
            bool ahci = false;
            bool nvme = false;
            if (classCode == kPciClassMassStorage && subclass == kPciSubclassAhci && progIf == kPciProgIfAhci) ahci = true;
            if (classCode == kPciClassMassStorage && subclass == kPciSubclassNvme && progIf == kPciProgIfNvme) nvme = true;
            if (!ahci && !nvme) return Fail(ControllerStage::Class, "the boot controller isn't an AHCI or NVMe device");

            // 3. 使能 PCI 内存空间与总线主控：固件可能只给配置空间，DMA 与 MMIO 需要这两个位
            uint16_t command = 0;
            if (!ReadConfig16(m_pciIo, kPciCommandRegister, command)) return Fail(ControllerStage::Bar, "the PCI command register is unreadable");
            command = static_cast<uint16_t>(command | kPciCommandMemorySpace | kPciCommandBusMaster);
            if (!WriteConfig16(m_pciIo, kPciCommandRegister, command)) return Fail(ControllerStage::Bar, "the PCI command register is unwritable");

            // 4. BAR：AHCI 用 BAR5，NVMe 用 BAR0；寄存器窗口长度必须覆盖访问范围
            const uint32_t barIndex = ahci ? 5 : 0;
            const uint64_t minimumBytes = ahci ? 0x1000 : 0x2000;
            uint64_t barBegin = 0;
            uint64_t barBytes = 0;
            if (!ReadBar(m_pciIo, barIndex, minimumBytes, barBegin, barBytes)) return Fail(ControllerStage::Bar, "the controller BAR is unusable");
            m_bar = reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(barBegin));
            m_barBytes = barBytes;

            // 5. 初始化控制器：AHCI 或 NVMe 的只读读路径
            bool initialized = false;
            if (ahci) initialized = InitializeAhci(services);
            else initialized = InitializeNvme(services);
            if (!initialized) return Fail(ControllerStage::Init, m_detail != nullptr ? m_detail : "the controller didn't initialize");
            if (m_capacityBytes == 0 || m_blockBytes == 0) return Fail(ControllerStage::Init, "the controller reports no usable capacity");
            return Storage::Error::Ok;
        }

        Storage::Error ControllerBlockDevice::Read(uint64_t offset, void* buffer, uint32_t bytes) {
            if (buffer == nullptr || bytes == 0 || m_blockBytes == 0) return Storage::Error::InvalidArgument;
            if ((offset & (m_blockBytes - 1)) != 0 || (bytes & (m_blockBytes - 1)) != 0) return Storage::Error::InvalidArgument;
            if (offset >= m_capacityBytes || bytes > m_capacityBytes - offset) return Storage::Error::InvalidArgument;
            if (m_ahci.abar != nullptr) return Ahci::Read(m_ahci, offset, buffer, bytes);
            if (m_nvme.registers.read != nullptr) return Nvme::Read(m_nvme, offset, buffer, bytes);
            return Storage::Error::NotSupported;
        }
    }
}
