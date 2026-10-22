/* UefiMemory.hpp
    固件内存图的捕获、转换与交接，以及交接缓冲的落点分配

    条目数组放在本阶段自己的静态存储里：内核入口按物理地址读它，并在建立页帧分配器时
    把 BootInfo 与这份数组所在的内存标为占用（见 Packages/LikesProgramOS/Core/Src/Memory.cpp），
    因此这里只如实上报固件事实，不做保留性扣减
    条目有两种视角，按取图快照的用途选择：
      - 退出启动服务之前：只有常规内存算可用；启动服务内存与引导器内存仍在固件手里，
        不能当可写目标（本路径的内核镜像因此先放在 AllocatePages 分配的中转缓冲里，
        不使用这份视角去核对固定装载地址）
      - 退出启动服务之后：常规内存、启动服务内存与引导器内存都归系统使用；
        运行期服务、ACPI NVS、MMIO 与持久内存始终不上报为可用
        UEFI 装载路径用这份视角核对内核的固定装载跨度并构造 BootInfo
    取图缓冲是固件池内存，退出启动服务之前不释放：释放会改变内存图，让手上的 MapKey 作废

    交接缓冲（清单副本、列表副本、记录数组、模块字节）由 UefiAllocator 分配：
    Baleen 映像可能被固件装到 4GiB 以上，而内核只按低 4GiB 恒等映射读这些缓冲，
    因此它们必须落在低 4GiB，且失败时要能逐段归还
*/

#pragma once
#include <stdint.h>

#include <BootClosure.hpp>
#include <BootInfo.hpp>
#include <Diagnostics.hpp>
#include <EfiTypes.hpp>

namespace Baleen {
    namespace Uefi {
        // 固件内存图：本阶段静态持有，负责取图、转换与交出 MapKey
        class MemoryMap {
        public:
            // 条目上限：固件的描述符比 BIOS 的 E820 碎得多，给足余量；超出部分置截断标志
            static constexpr uint32_t kMaxEntries = 128;

            // 分配取图缓冲；可重复调用，重复调用时先释放旧缓冲
            Diagnostic Allocate(BootServices* bootServices);
            // 取一次内存图，记下 MapKey 与描述符数；缓冲不够时按固件回报的长度重新分配后再取
            Diagnostic Capture(BootServices* bootServices);
            // 把最近一次取到的内存图转成条目数组，返回转换后的条目数；不调用固件服务
            // afterExit 决定启动服务内存与引导器内存算不算可用：
            // false = 退出前的当前归属视图（只有常规内存可用），true = 退出后交给系统的视图；
            // UEFI 装载路径在 ExitBootServices 之后再转换并核对固定装载跨度，两种视角都从同一份快照转换
            uint32_t Convert(bool afterExit);
            // 释放取图缓冲；退出启动服务之后调用无效（那时固件服务已不可用）
            void Release(BootServices* bootServices);

            // 最近一次取图的 MapKey：退出启动服务时要原样交回
            uint64_t Key() const { return m_key; }
            // 转换后的条目数
            uint32_t Count() const { return m_count; }
            // 描述符数超出条目上限时为真
            bool Truncated() const { return m_truncated; }
            // 转换后的条目数组；交给内核的物理地址按运行期地址取
            const Boot::MemoryMapEntry* Entries() const { return m_entries; }

        private:
            BootServices* m_bootServices = nullptr;   // 启动服务表（不拥有，使用期间须有效）
            void* m_buffer = nullptr;                 // 取图缓冲：固件池内存，由本类释放
            uint64_t m_bufferBytes = 0;               // 缓冲长度
            uint64_t m_mapBytes = 0;                  // 最近一次取到的内存图字节数
            uint64_t m_descriptorBytes = 0;           // 单个描述符的长度，由固件回报
            uint64_t m_key = 0;                       // 最近一次取图的 MapKey
            uint32_t m_count = 0;                     // 转换后的条目数
            bool m_truncated = false;                 // 是否因条目上限而截断
            // 转换后的条目数组：静态存储，交接后由内核按物理地址读取
            alignas(16) Boot::MemoryMapEntry m_entries[kMaxEntries] = {};
        };

        // 交接缓冲分配器：预装机制的落点分配在 UEFI 侧走固件页分配
        // 清单副本、列表副本、记录数组与模块字节都经它分配，全部限在低 4GiB（内核按恒等映射读）；
        // 每次分配都记进已分配表，失败路径按表 FreePages 回收，成功交权后不再归还——
        // 那几段内存随 BootInfo 交给内核，内核入口按记录把它们标为已占用
        class UefiAllocator final : public Baleen::Core::SpanAllocator {
        public:
            // 已分配区间的槽位上限：30 个模块落点加上清单 / 列表 / 记录三份副本，留余量
            static constexpr uint32_t kMaxSpans = Baleen::Core::kPreloadSlots + 4;

            // 绑定启动服务表：使用期间须有效，退出启动服务之后不可再调用
            void Install(BootServices* bootServices) { m_bootServices = bootServices; }
            // 分配一段 4 KiB 对齐、整段落在低 4GiB 的落点；失败返回 false
            bool Allocate(uint32_t bytes, Baleen::Core::PreloadSpan& span) override;
            // 归还一段落点：按记录的页数 FreePages，无论固件是否接受都从已分配表里移除
            void Release(const Baleen::Core::PreloadSpan& span) override;
            // 归还全部已分配落点：装载 / 预装失败的路径用；成功交权前不得调用
            void ReleaseAll();
            // 当前已分配的区间数量；诊断与自测用
            uint32_t Count() const { return m_count; }

        private:
            // 已分配区间：物理起址与页数，ReleaseAll 按它逐段归还
            struct Span {
                uint64_t phys;    // 物理起址
                uint32_t pages;   // 页数
            };
            BootServices* m_bootServices = nullptr;   // 启动服务表（不拥有，退出启动服务后失效）
            Span m_spans[kMaxSpans] = {};             // 已分配区间
            uint32_t m_count = 0;                     // 已分配区间数量
        };
    }
}
