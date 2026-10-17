/* BiosBlockDevice.hpp
    BaleenCore 的块设备实现：把交权块的 readSectors 服务包成 Storage::BlockDevice

    readSectors 只接受落低 1MiB、16 字节对齐的目的地（Stub 的磁盘服务契约），需要高位缓冲的
    调用方要自备中转；本实现用一段低地址中转窗口完成"窗口读入 → 拷贝到调用方缓冲"的搬运，
    文件系统层看到的仍是普通字节偏移读，这个约束不上升为接口语义
    窗口与容量由 BaleenCore 主流程给出：窗口按内存图挑，容量按读盘服务的 LBA 上限声明

    写盘服务同样是同步的固件块写；交权块里没有缓存刷新原语，刷新服务由主流程另行绑入
    （当前实现是 Src/DiskFlush.cpp：AHCI 优先、传统 IDE 兜底，对在用 ATA 盘下发 FLUSH CACHE）。
    没有绑定刷新服务、或绑定方报“无可用原语”的介质（光盘、USB、NVMe）按无缓存通过，
    持久化保证止于“写返回成功 + 读回校验通过”，边界写在 Docs/Specs/Baleen/引导状态记录.md 第五节
*/

#pragma once
#include <stdint.h>

#include <Storage/BlockDevice.hpp>

namespace Baleen {
    namespace Core {
        // 读盘服务签名：与 CoreHandoff::readSectors 一致，成功返回 1、失败返回 0
        using ReadSectorsFn = uint32_t (*)(uint32_t lba, uint32_t count, void* dest, uint32_t sectorBytes);
        // 写盘服务签名：与 CoreHandoff::writeSectors 一致，成功返回 1、失败返回 0
        using WriteSectorsFn = uint32_t (*)(uint32_t lba, uint32_t count, const void* src, uint32_t sectorBytes);
        // 写缓存刷新的结论：由绑定方给出，本层据此决定 Flush 的返回值
        enum class FlushOutcome : uint8_t {
            Unsupported,   // 没有可刷新的原语：按“无缓存”通过
            Flushed,       // 已下发并完成刷新
            Failed,        // 命令出错或等待超时：按写失败处理
        };
        // 写缓存刷新服务签名
        using FlushFn = FlushOutcome (*)();

        // 块设备：逻辑块大小取会话扇区大小；只读
        class BiosBlockDevice final : public Storage::BlockDevice {
        public:
            // 绑定读盘服务、会话扇区大小、低地址中转窗口与容量
            // window 须 16 字节对齐且整段落低 1MiB；windowBytes 须是扇区大小的整数倍，且为 2 的幂
            // 参数非法时保持未绑定，后续操作用 InvalidArgument 拒绝
            void Bind(ReadSectorsFn read, uint32_t sectorBytes, uint8_t* window, uint32_t windowBytes, uint64_t capacityBytes);
            // 绑定写能力：写盘经 Stub 的 writeSectors 服务完成，数据经低地址窗口中转
            // 只读介质不提供该服务，设备保持只读
            void BindWrite(WriteSectorsFn write);
            // 绑定写缓存刷新服务：交权块没有刷新原语，由主流程绑入平台实现（当前是 AHCI）
            // covers 是主流程核验出来的事实：可刷新的端口是不是启动介质所在的设备
            // 刷不到自己写的介质时，那次提交只有设备缓存的保证——这一位就是"能不能声称持久"的唯一来源
            void BindFlush(FlushFn flush, bool covers);
            // 解除绑定
            void Unbind();
            // 逻辑块大小（会话扇区大小）；未绑定返回 0
            uint32_t BlockBytes() const override { return m_blockBytes; }
            // 容量，字节；未绑定返回 0
            uint64_t CapacityBytes() const override { return m_capacityBytes; }
            // 是否可写：绑定写服务后为真
            bool Writable() const override { return m_write != nullptr; }
            // 读 [offset, offset + bytes)：按块对齐；超过窗口的请求分段中转，底层失败返回 Io
            Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override;
            // 写 [offset, offset + bytes)：按块对齐；数据经窗口中转后交写服务，底层失败返回 Io
            Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override;
            // 刷介质缓存：对 AHCI 上的在用 ATA 端口下发 FLUSH CACHE
            // 只读设备直接返回 Ok；没有可刷新端口（光盘、USB、NVMe）按“无可用原语”返回 Ok；
            // 端口在但命令出错或超时返回 Io——那表示这次写的持久化没有保证，按写失败处理
            Storage::Error Flush() override;
            // 只有写服务在、刷新服务在、且刷新确实覆盖本介质时才敢声称提交是持久的
            // 光盘 / USB / NVMe 上没有可刷新的端口；平台上另有 ATA 盘时刷新的是别人，同样不算覆盖
            bool HasFlushPrimitive() const override { return m_write != nullptr && m_flush != nullptr && m_flushCovers; }

        private:
            ReadSectorsFn m_read = nullptr;    // 读盘服务入口
            uint32_t m_blockBytes = 0;         // 逻辑块大小 = 会话扇区大小
            uint8_t* m_window = nullptr;       // 低地址中转窗口
            uint32_t m_windowBytes = 0;        // 窗口字节数
            uint64_t m_capacityBytes = 0;      // 设备容量，字节
            WriteSectorsFn m_write = nullptr;  // 写盘服务入口
            FlushFn m_flush = nullptr;
            bool m_flushCovers = false;      // 刷新的端口是否就是启动介质所在设备（主流程核验）         // 写缓存刷新服务入口；未绑定时按无缓存通过
        };
    }
}
