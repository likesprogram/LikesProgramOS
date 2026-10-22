/* ModuleImage.hpp
    映像映射：按容器 ImageBytes 取连续页作模块基址，逐段拷贝文件字节、清零 BSS

    物理地址到可写指针的换算经映像平台注入：内核侧用直映射窗口（PageAllocatorPlatform），
    宿主检查用可写缓冲替身，两者都把「物理地址」当平台能力，本层不假设页表形态；换成
    4 KiB 精细映射时只换平台，装载编排接口不变
    基址按 4 KiB 对齐、整段落在低 4 GiB 恒等映射窗口内；映像内容先整段清零再逐段拷贝文件字节，
    段间空洞、BSS 与各段未落盘的尾部因此都是零，不会带出分配器页里遗留的旧内容
    容器视图由容器解析器的有界解析给出；本层仍复核段类型、段文件区间、段内存区间与内存重叠，
    因为映像映射是唯一把容器字节变成内存的入口，写坏页帧的代价由整机承担
    失败返回 ImageError 分类，原因文本由 ImageErrorText 给出；装载器统一映射到 E-KRN-57 并把
    文本作为 detail，本文件不打印、不停机
*/

#pragma once
#include <stdint.h>

#include <Memory.hpp>
#include <ModuleContainer.hpp>
#include <Paging.hpp>

namespace Kernel {
    // 取页回调：context 是映像平台上下文，count 是要取的连续页数，失败返回 0
    using ImageAcquire = uint64_t (*)(void* context, uint32_t count);
    // 归还回调：physical 与 count 必须与取页时一致
    using ImageRelease = void (*)(void* context, uint64_t physical, uint32_t count);
    // 物理地址到可写指针的回调：不可映射返回空
    using ImageMapAddress = void* (*)(void* context, uint64_t physical);

    // 映像平台：映像映射只依赖取连续页、归还连续页与物理地址换算三件事，其他能力与本层无关
    // 内核侧用 PageAllocatorPlatform 适配页帧分配器与直映射窗口，宿主检查用替身注入失败与越界
    struct ImagePlatform {
        ImageAcquire acquire;      // 取 count 个连续页帧，失败返回 0
        ImageRelease release;      // 归还连续页帧
        ImageMapAddress map;       // 物理地址到可写指针，不可映射返回空
        void* context;             // 透传给三个回调的上下文
    };

    namespace Detail {
        // 页帧分配器的取页回调：context 是被适配的页帧分配器
        inline uint64_t PageAllocatorAcquire(void* context, uint32_t count) {
            return static_cast<PageAllocator*>(context)->Allocate(count);
        }

        // 页帧分配器的归还回调：context 是被适配的页帧分配器
        inline void PageAllocatorRelease(void* context, uint64_t physical, uint32_t count) {
            static_cast<PageAllocator*>(context)->Free(physical, count);
        }

        // 直映射窗口的地址换算回调：context 不参与换算
        inline void* PageAllocatorMap(void* /*context*/, uint64_t physical) {
            return PhysToVirt(physical);
        }
    }

    // 内核侧映像平台：页帧分配器取页与归还，直映射窗口换算地址；分配器须在使用期间保持有效
    inline ImagePlatform PageAllocatorPlatform(PageAllocator& allocator) {
        ImagePlatform platform;
        platform.acquire = &Detail::PageAllocatorAcquire;
        platform.release = &Detail::PageAllocatorRelease;
        platform.map = &Detail::PageAllocatorMap;
        platform.context = &allocator;
        return platform;
    }

    // 映射失败分类：全部映射到 E-KRN-57，文本作 detail；分类只增不改
    enum class ImageError : uint8_t {
        None = 0,       // 成功
        NullPointer,    // 容器视图没有容器字节或映像平台没有回调
        ImageBytes,     // ImageBytes 为零、非 4 KiB 倍数或超过容器总长上限
        NoPages,        // 页帧分配给不出连续的 ImageBytes/4KiB 页
        BaseRange,      // 基址未按 4 KiB 对齐或基址加映像跨度越过低 4 GiB
        Unmapped,       // 基址无法换算成可写指针
        Section,        // 段类型未知、BSS 与文件字节组合非法或段文件区间越出容器
        SectionSpan,    // 段内存偏移或跨度非法、越过 ImageBytes 或与其它段重叠
    };

    // 映射后的模块映像：base 是物理地址，memory 是同一段的可写指针（当前为直映射窗口地址）
    struct ModuleImage {
        uint64_t base = 0;          // 模块基址，物理地址，4 KiB 对齐
        uint8_t* memory = nullptr;  // 基址的可写指针，映像内容从它起算
        uint32_t imageBytes = 0;    // 映像跨度，4 KiB 倍数
        uint32_t pageCount = 0;     // 映像占用的连续页帧数
    };

    // 取连续页帧并写出模块映像：成功返回 None 并填 image；失败返回分类且 image 保持全零
    // 失败路径自行归还已取到的页帧，调用方不需要再调 ReleaseModuleImage
    ImageError MapModuleImage(const ContainerView& view, ImagePlatform& platform, ModuleImage& image);

    // 归还映像占用的页帧并清零 image：image 全零时是空操作，可重复调用
    void ReleaseModuleImage(ImagePlatform& platform, ModuleImage& image);

    // 失败分类的英文原因文本：给 E-KRN-57 的 detail 用，不带标点
    const char* ImageErrorText(ImageError error);
}
