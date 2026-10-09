/* ReadFailure.hpp
    读盘失败事实：Bios.asm 的读盘服务在失败路径留下的最后一份现场

    记录块常驻 Stub 映像：每次 _Bios_Read_Sectors 进入时清零，失败时写入失败请求的起始 LBA、
    块数、固件返回的 AH 状态与失败阶段；成功返回的调用不留下记录，诊断只在本次失败后取用
    地址随交权块交给 BaleenCore，两侧对同一份字节的解释必须一致：字段偏移与长度是 ABI
    本文件只做数据与写法：不判断何时打印，也不停机
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Baleen {
    // 失败阶段：读盘服务在哪一层放弃，取值与 Bios.asm 的阶段码一致
    enum class ReadFailPhase : uint32_t {
        None = 0,        // 没有失败：本次调用成功，或记录已被清空
        Reject = 1,      // 参数非法，未走固件
        BatchEdd = 2,    // 批量 EDD 请求失败
        SingleEdd = 3,   // 逐扇区重读的 EDD 请求失败
        SingleChs = 4,   // 逐扇区重读的 CHS 回退失败
        Window = 5,      // 请求放不进 64KiB 窗口，未走固件
    };

    // 最后一次读盘失败的现场：Bios.asm 写入，Stub 与 BaleenCore 的诊断行读取
    struct ReadFailureFacts {
        uint32_t lba;      // 失败请求的起始 LBA，本地扇区单位
        uint32_t count;    // 失败请求的块数
        uint32_t status;   // 固件返回的 AH 状态码；未走固件的情形为 0
        uint32_t phase;    // 失败阶段，取值同 ReadFailPhase
    };

    // 记录块的长度与字段偏移：与 Contract.inc 的 READ_FAIL_* 是同一组取值
    struct ReadFailureLayout {
        static constexpr uint32_t kBytes = 16;   // 结构长度
        static constexpr uint32_t kLba = 0x00;   // lba 字段
        static constexpr uint32_t kCount = 0x04; // count 字段
        static constexpr uint32_t kStatus = 0x08; // status 字段
        static constexpr uint32_t kPhase = 0x0C; // phase 字段
    };

    // 记录是汇编写入、两侧读取的字节块，长度与字段偏移在编译期固定；改动必须同步 Bios.asm
    static_assert(sizeof(ReadFailureFacts) == ReadFailureLayout::kBytes, "读盘失败记录必须是 16 字节，见 Contract.inc 的 READ_FAIL_BYTES");
    static_assert(offsetof(ReadFailureFacts, lba) == ReadFailureLayout::kLba, "读盘失败记录的 LBA 字段偏移已变");
    static_assert(offsetof(ReadFailureFacts, count) == ReadFailureLayout::kCount, "读盘失败记录的块数字段偏移已变");
    static_assert(offsetof(ReadFailureFacts, status) == ReadFailureLayout::kStatus, "读盘失败记录的 AH 字段偏移已变");
    static_assert(offsetof(ReadFailureFacts, phase) == ReadFailureLayout::kPhase, "读盘失败记录的阶段字段偏移已变");

    // 阶段名，供诊断行显示；未定义的取值返回 Unknown
    const char* ReadFailPhaseName(uint32_t phase);

    // 写一行读盘失败现场：续行标签加 Lba、Count、Ah、Phase 四个字段
    // 何时打印由调用方决定：本函数只负责写法，两个阶段的这一行因此保持一致
    void WriteReadFailureLine(const ReadFailureFacts& failure);
}
