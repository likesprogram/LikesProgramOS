/* EfiBlockDevice.cpp
    EfiBlockDevice 的实现：绑定块设备协议，按块读、写与刷新

    协议要求：缓冲区大小必须是块大小的整数倍，块号按设备起点算；
    介质可能在两次调用之间被换掉，所以每次读写之前重取介质事实并核对介质标识
*/

#include <EfiBlockDevice.hpp>

namespace {
    // 固件状态到存储层错误的映射：区分「参数不对」「设备不支持」「只读」与「底层出错」
    // 介质被换掉会导致本次调用失败，归到 NotFound：调用方需要重新取介质事实
    Storage::Error MapStatus(Baleen::Uefi::EfiStatus status) {
        using Baleen::Uefi::EfiStatus;
        if (status == Baleen::Uefi::kStatusSuccess) return Storage::Error::Ok;
        if (status == Baleen::Uefi::kStatusInvalidParameter) return Storage::Error::InvalidArgument;
        if (status == Baleen::Uefi::kStatusUnsupported) return Storage::Error::NotSupported;
        if (status == Baleen::Uefi::kStatusBadBufferSize) return Storage::Error::InvalidArgument;
        if (status == Baleen::Uefi::kStatusWriteProtected) return Storage::Error::ReadOnly;
        if (status == Baleen::Uefi::kStatusNoMedia) return Storage::Error::NotFound;
        if (status == Baleen::Uefi::kStatusMediaChanged) return Storage::Error::NotFound;
        if (status == Baleen::Uefi::kStatusNotFound) return Storage::Error::NotFound;
        return Storage::Error::Io;
    }

    // 逐字节搬运：不引入 C 运行库，也不需要并发安全的优化
    void CopyBytes(void* destination, const void* source, uint32_t bytes) {
        uint8_t* to = static_cast<uint8_t*>(destination);
        const uint8_t* from = static_cast<const uint8_t*>(source);
        for (uint32_t i = 0; i < bytes; ++i) to[i] = from[i];
    }
}

namespace Baleen {
    namespace Uefi {
        Storage::Error EfiBlockDevice::Bind(BlockIo* blockIo) {
            m_blockIo = nullptr;
            m_blockBytes = 0;
            m_mediaId = 0;
            m_ioAlign = 0;
            m_capacityBytes = 0;
            m_writable = false;
            m_removable = false;
            if (blockIo == nullptr || blockIo->media == nullptr) return Storage::Error::InvalidArgument;
            const BlockIoMedia* media = blockIo->media;
            if (media->mediaPresent == 0) return Storage::Error::NotFound;
            const uint32_t blockBytes = media->blockSize;
            // 逻辑块大小必须是 2 的幂：块设备抽象的这一条是硬要求
            if (blockBytes == 0 || (blockBytes & (blockBytes - 1)) != 0) return Storage::Error::NotSupported;
            // 中转缓冲按块大小成块搬运，因此它也限定了可接受的块大小与对齐上限
            if (blockBytes > kTransferBytes || media->ioAlign > kTransferBytes) return Storage::Error::NotSupported;
            // 容量 = (最后一个块号 + 1) × 块大小：越界即无法用 64 位表示，宁可拒绝也不回绕成零
            if (media->lastBlock >= (~0ull) / blockBytes) return Storage::Error::NotSupported;
            m_blockIo = blockIo;
            m_blockBytes = blockBytes;
            m_mediaId = media->mediaId;
            m_ioAlign = media->ioAlign;
            m_capacityBytes = (media->lastBlock + 1) * static_cast<uint64_t>(blockBytes);
            m_writable = media->readOnly == 0;
            m_removable = media->removableMedia != 0;
            return Storage::Error::Ok;
        }

        uint32_t EfiBlockDevice::BlockBytes() const { return m_blockBytes; }

        uint64_t EfiBlockDevice::CapacityBytes() const { return m_capacityBytes; }

        bool EfiBlockDevice::Writable() const { return m_writable; }

        bool EfiBlockDevice::MediaPresent() const {
            if (m_blockIo == nullptr || m_blockIo->media == nullptr) return false;
            return m_blockIo->media->mediaPresent != 0;
        }

        bool EfiBlockDevice::Aligned(const void* buffer) const {
            if (m_ioAlign <= 1) return true;
            return (reinterpret_cast<uintptr_t>(buffer) % m_ioAlign) == 0;
        }

