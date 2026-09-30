/* Error.cpp
    存储层错误码与诊断文本的实现
*/

#include <Storage/Error.hpp>

namespace Storage {
    const char* ErrorMessage(Error error) {
        switch (error) {
            case Error::Ok: return "ok";
            case Error::NotFound: return "not found";
            case Error::NotDirectory: return "not a directory";
            case Error::IsDirectory: return "is a directory";
            case Error::NotSupported: return "not supported";
            case Error::Corrupt: return "corrupt structure";
            case Error::Io: return "i/o error";
            case Error::ReadOnly: return "read-only";
            case Error::NoSpace: return "no space";
            case Error::NameTooLong: return "name too long";
            case Error::InvalidArgument: return "invalid argument";
        }
        return "unknown error";   // 枚举扩展后未同步本表时的兜底，漏项正常由 -Wswitch 在构建期拦下
    }
}
