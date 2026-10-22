/* ScreenFacts.cpp
    UEFI 路径的屏幕事实采集实现：把 GOP 当前 mode 的帧缓冲事实映射成交给内核的屏幕形态

    判据：两种 32 位打包像素格式给出 Bgrx8888 / Rgbx8888，
    位掩码与 Blt-only 格式给 None；事实必须自洽（几何、步长、可写字节数非零）且整段在低 4GiB
    本文件只做采集与映射：不打印、不停机，格式退化不是失败
*/

#include <ScreenFacts.hpp>

namespace Baleen {
    namespace Uefi {
        namespace {
            // 像素格式映射：只支持 32 位打包的两种字节序，其余格式返回 false
            bool MapPixelFormat(uint32_t pixelFormat, Boot::ScreenFormat& format) {
                switch (pixelFormat) {
                    case kPixelFormatBlueGreenRedReserved8BitPerColor:
                        format = Boot::ScreenFormat::Bgrx8888;
                        return true;
                    case kPixelFormatRedGreenBlueReserved8BitPerColor:
                        format = Boot::ScreenFormat::Rgbx8888;
                        return true;
                    default:
                        // PixelBitMask / PixelBltOnly / 未知格式：没有可直写的线性像素布局
                        return false;
                }
            }
        }

        ScreenFacts CollectScreenFacts(const GraphicsOutputProtocolMode* mode) {
            ScreenFacts facts{};
            if (mode == nullptr || mode->info == nullptr) return facts;
            const GraphicsOutputModeInformation& info = *mode->info;
            Boot::ScreenFormat format = Boot::ScreenFormat::VgaText;
            if (!MapPixelFormat(info.pixelFormat, format)) return facts;
            // 事实必须自洽：几何、步长与可写字节数都要非零，否则内核无法按它们自绘
            const uint64_t stride = static_cast<uint64_t>(info.pixelsPerScanLine) * kBytesPerPixel;
            const uint64_t base = mode->frameBufferBase;
            const uint64_t bytes = mode->frameBufferSize;
            if (info.horizontalResolution == 0 || info.verticalResolution == 0 || stride == 0 || bytes == 0) return facts;
            // 字段是 32 位：装不下的步长按事实不成立处理
            if (stride > 0xFFFFFFFFull) return facts;
            // 帧缓冲整段必须在低 4GiB：高端地址超出内核的恒等映射，按“未提供”给 None
            if (base == 0 || base >= kIdentityLimit) return facts;
            if (bytes > kIdentityLimit - base) return facts;
            facts.kind = Boot::ScreenKind::Framebuffer;
            facts.address = base;
            facts.width = info.horizontalResolution;
            facts.height = info.verticalResolution;
            facts.stride = static_cast<uint32_t>(stride);
            facts.bytes = static_cast<uint32_t>(bytes);
            facts.format = format;
            return facts;
        }
    }
}
