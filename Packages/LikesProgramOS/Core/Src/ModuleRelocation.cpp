/* ModuleRelocation.cpp
    重定位实现：逐项复核类型、对齐、目标区间与可写段，再写入 Base + Addend

    表区与条目在写入前整表复核一遍：条目数上限、表偏移对齐与表区边界都按 64 位加法检查，
    读取条目沿用 ReadContainerRelocation，本文件不重复解析容器字节格式
    写入是逐字节小端：目标按宽度对齐、映像基址按页对齐，字节写入不依赖宿主或目标侧的
    指针宽度与对齐假设，也不会在严格别名规则下产生未定义行为
*/

#include <ModuleRelocation.hpp>

namespace {
    // 64 位无符号最大值：Base + Addend 溢出判定用
    constexpr uint64_t kMaxValue = ~static_cast<uint64_t>(0);

    // 重定位条目对应的写入宽度：调用前类型已校验
    uint32_t RelocationWidth(uint8_t type) {
        return type == static_cast<uint8_t>(Kernel::ContainerRelocationType::Relative64) ? 8 : 4;
    }

    // 目标区间是否完整落在可写段（DATA / BSS）内
    bool WritableContains(const Kernel::ContainerView& view, uint64_t target, uint64_t width) {
        for (uint32_t index = 0; index < view.sectionCount; ++index) {
            const Kernel::ContainerSection& section = view.sections[index];
            const bool writable = section.type == static_cast<uint32_t>(Kernel::ContainerSectionType::Data)
                || section.type == static_cast<uint32_t>(Kernel::ContainerSectionType::Bss);
            if (!writable) continue;
            if (target < section.memoryOffset) continue;
            if (width > section.memoryBytes) continue;
            if (target - section.memoryOffset > section.memoryBytes - width) continue;
            return true;
        }
        return false;
    }

    // 小端写入 value 的低 width 字节
    void WriteLittleEndian(uint8_t* target, uint64_t value, uint32_t width) {
        for (uint32_t index = 0; index < width; ++index) target[index] = static_cast<uint8_t>(value >> (8 * index));
    }
}

namespace Kernel {
    RelocationError ApplyModuleRelocations(const ContainerView& view, const ModuleImage& image) {
        if (view.bytes == nullptr) return RelocationError::NullPointer;
        if (image.base == 0 || image.base % kPageBytes != 0 || image.memory == nullptr
            || image.imageBytes == 0 || image.imageBytes != view.imageBytes) {
            return RelocationError::ImageMismatch;
        }
        if (view.relocationCount > kContainerMaxRelocations) return RelocationError::Table;
        if (view.relocationCount != 0) {
            const uint64_t tableBytes = static_cast<uint64_t>(view.relocationCount) * kContainerRelocationEntryBytes;
            if (view.relocationTableOffset % 8 != 0) return RelocationError::Table;
            if (view.relocationTableOffset < kContainerHeaderBytes || view.relocationTableOffset >= view.bytesCount) {
                return RelocationError::Table;
            }
            if (tableBytes > view.bytesCount - view.relocationTableOffset) return RelocationError::Table;
        }
        for (uint32_t index = 0; index < view.relocationCount; ++index) {
            ContainerRelocation relocation{};
            if (ReadContainerRelocation(view, index, relocation) != ContainerError::None) return RelocationError::Table;
            const uint8_t type = relocation.type;
            if (type != static_cast<uint8_t>(ContainerRelocationType::Relative64)
                && type != static_cast<uint8_t>(ContainerRelocationType::Relative32)) {
                return RelocationError::UnknownType;
            }
            const uint32_t width = RelocationWidth(type);
            if (relocation.target % width != 0) return RelocationError::TargetAlignment;
            if (relocation.target > image.imageBytes || width > image.imageBytes - relocation.target) {
                return RelocationError::TargetRange;
            }
            if (!WritableContains(view, relocation.target, width)) return RelocationError::TargetSection;
            if (relocation.addend > kMaxValue - image.base) return RelocationError::AddressOverflow;
            const uint64_t value = image.base + relocation.addend;
            if (type == static_cast<uint8_t>(ContainerRelocationType::Relative32) && value > 0xFFFFFFFFull) {
                return RelocationError::Result32;
            }
            WriteLittleEndian(image.memory + relocation.target, value, width);
        }
        return RelocationError::None;
    }

    const char* RelocationErrorText(RelocationError error) {
        switch (error) {
            case RelocationError::None: return "";
            case RelocationError::NullPointer: return "the module relocation view has no container bytes";
            case RelocationError::ImageMismatch: return "the mapped image doesn't match the container view";
            case RelocationError::Table: return "the module relocation table is out of range";
            case RelocationError::UnknownType: return "a module relocation type isn't supported";
            case RelocationError::TargetAlignment: return "a relocation target isn't aligned to its width";
            case RelocationError::TargetRange: return "a relocation target runs past the image span";
            case RelocationError::TargetSection: return "a relocation target isn't inside a writable section";
            case RelocationError::AddressOverflow: return "a relocation base plus addend overflows 64 bits";
            case RelocationError::Result32: return "a RELATIVE32 relocation result doesn't fit 32 bits";
        }
        return "the module relocation error is unknown";
    }
}
