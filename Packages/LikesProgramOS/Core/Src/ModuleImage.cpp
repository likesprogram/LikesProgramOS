/* ModuleImage.cpp
    映像映射实现：复核容器视图的段表，取连续页帧，整段清零后逐段拷贝文件字节

    校验顺序：段类型与文件字节组合、段内存区间与重叠 → 分配连续页 → 基址与恒等映射窗口
    → 清零并拷贝；先校验再分配，非法视图不占用页帧，分配后失败则立刻归还
    地址换算统一走 Kernel::PhysToVirt：当前低 4 GiB 同时有恒等映射与直映射窗口，
    按直映射窗口写页帧内容不依赖调用方此前映射过哪一段
*/

#include <ModuleImage.hpp>

#include <Libc.hpp>
#include <Paging.hpp>

namespace {
    // 段类型是否合法：与容器段类型的四种取值一致
    bool KnownType(uint32_t type) {
        return type == static_cast<uint32_t>(Kernel::ContainerSectionType::Text)
            || type == static_cast<uint32_t>(Kernel::ContainerSectionType::Rodata)
            || type == static_cast<uint32_t>(Kernel::ContainerSectionType::Data)
            || type == static_cast<uint32_t>(Kernel::ContainerSectionType::Bss);
    }

    // 段的文件字节与类型是否自洽：BSS 不占文件字节，其他段必须有且不超过内存跨度
    bool FileBytesValid(const Kernel::ContainerSection& section) {
        if (section.type == static_cast<uint32_t>(Kernel::ContainerSectionType::Bss)) {
            return section.fileOffset == 0 && section.fileBytes == 0;
        }
        return section.fileBytes != 0 && section.fileBytes <= section.memoryBytes;
    }

    // 复核段表：类型、文件字节、文件区间、内存区间与内存重叠；只读视图，不写内存
    Kernel::ImageError ValidateSections(const Kernel::ContainerView& view) {
        if (view.sectionCount == 0 || view.sectionCount > Kernel::kContainerMaxSections) return Kernel::ImageError::Section;
        for (uint32_t index = 0; index < view.sectionCount; ++index) {
            const Kernel::ContainerSection& section = view.sections[index];
            if (!KnownType(section.type)) return Kernel::ImageError::Section;
            // 段内存区间：按页对齐、非零、整段落在 ImageBytes 内
            if (section.memoryOffset % Kernel::kContainerPageBytes != 0) return Kernel::ImageError::SectionSpan;
            if (section.memoryBytes == 0 || section.memoryBytes % Kernel::kContainerPageBytes != 0) return Kernel::ImageError::SectionSpan;
            if (static_cast<uint64_t>(section.memoryOffset) + section.memoryBytes > view.imageBytes) return Kernel::ImageError::SectionSpan;
            if (!FileBytesValid(section)) return Kernel::ImageError::Section;
            // 段文件区间：非 BSS 段从文件头之后开始，整段落在容器字节内
            if (section.fileBytes != 0) {
                if (section.fileOffset < Kernel::kContainerHeaderBytes) return Kernel::ImageError::Section;
                if (static_cast<uint64_t>(section.fileOffset) + section.fileBytes > view.bytesCount) return Kernel::ImageError::Section;
            }
        }
        // 段内存区间两两不重叠：读取方不依赖段表顺序
        for (uint32_t first = 0; first < view.sectionCount; ++first) {
            for (uint32_t second = first + 1; second < view.sectionCount; ++second) {
                const uint64_t firstBegin = view.sections[first].memoryOffset;
                const uint64_t secondBegin = view.sections[second].memoryOffset;
                if (firstBegin < secondBegin + view.sections[second].memoryBytes
                    && secondBegin < firstBegin + view.sections[first].memoryBytes) {
                    return Kernel::ImageError::SectionSpan;
                }
            }
        }
        return Kernel::ImageError::None;
    }
}

namespace Kernel {
    ImageError MapModuleImage(const ContainerView& view, ImagePlatform& platform, ModuleImage& image) {
        image = ModuleImage{};
        if (view.bytes == nullptr || platform.acquire == nullptr || platform.release == nullptr
            || platform.map == nullptr) {
            return ImageError::NullPointer;
        }
        if (view.imageBytes == 0 || view.imageBytes % kContainerPageBytes != 0
            || view.imageBytes > kContainerMaxTotalBytes) {
            return ImageError::ImageBytes;
        }
        if (const ImageError error = ValidateSections(view); error != ImageError::None) return error;
        const uint32_t pageCount = view.imageBytes / kContainerPageBytes;
        const uint64_t base = platform.acquire(platform.context, pageCount);
        if (base == 0) return ImageError::NoPages;
        // 基址与恒等映射窗口：分配器只给低 4 GiB 的页，仍按映像跨度复核一次
        if (base % kPageBytes != 0 || base >= kMapLimitBytes
            || static_cast<uint64_t>(view.imageBytes) > kMapLimitBytes - base) {
            platform.release(platform.context, base, pageCount);
            return ImageError::BaseRange;
        }
        uint8_t* const imageBase = static_cast<uint8_t*>(platform.map(platform.context, base));
        if (imageBase == nullptr) {
            platform.release(platform.context, base, pageCount);
            return ImageError::Unmapped;
        }
        // 先整段清零再逐段拷贝：段间空洞、BSS 与未落盘尾部都是零
        memset(imageBase, 0, view.imageBytes);
        for (uint32_t index = 0; index < view.sectionCount; ++index) {
            const ContainerSection& section = view.sections[index];
            if (section.fileBytes == 0) continue;
            memcpy(imageBase + section.memoryOffset, view.bytes + section.fileOffset, section.fileBytes);
        }
        image.base = base;
        image.memory = imageBase;
        image.imageBytes = view.imageBytes;
        image.pageCount = pageCount;
        return ImageError::None;
    }

    void ReleaseModuleImage(ImagePlatform& platform, ModuleImage& image) {
        if (image.base != 0 && image.pageCount != 0) platform.release(platform.context, image.base, image.pageCount);
        image = ModuleImage{};
    }

    const char* ImageErrorText(ImageError error) {
        switch (error) {
            case ImageError::None: return "";
            case ImageError::NullPointer: return "the module image view or platform callbacks are null";
            case ImageError::ImageBytes: return "the module image span is zero, unaligned or beyond the limit";
            case ImageError::NoPages: return "the image platform cannot provide enough contiguous pages";
            case ImageError::BaseRange: return "the module image base is unaligned or beyond the 4 GiB identity window";
            case ImageError::Unmapped: return "the module image base can't be mapped to a writable pointer";
            case ImageError::Section: return "a module section type, file span or file bytes field is invalid";
            case ImageError::SectionSpan: return "a module section memory span is invalid or overlaps another";
        }
        return "the module image error is unknown";
    }
}
