/* BlockDevice.hpp
    块设备抽象：固定大小逻辑块的随机访问视图，文件系统层的下层

    逻辑块大小由设备报告，实现不写死 512；本层接受字节偏移与长度，偏移须按块对齐、
    长度须是块的整数倍，跨块与非对齐请求由文件系统层自理
    本层不分配内存、不缓存、不重试；缓冲由调用方提供，只读设备的 Write 返回 ReadOnly
    实例由提供方按具体类型构造与销毁，不经基类指针销毁，因此不需要虚析构
*/

#pragma once
#include <stdint.h>

#include <Storage/Error.hpp>

namespace Storage {
    // 块设备：固定大小逻辑块的随机访问视图
    class BlockDevice {
    public:
        // 逻辑块大小，字节，必须是 2 的幂；未绑定的实现返回 0
        virtual uint32_t BlockBytes() const { return 0; }
        // 设备容量，字节；未绑定的实现返回 0
        virtual uint64_t CapacityBytes() const { return 0; }
        // 是否可写；只读设备不必重写 Write
        virtual bool Writable() const { return false; }
        // 读 [offset, offset + bytes) 到 buffer：偏移或长度未按块对齐、超出容量、指针为空
        // 或长度为零返回 InvalidArgument，底层失败返回 Io，成功返回 Ok
        virtual Error Read(uint64_t offset, void* buffer, uint32_t bytes) {
            (void)offset;
            (void)buffer;
            (void)bytes;
            return Error::NotSupported;
        }
        // 写 [offset, offset + bytes)；默认拒绝，只读设备不必重写
        virtual Error Write(uint64_t offset, const void* buffer, uint32_t bytes) {
            (void)offset;
            (void)buffer;
            (void)bytes;
            return Error::ReadOnly;
        }
        // 刷新写缓存；默认无缓存，返回 Ok
        virtual Error Flush() { return Error::Ok; }

    protected:
        BlockDevice() = default;
        ~BlockDevice() = default;
    };
}
