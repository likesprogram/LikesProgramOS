/* FileSystem.cpp
    文件系统抽象的共用接线：统一路径的逐段解析与按标识的事实查询
*/

#include <Storage/FileSystem.hpp>

namespace Storage {
    Error FileSystem::ResolvePath(const char* path, FileId& id) {
        PathWalker walker(path);
        if (walker.Check() != Error::Ok) return walker.Check();
        // 根路径没有分量：目标就是根目录
        FileId current;
        if (const Error error = RootId(current); error != Error::Ok) return error;
        while (walker.HasName()) {
            FileId child;
            if (const Error error = FindChild(current, walker.Name(), walker.NameBytes(), child); error != Error::Ok) return error;
            current = child;
            walker.Advance();
        }
        id = current;
        return Error::Ok;
    }

    Error FileSystem::Open(const char* path, FileId& id) {
        return ResolvePath(path, id);
    }

    Error FileSystem::Stat(const char* path, FileInfo& info) {
        FileId id;
        if (const Error error = Open(path, id); error != Error::Ok) return error;
        return StatId(id, info);
    }
}
