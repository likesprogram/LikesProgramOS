/* UefiMemory.cpp
    MemoryMap 的实现：分配取图缓冲、反复取图、把描述符转成 BootInfo 的条目；
    UefiAllocator 的实现：交接缓冲的页分配、逐段归还与失败路径的整体回收
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

        bool UefiAllocator::Allocate(uint32_t bytes, Baleen::Core::PreloadSpan& span) {
            span = Baleen::Core::PreloadSpan{};
            if (m_bootServices == nullptr || m_bootServices->allocatePages == nullptr) return false;
            if (bytes == 0 || m_count >= kMaxSpans) return false;
            const uint64_t pages = (static_cast<uint64_t>(bytes) + kPageBytes - 1) / kPageBytes;
            // 上界取低 4GiB 的最高字节：固件把整段分配在它之下，页对齐后必然整段落在恒等映射里
            uint64_t address = kLowMemoryLimit - 1;
            if (IsError(m_bootServices->allocatePages(kAllocateMaxAddress, kMemoryLoaderData, pages, &address)) || address == 0) return false;
            const uint64_t total = pages * kPageBytes;
            // 固件承诺“分配在给定地址之下”，这里再自核一次：整段在低 4GiB 内且不回绕
            if (address >= kLowMemoryLimit || total > kLowMemoryLimit - address) {
                m_bootServices->freePages(address, pages);
                return false;
            }
            span.phys = address;
            // 引导期固件按恒等映射运行，物理地址可直接当可写指针用（与内核镜像中转缓冲同一写法）
            span.data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
            span.bytes = bytes;
            m_spans[m_count].phys = address;
            m_spans[m_count].pages = static_cast<uint32_t>(pages);
            ++m_count;
            return true;
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
