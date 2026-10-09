/* Diagnostics.hpp
    引导期失败诊断的稳定编号：编号与文本的配对，以及带编号的诊断行写法

    编号是工单与日志聚合引用的稳定标识：同一原因的编号一经发布不再改动，文本可以润色；
    阶段前缀为 E-STUB 与 E-CORE，完整清单见 Docs/Specs/Baleen/失败诊断编号.md
    IPL 因 446 / 510 字节的首段预算只输出单字母码，编号与字母的对应见一级引导契约第六节
    本文件只做数据与写法：不判断何时打印，也不停机
*/

#pragma once
#include <stdint.h>

#include <PrintTarget.hpp>

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
