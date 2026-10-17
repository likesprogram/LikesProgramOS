/* UefiMemory.hpp
    固件内存图的捕获、转换与交接：取一次内存图，转成 BootInfo 要的条目数组

    条目数组放在本阶段自己的静态存储里：内核入口按物理地址读它，并在建立页帧分配器时
    把 BootInfo 与这份数组所在的内存标为占用（见 Packages/LikesProgramOS/Core/Src/Memory.cpp），
    因此这里只如实上报固件事实，不做保留性扣减
    哪些类型算可用：常规内存，以及退出启动服务之后归系统的启动服务内存与引导器内存；
    运行期服务、ACPI NVS、MMIO 与持久内存都不上报为可用
    取图缓冲是固件池内存，退出启动服务之前不释放：释放会改变内存图，让手上的 MapKey 作废
*/

#pragma once
#include <stdint.h>

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
            uint32_t Convert();
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
    }
}
