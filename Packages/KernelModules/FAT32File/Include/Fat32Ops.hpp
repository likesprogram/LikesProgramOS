/* Fat32Ops.hpp
    FAT32 模块的 FileSystemOps 九槽表与模块内静态卷实例的访问器

    表与实例是模块内唯一的一份：Init 把它们交给 Fs.Vfs 的 RegisterFileSystem 登记，
    之后 Vfs 在候选设备上调用 Mount，再经 Stat/Open/Read/ReadDirectory 读卷
    宿主夹具直接取这张表调用，检查的就是模块将要登记的那份实现
*/

#pragma once
#include <stdint.h>

#include <Types.hpp>

namespace KernelModules {
    namespace Fat32 {
        // FileSystemOps 表：9 槽常量表，context 是模块内静态卷实例
        const FileSystemOps* FileSystemOpsTable();

        // 表的 context：模块内唯一的 Fat32Volume 实例（零初始化落 .bss）
        void* FileSystemContext();
    }
}
