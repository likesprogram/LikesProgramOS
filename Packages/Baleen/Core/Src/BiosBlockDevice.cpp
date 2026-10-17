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
            m_write = nullptr;
        }

        void BiosBlockDevice::BindWrite(WriteSectorsFn write) {
            m_write = write;
        }

        void BiosBlockDevice::BindFlush(FlushFn flush, bool covers) {
            m_flush = flush;
            m_flushCovers = flush != nullptr && covers;
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

        Storage::Error BiosBlockDevice::Write(uint64_t offset, const void* buffer, uint32_t bytes) {
            if (m_write == nullptr || m_window == nullptr || m_blockBytes == 0 || m_windowBytes == 0) return Storage::Error::ReadOnly;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            if ((offset & (m_blockBytes - 1)) != 0) return Storage::Error::InvalidArgument;
            if ((bytes & (m_blockBytes - 1)) != 0) return Storage::Error::InvalidArgument;
            if (offset > m_capacityBytes || static_cast<uint64_t>(bytes) > m_capacityBytes - offset) return Storage::Error::InvalidArgument;
            // 写服务只接受落低 1MiB 的源：数据先拷进中转窗口，再按窗口容量分段交出去
            const uint32_t shift = SectorShift(m_blockBytes);
            const uint8_t* source = static_cast<const uint8_t*>(buffer);
            uint64_t position = offset;
            uint32_t remaining = bytes;
            while (remaining > 0) {
                const uint32_t chunk = remaining < m_windowBytes ? remaining : m_windowBytes;
                if ((position >> 32) != 0) return Storage::Error::InvalidArgument;
                memcpy(m_window, source, chunk);
                const uint32_t lba = static_cast<uint32_t>(position >> shift);
                if (m_write(lba, chunk >> shift, m_window, m_blockBytes) == 0) return Storage::Error::Io;
                source += chunk;
                position += chunk;
                remaining -= chunk;
            }
            return Storage::Error::Ok;
        }

        Storage::Error BiosBlockDevice::Flush() {
            // 只读设备没有写缓存要刷；没有绑定刷新服务时同样按“无可用原语”通过
            if (m_write == nullptr || m_flush == nullptr) return Storage::Error::Ok;
            // 绑定方报“不支持”（光盘、USB、NVMe 上没有可刷新的端口）时通过，报失败时按写失败处理：
            // 那表示这次写的持久化没有保证，调用方按写失败降级
            if (m_flush() == FlushOutcome::Failed) return Storage::Error::Io;
            return Storage::Error::Ok;
        }
    }
}
