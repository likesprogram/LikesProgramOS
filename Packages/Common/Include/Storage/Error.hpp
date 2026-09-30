/* Error.hpp
    存储层操作结果与诊断文本：块设备、偏移视图与文件系统共用的错误模型

    错误码是稳定编号：复用既有取值或改写语义必须按 ABI 兼容与外部契约走新版本
    诊断文本是共用实现：Baleen 阶段把它拼进 FATAL 行，内核侧进日志
*/

#pragma once
#include <stdint.h>

namespace Storage {
    // 操作结果；除 Ok 外都表示失败，诊断文本由 ErrorMessage 给出
    enum class Error : int32_t {
        Ok = 0,            // 成功
        NotFound,          // 路径或对象不存在
        NotDirectory,      // 路径中间项不是目录
        IsDirectory,       // 目标要求常规文件，实际是目录
        NotSupported,      // 卷特性或操作不受支持，含未知的 Ext4 INCOMPAT 位
        Corrupt,           // 结构非法：魔数、校验、范围、环路
        Io,                // 底层读写失败
        ReadOnly,          // 目标只读
        NoSpace,           // 空间不足
        NameTooLong,       // 路径或单个名字超出实现上限
        InvalidArgument,   // 参数非法：空指针、零长度、未对齐、越界
    };

    // 错误码的诊断文本，不含标点；不认识的取值返回 "unknown error"
    const char* ErrorMessage(Error error);
}
