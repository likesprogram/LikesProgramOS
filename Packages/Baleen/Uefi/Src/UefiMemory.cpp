/* UefiMemory.cpp
    MemoryMap 的实现：分配取图缓冲、反复取图、把描述符转成 BootInfo 的条目
*/

#include <UefiMemory.hpp>

#include <UefiDiagnostics.hpp>

namespace {
    // 取图缓冲的初始余量：固件的描述符数量在两次取图之间可能变大，多要一些省一次重分配
    constexpr uint32_t kDescriptorMargin = 16;

    // 按小端把描述符里的物理起址与页数换算成字节长度：页大小固定 4096
    uint64_t RegionBytes(uint64_t pages) { return pages * Baleen::Uefi::kPageBytes; }

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

        uint32_t MemoryMap::Convert() {
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
                m_entries[m_count].base = descriptor->physicalStart;
                m_entries[m_count].length = RegionBytes(descriptor->pages);
                m_entries[m_count].type = UsableAfterExit(descriptor->type) ? 1u : 2u;
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
    }
}
