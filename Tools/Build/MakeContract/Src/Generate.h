/* Generate.h
    生成物：由契约输入生成模块契约层第九节 9.1 的 10 份头文件内容

    生成函数只接受已通过 contract::Load 校验的输入；文本内容只由输入与本文档的固定
    模板决定，不含时间戳、路径或用户名，同一输入两次生成的字节必须一致
*/

#pragma once
#include <string>
#include <vector>

#include "Contract.h"

namespace contract {
    // 一份生成物：文件名与完整文本
    struct OutputFile {
        std::string name;    // 文件名，如 Abi.hpp
        std::string text;    // 文件内容，LF 行尾
    };

    // 生成全部生成物：顺序固定，失败抛 InputError
    std::vector<OutputFile> Generate(const Contract& contract);
}
