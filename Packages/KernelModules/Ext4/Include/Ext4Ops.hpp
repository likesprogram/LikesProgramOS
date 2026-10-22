/* Ext4Ops.hpp
    Ext4 模块的 FileSystemOps 九槽表与模块内静态卷实例的访问器

    表与实例是模块内唯一的一份：Init 把它们交给 Fs.Vfs 的 RegisterFileSystem 登记，
    之后 Vfs 的 Boot.Volume 按候选设备调用 Mount，再经 Stat/Open/Read/ReadDirectory 读卷
    表内容以生成头 Types.hpp 的 FileSystemOps 为准（9 字段，布局与空 ModuleAbi/libc 无关）
    宿主夹具直接取这张表调用，检查的就是模块将要登记的那份实现，不做第二份适配层
*/

#pragma once
#include <stdint.h>

#include <Types.hpp>

namespace KernelModules {
    namespace Ext4 {
        // FileSystemOps 表：9 槽常量表，context 是模块内静态卷实例
        const FileSystemOps* FileSystemOpsTable();

        // 表的 context：模块内唯一的 Ext4Volume 实例（零初始化落 .bss）
        void* FileSystemContext();
    }
}
