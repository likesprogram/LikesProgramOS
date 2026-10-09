/* BiosBlockDevice.hpp
    BaleenCore 的块设备实现：把交权块的 readSectors 服务包成 Storage::BlockDevice

    readSectors 只接受落低 1MiB、16 字节对齐的目的地（Stub 的磁盘服务契约），需要高位缓冲的
    调用方要自备中转；本实现用一段低地址中转窗口完成"窗口读入 → 拷贝到调用方缓冲"的搬运，
    文件系统层看到的仍是普通字节偏移读，这个约束不上升为接口语义
    窗口与容量由 BaleenCore 主流程给出：窗口按内存图挑，容量按读盘服务的 LBA 上限声明
*/

#pragma once
#include <stdint.h>

#include <Storage/BlockDevice.hpp>

namespace Baleen {
    namespace Core {
        // 读盘服务签名：与 CoreHandoff::readSectors 一致，成功返回 1、失败返回 0
        using ReadSectorsFn = uint32_t (*)(uint32_t lba, uint32_t count, void* dest, uint32_t sectorBytes);

        // 块设备：逻辑块大小取会话扇区大小；只读
        class BiosBlockDevice final : public Storage::BlockDevice {
        public:
            // 绑定读盘服务、会话扇区大小、低地址中转窗口与容量
            // window 须 16 字节对齐且整段落低 1MiB；windowBytes 须是扇区大小的整数倍，且为 2 的幂
            // 参数非法时保持未绑定，后续操作用 InvalidArgument 拒绝
            void Bind(ReadSectorsFn read, uint32_t sectorBytes, uint8_t* window, uint32_t windowBytes, uint64_t capacityBytes);
            // 解除绑定
            void Unbind();
            // 逻辑块大小（会话扇区大小）；未绑定返回 0
            uint32_t BlockBytes() const override { return m_blockBytes; }
            // 容量，字节；未绑定返回 0
            uint64_t CapacityBytes() const override { return m_capacityBytes; }
            // 只读设备
            bool Writable() const override { return false; }
            // 读 [offset, offset + bytes)：按块对齐；超过窗口的请求分段中转，底层失败返回 Io
            Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override;

        private:
            ReadSectorsFn m_read = nullptr;    // 读盘服务入口
            uint32_t m_blockBytes = 0;         // 逻辑块大小 = 会话扇区大小
            uint8_t* m_window = nullptr;       // 低地址中转窗口
            uint32_t m_windowBytes = 0;        // 窗口字节数
            uint64_t m_capacityBytes = 0;      // 设备容量，字节
        };
    }
}
