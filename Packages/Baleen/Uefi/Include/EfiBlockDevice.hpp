/* EfiBlockDevice.hpp
    把 UEFI 的块设备协议包成 Storage::BlockDevice：引导路径在固件环境里读介质的那一层

    语义与 BIOS 侧的 BiosBlockDevice 对齐：偏移与长度按设备报告的块大小对齐，
    以整个设备（不是分区）为视图，块号从设备起点算——布局描述在介质字节偏移 0x20000，
    两种固件环境因此读到同一处字节
    Flush 直接对应协议的 FlushBlocks：这是固件给出的真实刷新原语，不需要自建控制器路径
    设备报告 IoAlign 大于 1 时，未对齐的调用方缓冲经内部对齐缓冲中转：
    中转按设备块大小成块搬运，不跨调用保留数据，也不改变块设备「偏移与长度按块对齐」的契约
    中转缓冲是固定长度的，因此块大小与对齐要求都以它为上限（4096）：超过的设备在绑定时就拒绝，
    而不是等到某次未对齐调用才失败
    实例由调用方就地构造并持有；不动态分配，不使用虚析构
*/

#pragma once
#include <stdint.h>

#include <EfiTypes.hpp>
#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>

namespace Baleen {
    namespace Uefi {
        // UEFI 块设备：Storage::BlockDevice 在固件环境的实现
        class EfiBlockDevice final : public Storage::BlockDevice {
        public:
            // 绑定一个块设备协议：读介质事实并记下块大小、容量与可写性
            // 协议为空或协议没有介质事实返回 InvalidArgument，介质不在位返回 NotFound
            // 块大小为零、不是 2 的幂、容量无法用 64 位表示，或超过中转缓冲上限（4096）返回 NotSupported
            Storage::Error Bind(BlockIo* blockIo);
            // 逻辑块大小，字节；未绑定返回 0
            uint32_t BlockBytes() const override;
            // 设备容量，字节；未绑定返回 0
            uint64_t CapacityBytes() const override;
            // 是否可写；未绑定返回 false
            bool Writable() const override;
            // 读 [offset, offset + bytes) 到 buffer
            Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override;
            // 写 [offset, offset + bytes)；只读介质返回 ReadOnly
            Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override;
            // 刷新写缓存：直接下发协议的 FlushBlocks
            Storage::Error Flush() override;
            // 固件报的协议版本低于 REVISION2 时没有 FlushBlocks：按"无缓存"处理，与 BIOS 侧同一口径
            bool HasFlushPrimitive() const override;

            // 是否已绑定
            bool Bound() const { return m_blockIo != nullptr; }
            // 介质是否在位；未绑定返回 false
            bool MediaPresent() const;
            // 绑定时的介质标识；未绑定返回 0
            // 介质被换掉后本值仍是旧值，调用方靠一次读写拿到的 NotFound 发现换介质
            uint32_t MediaId() const { return m_mediaId; }
            // 固件要求的缓冲对齐；未绑定返回 0
            uint32_t IoAlign() const { return m_ioAlign; }
            // 是否可移动介质；未绑定返回 false
            bool Removable() const { return m_removable; }

        private:
            // 中转缓冲与单次搬运上限：4096 是固件侧常见的最大块大小
            static constexpr uint32_t kTransferBytes = 4096;

            // 缓冲是否满足固件的对齐要求
            bool Aligned(const void* buffer) const;
            // 做过介质核对之后真正下发一次读；bytes 必须是块大小的整数倍
            Storage::Error ReadAligned(uint64_t offset, void* buffer, uint32_t bytes);
            // 做过介质核对之后真正下发一次写；bytes 必须是块大小的整数倍
            Storage::Error WriteAligned(uint64_t offset, const void* buffer, uint32_t bytes);

            BlockIo* m_blockIo = nullptr;       // 底层协议（不拥有，使用期间由固件保证有效）
            uint32_t m_blockBytes = 0;          // 逻辑块大小
            uint32_t m_mediaId = 0;             // 绑定时的介质标识
            uint32_t m_ioAlign = 0;             // 固件要求的缓冲对齐
            uint64_t m_capacityBytes = 0;       // 设备容量
            bool m_writable = false;            // 是否可写
            bool m_removable = false;           // 是否可移动介质
            // 未对齐调用时的中转缓冲：不跨调用保留数据，只保证对齐与成块搬运
            alignas(kTransferBytes) uint8_t m_bounce[kTransferBytes] = {};
        };
    }
}
