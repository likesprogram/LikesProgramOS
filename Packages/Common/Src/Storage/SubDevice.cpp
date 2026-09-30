/* SubDevice.cpp
    偏移视图的绑定校验与区间转发实现
*/

#include <Storage/SubDevice.hpp>

namespace Storage {
    Error SubDevice::Open(BlockDevice& device, uint64_t begin, uint64_t bytes, SubDevice& out) {
        const uint32_t blockBytes = device.BlockBytes();
        // 底层没有块大小或不是 2 的幂时无法判定对齐，按不支持处理
        if (blockBytes == 0 || (blockBytes & (blockBytes - 1)) != 0) return Error::InvalidArgument;
        if (bytes == 0) return Error::InvalidArgument;
        // 对齐与边界都用位与和减法判定：32 位目标上不牵入 64 位除法
        if ((begin & (blockBytes - 1)) != 0) return Error::InvalidArgument;
        const uint64_t capacity = device.CapacityBytes();
        if (begin > capacity || bytes > capacity - begin) return Error::InvalidArgument;
        out.m_device = &device;
        out.m_begin = begin;
        out.m_bytes = bytes;
        return Error::Ok;
    }

    Error SubDevice::CheckRange(uint64_t offset, uint32_t bytes) const {
        const uint32_t blockBytes = BlockBytes();
        if (blockBytes == 0 || bytes == 0) return Error::InvalidArgument;
        if ((offset & (blockBytes - 1)) != 0) return Error::InvalidArgument;
        if ((bytes & (blockBytes - 1)) != 0) return Error::InvalidArgument;
        if (offset > m_bytes || bytes > m_bytes - offset) return Error::InvalidArgument;
        return Error::Ok;
    }

    Error SubDevice::Read(uint64_t offset, void* buffer, uint32_t bytes) {
        if (m_device == nullptr || buffer == nullptr) return Error::InvalidArgument;
        if (const Error error = CheckRange(offset, bytes); error != Error::Ok) return error;
        return m_device->Read(m_begin + offset, buffer, bytes);
    }

    Error SubDevice::Write(uint64_t offset, const void* buffer, uint32_t bytes) {
        if (m_device == nullptr || buffer == nullptr) return Error::InvalidArgument;
        if (!m_device->Writable()) return Error::ReadOnly;
        if (const Error error = CheckRange(offset, bytes); error != Error::Ok) return error;
        return m_device->Write(m_begin + offset, buffer, bytes);
    }
}
