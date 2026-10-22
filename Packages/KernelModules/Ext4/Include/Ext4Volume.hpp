/* Ext4Volume.hpp
    内核 Ext4 模块的只读卷实现：识别并挂载卷、按路径查找、按 extent 读常规文件、按游标列目录

    从 BaleenCore 的只读实现（Packages/Baleen/Core/Src/Ext4.cpp）移植，解析语义不变；
    差异只在底层访问方式与对外形态：
      - 底层由 BlockDeviceOps + deviceContext 给出的函数指针读取（模块不能链接 Storage 的
        虚接口实现），本头用 Ext4Device 把两者归一，宿主夹具也按同一形状注入内存实现
      - 对外由 FileSystemOps 九槽表调用（Src/Ext4Ops.cpp），模块 Init 经 Fs.Vfs 登记
    能力边界：
    识别 superblock 与特性标志、读块组描述符与 inode、走 extent 树、解析线性目录项、
    读常规文件、列线性目录；不支持的特性、htree 索引目录、符号链接与内联数据都明确拒绝
    无运行库、不动态分配：缓冲都是实例内的固定数组；实例是模块内静态对象（零初始化，
    不产生全局动态初始化），由调用方保证同一时刻只挂载一个卷
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/Path.hpp>

namespace KernelModules {
    namespace Ext4 {
        // 底层只读块设备视图：字段与契约层 BlockDeviceOps 的读侧同形，宿主夹具可直接构造
        // Read 的签名与 BlockDeviceOps::Read 完全一致，模块侧因此不需要包装函数
        struct Ext4Device {
            void* context;                                                          // 设备上下文，原样传给 Read
            uint32_t blockBytes;                                                    // 逻辑块大小，必须是 2 的幂
            uint64_t capacityBytes;                                                 // 设备容量，字节
            Storage::Error (*Read)(void* context, uint64_t offset, void* buffer, uint32_t bytes);   // 按块对齐读
        };

        // Ext4 只读卷：单卷实例，挂载后有界解析，卸载后回到未挂载状态
        class Ext4Volume {
        public:
            // 卷内逻辑块大小的范围（Ext4 规范：1024、2048、4096）
            static constexpr uint32_t kMinBlockBytes = 1024;
            static constexpr uint32_t kMaxBlockBytes = 4096;
            // inode 大小的范围（128 或 256）
            static constexpr uint32_t kMaxInodeBytes = 256;
            // 根目录 inode 号
            static constexpr uint64_t kRootInode = 2;

            // 挂载：读 superblock、校验魔数与特性清单（按 kBaleenAccepted）；失败返回原因且实例保持未挂载
            // 只读实现只接受只读语义：调用方传入的 mode 由 Ext4Ops 先判定，本函数不解析挂载模式
            Storage::Error Mount(const Ext4Device& device);
            // 卸载：清空挂载状态，实例可再次 Mount
            void Unmount();
            // 格式名，诊断用
            const char* Format() const;
            // 能力：只读、支持线性目录遍历，不跟随符号链接，名字逐字节比较
            Storage::FileSystemCaps Caps() const;
            // 卷标识（superblock 的 s_uuid，16 字节）：挂载成功后有效，未挂载返回 InvalidArgument
            Storage::Error VolumeIdentity(uint8_t volumeId[16]) const;
            // 卷内逻辑块大小；未挂载返回 0
            uint32_t BlockBytes() const;
            // 卷内 inode 总数；未挂载返回 0
            uint32_t InodesCount() const;
            // 卷内块总数；未挂载返回 0
            uint64_t BlocksCount() const;
            // 按卷内绝对路径查询事实
            Storage::Error Stat(const char* path, Storage::FileInfo& info);
            // 按卷内绝对路径解析对象标识（inode 号）
            Storage::Error Open(const char* path, uint64_t& object);
            // 读常规文件：按 extent 映射逐块读取，跨越文件尾时按实际长度回报
            Storage::Error Read(uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read);
            // 目录遍历：从 cursor 起交出一个目录项，名字写入 name，游标推进到下一项
            // 游标是目录数据的字节偏移：调用方从 0 开始，把上一次的值原样传回，遍历结束返回 NotFound
            // "." 与 ".." 不作为目录项交出；名字放不下返回 NameTooLong 且不推进游标
            // htree 索引目录、符号链接与非常规项返回 NotSupported，不静默跳过
            Storage::Error ReadDirectory(uint64_t object, uint64_t& cursor, char* name, uint32_t nameBytes, uint32_t& nameLength, Storage::FileInfo& info);
            // 按对象标识查询事实（目录遍历内部与宿主夹具使用）
            Storage::Error StatObject(uint64_t object, Storage::FileInfo& info);

        private:
            // 解析出来的 inode 事实
            struct Inode {
                uint16_t mode;        // 类型与权限
                uint32_t flags;       // inode 标志
                uint64_t bytes;       // 文件字节数（size_lo 与 size_high 合成）
                uint8_t block[60];    // i_block：extent 树的根
            };

            // 从底层设备按字节读一段：接口要求按块对齐，本层按设备块换算并拼装
            Storage::Error ReadBytes(uint64_t byteOffset, void* buffer, uint32_t bytes);
            // 读块组描述符里的 inode 表位置
            Storage::Error GroupInodeTable(uint32_t group, uint64_t& table);
            // 读 inode 表里的一个 inode（inode 号从 1 起）
            Storage::Error ReadInode(uint64_t ino, Inode& inode);
            // extent 树查找的完整结果：物理块、同一 extent 内从该块起可连续读的块数与是否空洞
            Storage::Error MapRun(const Inode& inode, uint32_t logical, uint64_t& physical, uint32_t& runBlocks, bool& hole);
            // 按 inode 的数据映射读一段（目录与常规文件共用）
            Storage::Error ReadInodeBytes(const Inode& inode, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read);
            // 读一个卷内逻辑块到块缓冲
            Storage::Error ReadBlock(uint64_t block, void* buffer);
            // 用 RootId 与 FindChild 逐段解析统一路径；语法在本层再核一次，Vfs 已按统一语法预校验
            Storage::Error ResolvePath(const char* path, uint64_t& object);
            // 在线性目录里按名字逐字节比较找直接子项
            Storage::Error FindChild(uint64_t directory, const char* name, uint32_t nameBytes, uint64_t& child);
            // 按对象标识查询 inode 事实
            Storage::Error StatInode(uint64_t object, Storage::FileInfo& info);

            Ext4Device m_device;                        // 底层设备（不拥有，使用期间须有效）
            bool m_mounted;                             // 是否已挂载
            uint32_t m_blockBytes;                      // 卷内逻辑块大小
            uint32_t m_blocksPerGroup;                  // 每组块数
            uint32_t m_inodesPerGroup;                  // 每组 inode 数
            uint32_t m_inodesCount;                     // inode 总数
            uint32_t m_inodeBytes;                      // inode 大小
            uint32_t m_descBytes;                       // 块组描述符大小
            uint32_t m_incompat;                        // INCOMPAT 特性位
            uint64_t m_blocksCount;                     // 卷内块总数
            uint8_t m_uuid[16];                         // 卷标识
            uint8_t m_super[1024];                      // superblock 原字节
            uint8_t m_unitBuffer[4096];                 // 底层设备单位缓冲（superblock、inode、描述符读取）
            uint8_t m_blockBuffer[kMaxBlockBytes];      // 卷内块缓冲（extent 索引块、目录块、文件数据块）
        };
    }
}
