/* Diagnostics.hpp
    引导期失败诊断的稳定编号：编号与文本的配对，以及带编号的诊断行写法

    编号是工单与日志聚合引用的稳定标识：同一原因的编号一经发布不再改动，文本可以润色；
    阶段前缀为 E-STUB、E-CORE 与 E-UEFI，完整清单见 Docs/Specs/Baleen/失败诊断编号.md
    IPL 因 446 / 510 字节的首段预算只输出单字母码，编号与字母的对应见一级引导契约第六节
    本文件只做数据与写法：不判断何时打印，也不停机

    共享实现（SelectSet / ManifestVerify / LoadKernel 等）在两个固件环境里各编一份，
    编号前缀随编译目标变、序号不变（同号异前缀约定）：源码里把编号写成
    BALEEN_STAGE_PREFIX "-61" 这样的相邻字符串字面量，缺省前缀是 E-CORE，
    UEFI 侧在编译选项里定义 BALEEN_STAGE_PREFIX 为 "E-UEFI"
*/

#pragma once
#include <stdint.h>

#include <PrintTarget.hpp>

// 诊断编号的阶段前缀：默认 BaleenCore 阶段；换阶段时改这个定义，不改序号
#ifndef BALEEN_STAGE_PREFIX
#define BALEEN_STAGE_PREFIX "E-CORE"
#endif

// 把序号拼成完整编号文本："61" 在这个阶段就是 BALEEN_STAGE_PREFIX "-61"
#define BALEEN_STAGE_CODE(number) BALEEN_STAGE_PREFIX "-" number

namespace Baleen {
    // 失败原因：编号是稳定标识，文本是给人看的英文原因
    // code 为空表示成功；按值返回，文本允许指向运行期字符串（如共享实现给出的字段原因）
    struct Diagnostic {
        const char* code;   // 编号文本，含阶段前缀与两位序号，如 "E-CORE-07"
        const char* text;   // 原因文本，不含标点；为空时诊断行只输出编号
    };

    // 写一行带编号的诊断：标签 + 编号 + 文本；detail 非空时在文本后补 " (detail)"
    // detail 用于补充稳定枚举一类的现场（如存储层错误名），不作为编号的替代
    // 何时打印由调用方决定：本函数只负责写法，各阶段的这一行因此保持一致
    void WriteDiagnostic(PrintTargets::Tag tag, const Diagnostic& diagnostic, const char* detail = nullptr);
}
