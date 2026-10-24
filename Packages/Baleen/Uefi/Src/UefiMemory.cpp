/* UefiMemory.cpp
    MemoryMap 的实现：分配取图缓冲、反复取图、把描述符转成 BootInfo 的条目；
    UefiAllocator 的实现：交接缓冲的页分配（低 4GiB、避开低 1 MiB 与两段装载跨度）、
    逐段归还与失败路径的整体回收
*/

#include <UefiMemory.hpp>

#include <UefiDiagnostics.hpp>

namespace {
    // 取图缓冲的初始余量：固件的描述符数量在两次取图之间可能变大，多要一些省一次重分配
    constexpr uint32_t kDescriptorMargin = 16;

    // 按小端把描述符里的物理起址与页数换算成字节长度：页大小固定 4096
    uint64_t RegionBytes(uint64_t pages) { return pages * Baleen::Uefi::kPageBytes; }

    // 退出启动服务之前只有常规内存算可用：装载跨度必须落在它上面，
    // 启动服务内存与引导器内存这时还在固件手里，不能当可写目标
    bool UsableBeforeExit(uint32_t type) {
        return type == Baleen::Uefi::kMemoryConventional;
    }

    // 该类型的内存在退出启动服务之后是否归系统使用
    bool UsableAfterExit(uint32_t type) {
        using namespace Baleen::Uefi;
        return type == kMemoryConventional || type == kMemoryBootServicesCode || type == kMemoryBootServicesData ||
               type == kMemoryLoaderCode || type == kMemoryLoaderData;
    }

    // 避让重试的轮数上限：每次相交把分配上界降到相交跨度起点之下，
    // 引导器与内核两段跨度各最多消耗一轮，4 轮留一倍余量，也防止固件行为异常时死循环
    constexpr uint32_t kAvoidRounds = 4;
}

namespace Baleen {
    namespace Uefi {
        Diagnostic MemoryMap::Allocate(BootServices* bootServices) {
            if (bootServices == nullptr) return kFailMemoryMapInit;
            if (bootServices->allocatePool == nullptr || bootServices->getMemoryMap == nullptr) return kFailMemoryMapInit;
            Release(bootServices);
            m_bootServices = bootServices;
            const uint64_t bytes = (static_cast<uint64_t>(kMaxEntries) + kDescriptorMargin) * sizeof(MemoryDescriptor);
            void* buffer = nullptr;
            if (IsError(bootServices->allocatePool(kPoolLoaderData, bytes, &buffer)) || buffer == nullptr) return kFailMemoryMapInit;
            m_buffer = buffer;
            m_bufferBytes = bytes;
            return Diagnostic{};
        }

        Diagnostic MemoryMap::Capture(BootServices* bootServices) {
            if (bootServices == nullptr || m_buffer == nullptr) return kFailMemoryMapInit;
            // 缓冲不够时固件回报所需长度：按它重新分配再取，最多几轮，避免死循环
            for (uint32_t round = 0; round < 4; ++round) {
                uint64_t mapBytes = m_bufferBytes;
                uint64_t key = 0;
                uint64_t descriptorBytes = 0;
                uint32_t version = 0;
                const EfiStatus status = bootServices->getMemoryMap(&mapBytes, static_cast<MemoryDescriptor*>(m_buffer), &key, &descriptorBytes, &version);
                if (!IsError(status)) {
                    if (descriptorBytes < sizeof(MemoryDescriptor)) return kFailMemoryMapFormat;
                    m_mapBytes = mapBytes;
                    m_descriptorBytes = descriptorBytes;
                    m_key = key;
                    return Diagnostic{};
                }
                if (status != kStatusBufferTooSmall) return kFailMemoryMapRead;
                const uint64_t step = descriptorBytes != 0 ? descriptorBytes : sizeof(MemoryDescriptor);
                const uint64_t needed = mapBytes + kDescriptorMargin * step;
                bootServices->freePool(m_buffer);
                m_buffer = nullptr;
                m_bufferBytes = 0;
                void* buffer = nullptr;
                if (IsError(bootServices->allocatePool(kPoolLoaderData, needed, &buffer)) || buffer == nullptr) return kFailMemoryMapInit;
                m_buffer = buffer;
                m_bufferBytes = needed;
            }
            return kFailMemoryMapRead;
        }

        uint32_t MemoryMap::Convert(bool afterExit) {
            m_count = 0;
            m_truncated = false;
            if (m_buffer == nullptr || m_descriptorBytes == 0) return 0;
            const uint8_t* const begin = static_cast<const uint8_t*>(m_buffer);
            for (uint64_t offset = 0; offset + sizeof(MemoryDescriptor) <= m_mapBytes; offset += m_descriptorBytes) {
                const MemoryDescriptor* const descriptor = reinterpret_cast<const MemoryDescriptor*>(begin + offset);
                if (m_count >= kMaxEntries) {
                    m_truncated = true;
                    break;
                }
                const bool usable = afterExit ? UsableAfterExit(descriptor->type) : UsableBeforeExit(descriptor->type);
                m_entries[m_count].base = descriptor->physicalStart;
                m_entries[m_count].length = RegionBytes(descriptor->pages);
                m_entries[m_count].type = usable ? 1u : 2u;
                m_entries[m_count].attributes = 1u;   // bit0 置 1：条目有效；内核侧只按类型判定
                ++m_count;
            }
            return m_count;
        }

