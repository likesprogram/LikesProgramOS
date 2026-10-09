/* BiosBlockDevice.cpp
    BaleenCore 块设备适配器的实现：分段中转与对齐校验
*/

#include <BiosBlockDevice.hpp>

#include <Libc.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            // 会话扇区大小的移位量：扇区大小都是 2 的幂，换算用移位
            uint32_t SectorShift(uint32_t sectorBytes) {
                return static_cast<uint32_t>(__builtin_ctz(sectorBytes));
            }
        }

        void BiosBlockDevice::Bind(ReadSectorsFn read, uint32_t sectorBytes, uint8_t* window, uint32_t windowBytes, uint64_t capacityBytes) {
            Unbind();
            // 非法参数保持未绑定：读盘服务与窗口必须存在，扇区大小与窗口大小必须是 2 的幂
            if (read == nullptr || window == nullptr) return;
            if (sectorBytes == 0 || (sectorBytes & (sectorBytes - 1)) != 0) return;
            if (windowBytes == 0 || (windowBytes & (windowBytes - 1)) != 0) return;
            if (windowBytes < sectorBytes) return;
            if ((reinterpret_cast<uintptr_t>(window) & 15) != 0) return;   // 读盘服务的 16 字节对齐前提
            m_read = read;
            m_blockBytes = sectorBytes;
            m_window = window;
            m_windowBytes = windowBytes;
            m_capacityBytes = capacityBytes;
        }

        void BiosBlockDevice::Unbind() {
            m_read = nullptr;
            m_blockBytes = 0;
            m_window = nullptr;
            m_windowBytes = 0;
            m_capacityBytes = 0;
        }

        Storage::Error BiosBlockDevice::Read(uint64_t offset, void* buffer, uint32_t bytes) {
            if (m_read == nullptr || m_window == nullptr || m_blockBytes == 0 || m_windowBytes == 0) return Storage::Error::InvalidArgument;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            if ((offset & (m_blockBytes - 1)) != 0) return Storage::Error::InvalidArgument;
            if ((bytes & (m_blockBytes - 1)) != 0) return Storage::Error::InvalidArgument;
            if (offset > m_capacityBytes || static_cast<uint64_t>(bytes) > m_capacityBytes - offset) return Storage::Error::InvalidArgument;
            const uint32_t shift = SectorShift(m_blockBytes);
            uint8_t* dest = static_cast<uint8_t*>(buffer);
            uint64_t position = offset;
            uint32_t remaining = bytes;
            while (remaining > 0) {
                const uint32_t chunk = remaining < m_windowBytes ? remaining : m_windowBytes;
                // 读盘服务的 LBA 是 32 位：超过的部分由容量声明拦住，这里再核一次
                if ((position >> 32) != 0) return Storage::Error::InvalidArgument;
                const uint32_t lba = static_cast<uint32_t>(position >> shift);
                if (m_read(lba, chunk >> shift, m_window, m_blockBytes) == 0) return Storage::Error::Io;
                memcpy(dest, m_window, chunk);
                dest += chunk;
                position += chunk;
                remaining -= chunk;
            }
            return Storage::Error::Ok;
        }
    }
}