        Storage::Error EfiBlockDevice::ReadAligned(uint64_t offset, void* buffer, uint32_t bytes) {
            if (m_blockIo->media == nullptr || m_blockIo->media->mediaPresent == 0) return Storage::Error::NotFound;
            if (m_blockIo->media->mediaId != m_mediaId) return Storage::Error::NotFound;
            const EfiLba lba = offset / m_blockBytes;
            return MapStatus(m_blockIo->readBlocks(m_blockIo, m_mediaId, lba, bytes, buffer));
        }

        Storage::Error EfiBlockDevice::WriteAligned(uint64_t offset, const void* buffer, uint32_t bytes) {
            if (m_blockIo->media == nullptr || m_blockIo->media->mediaPresent == 0) return Storage::Error::NotFound;
            if (m_blockIo->media->mediaId != m_mediaId) return Storage::Error::NotFound;
            const EfiLba lba = offset / m_blockBytes;
            return MapStatus(m_blockIo->writeBlocks(m_blockIo, m_mediaId, lba, bytes, buffer));
        }

        Storage::Error EfiBlockDevice::Read(uint64_t offset, void* buffer, uint32_t bytes) {
            if (m_blockIo == nullptr) return Storage::Error::NotSupported;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            // 与 BIOS 侧同一条契约：偏移按块对齐、长度是块的整数倍，越界一律拒绝
            if ((offset % m_blockBytes) != 0 || (bytes % m_blockBytes) != 0) return Storage::Error::InvalidArgument;
            if (offset > m_capacityBytes || bytes > m_capacityBytes - offset) return Storage::Error::InvalidArgument;
            if (Aligned(buffer)) return ReadAligned(offset, buffer, bytes);
            uint8_t* out = static_cast<uint8_t*>(buffer);
            uint64_t at = offset;
            uint32_t left = bytes;
            while (left > 0) {
                const uint32_t step = left < kTransferBytes ? left : kTransferBytes;
                const Storage::Error error = ReadAligned(at, m_bounce, step);
                if (error != Storage::Error::Ok) return error;
                CopyBytes(out, m_bounce, step);
                out += step;
                at += step;
                left -= step;
            }
            return Storage::Error::Ok;
        }

        Storage::Error EfiBlockDevice::Write(uint64_t offset, const void* buffer, uint32_t bytes) {
            if (m_blockIo == nullptr) return Storage::Error::NotSupported;
            // 参数先校验再看只读：只读设备上参数非法同样报参数非法，调用方按契约分派即可
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            if ((offset % m_blockBytes) != 0 || (bytes % m_blockBytes) != 0) return Storage::Error::InvalidArgument;
            if (offset > m_capacityBytes || bytes > m_capacityBytes - offset) return Storage::Error::InvalidArgument;
            if (!m_writable) return Storage::Error::ReadOnly;
            if (Aligned(buffer)) return WriteAligned(offset, buffer, bytes);
            const uint8_t* in = static_cast<const uint8_t*>(buffer);
            uint64_t at = offset;
            uint32_t left = bytes;
            while (left > 0) {
                const uint32_t step = left < kTransferBytes ? left : kTransferBytes;
                CopyBytes(m_bounce, in, step);
                const Storage::Error error = WriteAligned(at, m_bounce, step);
                if (error != Storage::Error::Ok) return error;
                in += step;
                at += step;
                left -= step;
            }
            return Storage::Error::Ok;
        }

        bool EfiBlockDevice::HasFlushPrimitive() const {
            if (m_blockIo == nullptr) return false;
            // REVISION2 起才有 FlushBlocks：更老的协议上那个成员不存在，不能碰
            return m_blockIo->revision >= Baleen::Uefi::kBlockIoRevision2;
        }

        Storage::Error EfiBlockDevice::Flush() {
            if (m_blockIo == nullptr) return Storage::Error::NotSupported;
            // 没有刷新原语（老协议版本，或固件对该服务报 Unsupported）时按"无缓存"通过：
            // 这与 BIOS 侧完全同一口径——写入仍然成立，只是那次提交可能只落在设备缓存里，
            // 掉电会让它回退。把"没有原语"当成"刷盘失败"会让整台机器在那种固件上永远做不出状态转换
            if (!HasFlushPrimitive()) return Storage::Error::Ok;
            // 与读写同一条核对：介质不在位或已被换掉时不下发，交由调用方重新取介质事实
            if (m_blockIo->media == nullptr || m_blockIo->media->mediaPresent == 0) return Storage::Error::NotFound;
            if (m_blockIo->media->mediaId != m_mediaId) return Storage::Error::NotFound;
            const Baleen::Uefi::EfiStatus status = m_blockIo->flushBlocks(m_blockIo, m_mediaId);
            if (status == Baleen::Uefi::kStatusUnsupported) return Storage::Error::Ok;
            return MapStatus(status);
        }
    }
}