        void MemoryMap::Release(BootServices* bootServices) {
            if (bootServices != nullptr && m_buffer != nullptr && bootServices->freePool != nullptr) bootServices->freePool(m_buffer);
            m_buffer = nullptr;
            m_bufferBytes = 0;
            m_mapBytes = 0;
            m_descriptorBytes = 0;
            m_key = 0;
        }

        void UefiAllocator::SetAvoid(uint64_t loaderBegin, uint64_t loaderBytes, uint64_t kernelBegin, uint64_t kernelBytes) {
            m_loader.begin = loaderBegin;
            m_loader.bytes = loaderBytes;
            m_kernel.begin = kernelBegin;
            m_kernel.bytes = kernelBytes;
        }

        bool UefiAllocator::AvoidOverlap(uint64_t address, uint64_t bytes, uint64_t& avoidBegin) const {
            // 候选区间整段在低 4GiB 内，不会回绕；跨度一侧用减法判定相交，避免 begin + bytes 回绕
            const AvoidSpan* const spans[] = {&m_loader, &m_kernel};
            bool hit = false;
            for (const AvoidSpan* const avoid : spans) {
                if (avoid->bytes == 0) continue;   // 0 长度表示调用方没有该事实
                if (address >= avoid->begin) {
                    if (address - avoid->begin >= avoid->bytes) continue;   // 候选在跨度起点之上：不相交
                } else if (avoid->begin - address >= bytes) {
                    continue;   // 跨度起点在候选末端之外：不相交
                }
                if (!hit || avoid->begin < avoidBegin) avoidBegin = avoid->begin;
                hit = true;
            }
            return hit;
        }

        bool UefiAllocator::Allocate(uint32_t bytes, Baleen::Core::PreloadSpan& span) {
            span = Baleen::Core::PreloadSpan{};
            if (m_bootServices == nullptr || m_bootServices->allocatePages == nullptr) return false;
            if (bytes == 0 || m_count >= kMaxSpans) return false;
            const uint64_t pages = (static_cast<uint64_t>(bytes) + kPageBytes - 1) / kPageBytes;
            const uint64_t total = pages * kPageBytes;
            // 上界取低 4GiB 的最高字节：固件把整段分配在它之下，页对齐后必然整段落在恒等映射里
            uint64_t limit = kLowMemoryLimit - 1;
            // 从高往下的避让重试：落在引导器 / 内核跨度里的候选先还给固件，再把上界降到相交跨度
            // 起点之下重试；轮数固定，避免固件行为异常时死循环
            for (uint32_t round = 0; round < kAvoidRounds; ++round) {
                uint64_t address = limit;
                if (IsError(m_bootServices->allocatePages(kAllocateMaxAddress, kMemoryLoaderData, pages, &address)) || address == 0) return false;
                // 固件承诺“分配在给定地址之下”，这里再自核一次：整段在低 4GiB 内且不回绕
                if (address >= kLowMemoryLimit || total > kLowMemoryLimit - address) {
                    m_bootServices->freePages(address, pages);
                    return false;
                }
                // 低 1 MiB 留给固件残留与内核早期代码：上界只能往下调，
                // 固件落到它之下说明这个上界下已经拿不到合规落点，再降只会更差，直接失败
                if (address < Baleen::Core::kPreloadLowLimit) {
                    m_bootServices->freePages(address, pages);
                    return false;
                }
                // 与引导器或内核的装载跨度相交：先还给固件，绝不登记进已分配表；
                // 再把上界降到相交跨度起点之下重试，整段分配在上界之下的前提下降一次即可
                uint64_t avoidBegin = 0;
                if (AvoidOverlap(address, total, avoidBegin)) {
                    m_bootServices->freePages(address, pages);
                    // 上界必须严格降低，否则下一轮只会拿到同一落点
                    if (avoidBegin == 0 || avoidBegin - 1 >= limit) return false;
                    limit = avoidBegin - 1;
                    continue;
                }
                span.phys = address;
                // 引导期固件按恒等映射运行，物理地址可直接当可写指针用
                span.data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
                span.bytes = bytes;
                m_spans[m_count].phys = address;
                m_spans[m_count].pages = static_cast<uint32_t>(pages);
                ++m_count;
                return true;
            }
            return false;
        }

        void UefiAllocator::Release(const Baleen::Core::PreloadSpan& span) {
            for (uint32_t i = 0; i < m_count; ++i) {
                if (m_spans[i].phys != span.phys) continue;
                const uint32_t pages = m_spans[i].pages;
                for (uint32_t j = i + 1; j < m_count; ++j) m_spans[j - 1] = m_spans[j];
                --m_count;
                if (m_bootServices != nullptr && m_bootServices->freePages != nullptr && pages != 0)
                    m_bootServices->freePages(span.phys, pages);
                return;
            }
        }

        void UefiAllocator::ReleaseAll() {
            for (uint32_t i = m_count; i > 0; --i) {
                const Span& record = m_spans[i - 1];
                if (m_bootServices != nullptr && m_bootServices->freePages != nullptr && record.pages != 0)
                    m_bootServices->freePages(record.phys, record.pages);
            }
            m_count = 0;
        }
    }
}
