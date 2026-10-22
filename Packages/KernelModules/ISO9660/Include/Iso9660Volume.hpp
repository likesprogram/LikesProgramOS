/* Iso9660Volume.hpp
    ISO9660 只读卷实现：主卷描述符、目录记录、Rock Ridge（NM/PX/SL）与文件读取

    依据 ECMA-119（卷描述符、目录记录、路径表）与 RRIP_1991A（SUSP/Rock Ridge）；
    只读、不解析路径表（按目录记录逐段查找即可满足统一路径的逐段解析）、不分配内存
    卷内标识符是 ISO 形式（大写、8.3、文件带 ";1" 版本），真实名字由 Rock Ridge 的 NM 给出；
    名字命中按 ASCII 折叠比较（FileSystemCaps::nameCase = Insensitive），比较前去掉版本后缀，
    因此 "/SystemVolume.img" 与 "/SYSTEMVO.IMG;1" 指向同一项
    未支持：Joliet、多区段文件（目录记录 flags bit7）、符号链接（SL）与关联/保护属性；
    遇到即明确拒绝，不猜测
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/Path.hpp>

namespace KernelModules {
    namespace Iso9660 {
        // 底层只读块设备视图：字段与契约层 BlockDeviceOps 的读侧同形
        struct IsoDevice {
            void* context;                                                          // 设备上下文，原样传给 Read
            uint32_t blockBytes;                                                    // 设备逻辑块大小，必须是 2 的幂
            uint64_t capacityBytes;                                                 // 设备容量，字节
            Storage::Error (*Read)(void* context, uint64_t offset, void* buffer, uint32_t bytes);   // 按块对齐读
        };

        // ISO9660 只读卷：单卷实例，挂载后有界解析，卸载后回到未挂载状态
        class Iso9660Volume {
        public:
            // ISO9660 逻辑块固定 2048 字节
            static constexpr uint32_t kBlockBytes = 2048;
            // 主卷描述符所在逻辑块（ECMA-119：16）
            static constexpr uint32_t kPvdLba = 16;
            // 卷标识与名字缓冲上限
            static constexpr uint32_t kVolumeIdBytes = 32;
            static constexpr uint32_t kMaxNameBytes = 255;
            // 根目录记录的固定偏移：主卷描述符内 156 起
            static constexpr uint32_t kPvdRootRecord = 156;

            // 挂载：读主卷描述符、校验 CD001/类型/逻辑块大小与根目录记录；失败保持未挂载
            Storage::Error Mount(const IsoDevice& device);
            // 卸载：清空挂载状态，实例可再次 Mount
            void Unmount();
            // 格式名，诊断用
            const char* Format() const;
            // 能力：只读、支持目录遍历、不跟随符号链接、名字 ASCII 折叠比较
            Storage::FileSystemCaps Caps() const;
            // 卷标识：主卷描述符的 Volume Identifier（32 字节，去尾空格）取前 16 字节，其余补零
            // 未挂载返回 InvalidArgument；卷标识为空的卷返回 NotSupported
            Storage::Error VolumeIdentity(uint8_t volumeId[16]) const;
            // 按卷内绝对路径查询事实
            Storage::Error Stat(const char* path, Storage::FileInfo& info);
            // 按卷内绝对路径解析对象标识（目录记录的字节偏移）
            Storage::Error Open(const char* path, uint64_t& object);
            // 读常规文件：目录记录给出 extent 与长度，按字节读取；文件尾之后读到 0
            Storage::Error Read(uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read);
            // 目录遍历：cursor 是目录数据内的字节偏移；"." 与 ".." 不作为目录项交出
            // 名字缓冲放不下返回 NameTooLong 且不推进游标；遍历结束返回 NotFound
            Storage::Error ReadDirectory(uint64_t object, uint64_t& cursor, char* name, uint32_t nameBytes, uint32_t& nameLength, Storage::FileInfo& info);
            // 按对象标识查询事实（目录遍历内部与宿主夹具使用）
            Storage::Error StatObject(uint64_t object, Storage::FileInfo& info);
            // 根目录对象标识（主卷描述符里的根目录记录偏移）
            uint64_t RootObject() const;

        private:
            // 一条目录记录解析出的事实
            struct Record {
                uint64_t offset;        // 记录在卷内的字节偏移（对象标识）
                uint32_t extentLba;     // 数据起始逻辑块号
                uint32_t dataBytes;     // 数据字节数
                bool directory;         // 是否目录（flags bit1）
                bool multiExtent;       // 是否多区段（flags bit7），不支持
                bool symlink;           // Rock Ridge SL 项存在，拒绝跟随
                bool nmSeen;            // 已遇到 Rock Ridge NM：NM 取代 ISO 标识符作为显示名，不是追加
                uint32_t nameBytes;     // 显示名字节数（Rock Ridge NM 优先，否则 ISO 标识符）
                char name[kMaxNameBytes + 1];       // 显示名，NUL 结尾
                uint32_t idNameBytes;   // ISO 标识符（文件去掉 ";1" 版本），作为第二种命中形式
                char idName[kMaxNameBytes + 1];     // ISO 标识符，NUL 结尾
            };

            // 从卷内字节偏移读一段（按设备逻辑块拼装）
            Storage::Error ReadBytes(uint64_t byteOffset, void* buffer, uint32_t bytes);
            // 读一个 ISO 逻辑块（2048 字节）
            Storage::Error ReadBlock(uint32_t lba, void* buffer);
            // 从已读入的逻辑块里解析一条目录记录（记录不跨块）；recordOffset 是记录在卷内的绝对偏移
            Storage::Error ParseRecord(const uint8_t* block, uint32_t offsetInBlock, uint64_t recordOffset, Record& record);
            Storage::Error ReadRecord(uint64_t offset, Record& record);
            // 解析系统用区里的 SUSP/Rock Ridge 项：RR 标志、NM 名字、PX 权限、SL 符号链接、CE 续接
            Storage::Error ParseSystemUse(const uint8_t* systemUse, uint32_t bytes, Record& record, bool allowContinuation);
            // 在目录里按折叠后的名字找一个直接子项
            Storage::Error FindChild(uint64_t directoryObject, const char* name, uint32_t nameBytes, uint64_t& child);
            // 用根目录与 FindChild 逐段解析统一路径
            Storage::Error ResolvePath(const char* path, uint64_t& object);

            IsoDevice m_device;                     // 底层设备（不拥有，使用期间须有效）
            bool m_mounted;                         // 是否已挂载
            uint32_t m_volumeBlocks;                // 卷空间块数（32 位字段）
            uint8_t m_volumeId[kVolumeIdBytes];     // 主卷描述符的卷标识原字节
            uint64_t m_rootObject;                  // 根目录记录偏移
            uint8_t m_unit[4096];                   // 设备逻辑块读缓冲
            uint8_t m_block[kBlockBytes];           // ISO 逻辑块读缓冲
            uint8_t m_continuation[1024];           // CE 续接区缓冲
        };
    }
}
