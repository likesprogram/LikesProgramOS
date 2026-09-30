/* SubDevice.hpp
    偏移视图：把底层块设备的一段字节区间映射为独立的块设备

    用于分区视图与光盘内的 Ext4 镜像文件：这两类上层的偏移基准都从 0 重新开始
    不拥有底层设备，使用期间底层必须保持有效；区间连续性不由本层验证，由调用方
    按卷布局事实（如镜像文件的 extent）确认
*/

#pragma once
#include <stdint.h>

#include <Storage/BlockDevice.hpp>

namespace Storage {
    // 子设备：底层设备上一段对齐区间的独立块设备视图
    class SubDevice final : public BlockDevice {
    public:
        // 默认构造：未绑定状态，除 BlockBytes 与 CapacityBytes 外的操作返回 InvalidArgument
        SubDevice() = default;

        // 绑定 [begin, begin + bytes)：起点须按底层块大小对齐，底层须已绑定，
        // 长度非零且不越界；任一项不满足返回 InvalidArgument 且不修改 out
        // 底层取非 const 引用：读盘会改动设备的内部状态（会话、缓冲、DMA 寄存器），不是逻辑只读
        static Error Open(BlockDevice& device, uint64_t begin, uint64_t bytes, SubDevice& out);

        // 是否已绑定
        bool Bound() const { return m_device != nullptr; }
        // 逻辑块大小取底层
        uint32_t BlockBytes() const override { return m_device != nullptr ? m_device->BlockBytes() : 0; }
        // 区间长度，字节
        uint64_t CapacityBytes() const override { return m_bytes; }
        // 写能力随底层；Baleen 侧视图只读
        bool Writable() const override { return m_device != nullptr && m_device->Writable(); }
        // 区间内读：未绑定、对齐或范围非法返回 InvalidArgument
        Error Read(uint64_t offset, void* buffer, uint32_t bytes) override;
        // 区间内写：未绑定、对齐或范围非法返回 InvalidArgument，底层只读时返回 ReadOnly
        Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override;
        // 底层刷新
        Error Flush() override { return m_device != nullptr ? m_device->Flush() : Error::InvalidArgument; }

    private:
        // 对齐与范围检查：偏移与长度按底层块大小对齐，区间内的 [offset, offset + bytes) 不越界
        Error CheckRange(uint64_t offset, uint32_t bytes) const;

        BlockDevice* m_device = nullptr;   // 底层设备，非拥有，须在使用期间保持有效
        uint64_t m_begin = 0;                    // 区间起点，按底层块大小对齐
        uint64_t m_bytes = 0;                    // 区间长度，字节
    };
}
