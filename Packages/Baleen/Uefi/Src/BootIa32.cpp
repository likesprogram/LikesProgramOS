/* BootIa32.cpp
    IA32（32 位 UEFI）侧的不支持提示件：把"这台平台为什么起不来"打到固件控制台上

    本项目的引导器与内核都按 64 位构建：内核入口是长模式，UEFI 侧的引导器也是 x64 固件应用，
    因此 32 位固件上没有任何可引导的东西——这一件的职责只有一句提示，让用户知道死在哪儿
    由 IA32 固件按可移动介质回退路径 \EFI\BOOT\BOOTIA32.EFI 加载；x64 固件不会加载它
    只用到 Simple Text Output 协议的 OutputString，不用任何固件服务之外的设施
    偏移按 UEFI 规范第 4 章：32 位构建下指针 4 字节，EFI_SYSTEM_TABLE 的 ConOut 在 44
*/

#include <stddef.h>
#include <stdint.h>

namespace {
    // EFI 表头：24 字节，所有 EFI 表都以它开头
    struct TableHeader {
        uint64_t signature;      // 表标记
        uint32_t revision;       // 规范版本
        uint32_t headerBytes;    // 表头长度
        uint32_t crc32;          // 表头校验
        uint32_t reserved;       // 保留
    };

    // 简单文本输出协议：只需要 OutputString，前面的成员按规范占位
    struct SimpleTextOutput {
        void* reset;                                                        // 复位
        uint64_t (*outputString)(SimpleTextOutput*, const char16_t*);       // 输出 UTF-16 字符串
    };

    // 32 位固件下的 EFI_SYSTEM_TABLE：只用到这里为止的成员，之后的成员不读也不写
    struct SystemTable {
        TableHeader header;                  // 表头
        const char16_t* firmwareVendor;      // 固件厂商标识
        uint32_t firmwareRevision;           // 固件版本
        void* consoleInHandle;               // 控制台输入句柄
        void* consoleIn;                     // 控制台输入协议
        void* consoleOutHandle;              // 控制台输出句柄
        SimpleTextOutput* consoleOut;        // 控制台输出协议
    };

    // 偏移是两侧 ABI 的一部分：与 UEFI 规范第 4 章的 32 位布局一致
    static_assert(offsetof(SystemTable, consoleOutHandle) == 40, "EFI_SYSTEM_TABLE 的 ConsoleOutHandle 偏移已变");
    static_assert(offsetof(SystemTable, consoleOut) == 44, "EFI_SYSTEM_TABLE 的 ConOut 偏移已变");
    static_assert(offsetof(SimpleTextOutput, outputString) == 4, "简单文本输出协议的 OutputString 偏移已变");

    // 提示文本：编号与文本在 Docs/Specs/Baleen/失败诊断编号.md 第四节登记
    // 与 BaleenCore 的 E-CORE-11 同号：两侧都表达“平台的位数/模式不满足内核要求”，此处判据是固件架构
    const char16_t kMessage[] =
        u"[ FATAL ] E-UEFI-11 The firmware is 32-bit UEFI (IA32): the UEFI boot path and the kernel require x64\r\n"
        u"\r\n"
        u"Baleen: the LikesProgramOS kernel and the Baleen UEFI boot path are 64-bit only:\r\n"
        u"they need x64 firmware and a CPU with long mode. Nothing is booted here.\r\n"
        u"Halting; power off or boot a supported x64 platform.\r\n";
}

// 入口：打印提示后停住
// 返回会把控制权交回固件，提示随即被下一个启动项刷掉，因此这里不返回
extern "C" uint64_t efi_main(void* imageHandle, SystemTable* systemTable) {
    (void)imageHandle;
    if (systemTable != nullptr && systemTable->consoleOut != nullptr && systemTable->consoleOut->outputString != nullptr) {
        systemTable->consoleOut->outputString(systemTable->consoleOut, kMessage);
    }
    for (;;) __asm__ volatile("hlt");
    return 0;
}
