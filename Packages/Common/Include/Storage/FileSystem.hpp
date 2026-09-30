/* FileSystem.hpp
    文件系统抽象：识别并挂载块设备上的卷，按统一路径索引卷内对象并读取

    统一路径的形式与分量切分取自 Path.hpp：所有实现共用同一条路径语法，调用方
    按同一种写法给出路径，不需要知道挂载的是 Ext4、ISO9660 还是 FAT32
    分量的查找与比较由各实现自己完成（FindChild）：Ext4 逐字节比较，ISO9660 与
    FAT32 按 ASCII 折叠比较，实现把结果经 Caps().nameCase 报告给调用方
    基类给出共用的逐段解析（ResolvePath）与按标识的事实查询接线（StatId），
    实现也可以按卷格式的需要直接覆盖 Open 与 Stat

    不依赖 C++ 运行库：无异常、无 RTTI、不动态分配内存、不使用虚析构
    接口方法都有默认实现而不是纯虚函数：引导镜像可能不带 C++ 运行库，纯虚函数
    会让链接引入 __cxa_pure_virtual；忘记覆盖时得到 NotSupported 而不是编译期错误，
    代价由存储层的实现数量可控
    实例由提供方构造与销毁：Baleen 侧用静态存储就地构造，内核侧由内存层分配
*/

#pragma once
#include <stdint.h>

#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>
#include <Storage/Path.hpp>

namespace Storage {
    // 卷内对象标识：只在所属卷内有效，不跨卷使用
    struct FileId {
        uint64_t value = 0;      // Ext4 为 inode 号，其他格式按各自定义
    };

    // 对象事实
    struct FileInfo {
        uint64_t bytes = 0;      // 常规文件字节数，目录为 0
        uint64_t id = 0;         // 同 FileId::value
        bool directory = false;  // 是否目录
    };

    // 实现能力：调用方据此选择路径，不在运行期试探
    struct FileSystemCaps {
        bool writable = false;                      // 是否支持写入
        bool directoryListing = false;              // 是否支持目录遍历
        bool symlinks = false;                      // 是否解析符号链接
        NameCase nameCase = NameCase::Sensitive;    // 卷内名字的比较语义
    };

    // 挂载模式
    enum class MountMode : uint32_t {
        ReadOnly,    // 只读挂载
        ReadWrite,   // 读写挂载；实现不支持时 Mount 返回 NotSupported
    };

    // 文件系统：识别并挂载块设备上的卷，按卷内路径查找、读取与遍历
    class FileSystem {
    public:
        // 格式名，诊断用，如 "ext4"；未覆盖时返回 "unknown"
        virtual const char* Format() const { return "unknown"; }
        // 识别并挂载；失败返回原因且实例保持未挂载，成功后可反复调用其他方法直至 Unmount
        // 设备取非 const 引用：挂载与读取都会改动设备的内部状态，不是逻辑只读
        virtual Error Mount(BlockDevice& device, MountMode mode) {
            (void)device;
            (void)mode;
            return Error::NotSupported;
        }
        // 卸载：释放对设备的引用，实例可再次 Mount
        virtual void Unmount() {}
        // 实现能力
        virtual FileSystemCaps Caps() const { return FileSystemCaps{}; }
        // 按路径查询对象事实；语法非法返回 InvalidArgument，路径不存在返回 NotFound
        virtual Error Stat(const char* path, FileInfo& info);
        // 按路径解析对象标识；路径指向目录时不返回 IsDirectory，由调用方按 FileInfo 判定
        virtual Error Open(const char* path, FileId& id);
        // 从 offset 起读至多 bytes 字节，实际长度写入 read；读到文件尾返回 Ok 且 read 为 0
        // offset 与 bytes 都以字节为单位，不分块
        virtual Error Read(const FileId& id, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) {
            (void)id;
            (void)offset;
            (void)buffer;
            (void)bytes;
            (void)read;
            return Error::NotSupported;
        }
        // 从 start 起读下一个目录项，名字写入 name 并更新 start；遍历结束返回 NotFound
        // name 是不带分隔符的单个分量，长度写入 nameLength；start 的取值含义由实现定义
        virtual Error ReadDirectory(const FileId& id, uint64_t& start, char* name, uint32_t nameBytes,
                                    uint32_t& nameLength, FileInfo& info) {
            (void)id;
            (void)start;
            (void)name;
            (void)nameBytes;
            (void)nameLength;
            (void)info;
            return Error::NotSupported;
        }

    protected:
        FileSystem() = default;
        // 实例由提供方按具体类型销毁，不经基类指针删除，因此不设虚析构
        ~FileSystem() = default;

        // 根目录的对象标识；挂载成功后有效，未覆盖时返回 NotSupported
        virtual Error RootId(FileId& id) {
            (void)id;
            return Error::NotSupported;
        }
        // 在目录里按名字找一个直接子项：名字比较规则由实现自己决定，见 Caps().nameCase
        // 名字是统一路径里的一个分量：非空、不含分隔符、非 ASCII 字节按原值使用
        // 找不到返回 NotFound，中间项不是目录返回 NotDirectory
        virtual Error FindChild(const FileId& directory, const char* name, uint32_t nameBytes, FileId& child) {
            (void)directory;
            (void)name;
            (void)nameBytes;
            (void)child;
            return Error::NotSupported;
        }
        // 按对象标识查询事实；目录的 bytes 为 0
        virtual Error StatId(const FileId& id, FileInfo& info) {
            (void)id;
            (void)info;
            return Error::NotSupported;
        }
        // 用 RootId 与 FindChild 逐段解析统一路径：根路径给出根目录标识
        // 语法检查、分量切分与错误码映射都在这里，实现不需要重写这段遍历
        Error ResolvePath(const char* path, FileId& id);
    };
}
