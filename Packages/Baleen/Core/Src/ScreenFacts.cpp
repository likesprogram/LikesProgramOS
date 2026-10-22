/* ScreenFacts.cpp
    BIOS 路径的屏幕事实采集实现：从 BIOS 数据区读当前显示模式与列数，映射成 VGA / MDA 文本页事实

    判据与 Packages/Baleen/Common/Src/PrintTarget.cpp 的文本模式判定同源：
    模式 2 / 3 / 7 且列数为 80 才算文本模式，其它形态一律给 None
    本文件只做采集与映射：不打印、不停机，退化诊断由控制台装配侧负责
*/

#include <ScreenFacts.hpp>

#include <Platform/PhysicalMemory.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            // BIOS 数据区的当前显示模式与列数：0040:0049 与 0040:004A，偏移从 BDA 起点算
            constexpr uintptr_t kBdaBase = 0x400;      // BIOS 数据区起点
            constexpr uintptr_t kBdaVideoMode = 0x49;  // 当前显示模式
            constexpr uintptr_t kBdaColumns = 0x4A;    // 当前文本列数

            // 文本页物理地址：模式 2 / 3 是 VGA 彩色文本，模式 7 是 MDA 单色文本
            constexpr uint64_t kColorTextAddress = 0xB8000;         // VGA 彩色文本页
            constexpr uint64_t kMonochromeTextAddress = 0xB0000;    // MDA 单色文本页
        }

        ScreenFacts DecodeVgaScreenFacts(uint8_t mode, uint8_t columns) {
            ScreenFacts facts{};
            // 列数不是 80：BDA 说的几何与固定文本页不符，事实不成立
            if (columns != kScreenColumns) return facts;
            uint64_t address = 0;
            if (mode == 2 || mode == 3) address = kColorTextAddress;
            else if (mode == 7) address = kMonochromeTextAddress;
            else return facts;   // 非文本模式（含图形模式）：屏幕事实给 None
            facts.kind = Boot::ScreenKind::VgaText;
            facts.address = address;
            facts.width = kScreenColumns;
            facts.height = kScreenRows;
            facts.stride = kScreenStride;
            facts.bytes = kScreenBytes;
            facts.format = Boot::ScreenFormat::VgaText;
            return facts;
        }

        ScreenFacts CollectScreenFacts() {
            const volatile uint8_t* const bda = Platform::PhysicalBytePointer(kBdaBase);
            return DecodeVgaScreenFacts(bda[kBdaVideoMode], bda[kBdaColumns]);
        }
    }
}
