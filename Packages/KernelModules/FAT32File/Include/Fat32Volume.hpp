/* Fat32Volume.hpp
    FAT32 只读卷实现：BPB、FAT 链、目录项（8.3 与 LFN）与文件读取

    依据 Microsoft FAT32 File System Specification（fatgen103）；
    只读、不写卷、不分配内存，缓冲都是实例内固定数组
    名字命中按 ASCII 折叠比较（FileSystemCaps::nameCase = Insensitive）：FAT 卷内 8.3 名以大写
    保存、LFN 保留真实大小写，两种形式都能命中；LFN 含非 ASCII 字符时退回 8.3 名（不猜字符集）
    未支持：FAT12/16、扇区或簇大小超过实例缓冲、非 8.3/LFN 的目录项属性组合；
    遇到即明确拒绝，不猜测
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/Path.hpp>

namespace KernelModules {
    namespace Fat32 {
        // 底层只读块设备视图：字段与契约层 BlockDeviceOps 的读侧同形
        struct FatDevice {
            void* context;                                                          // 设备上下文，原样传给 Read
            uint32_t blockBytes;                                                    // 设备逻辑块大小，必须是 2 的幂
            uint64_t capacityBytes;                                                 // 设备容量，字节
            Storage::Error (*Read)(void* context, uint64_t offset, void* buffer, uint32_t bytes);   // 按块对齐读
        };

        // FAT32 只读卷：单卷实例，挂载后有界解析，卸载后回到未挂载状态
        class Fat32Volume {
        public:
            // 名字缓冲上限（FAT 的 LFN 最长 255 个 UTF-16 字符，本实现只接受 ASCII 子集）
            static constexpr uint32_t kMaxNameBytes = 255;
            // 支持的簇字节数上限：实例缓冲按它固定
            static constexpr uint32_t kMaxClusterBytes = 4096;
            // 根目录对象标识：根目录没有目录项，用固定哨兵值区分
            static constexpr uint64_t kRootObject = 0xFFFFFFFFFFFFFFFFull;

            // 挂载：读 BPB、校验 FAT32 形态与几何字段；失败保持未挂载
            Storage::Error Mount(const FatDevice& device);
            // 卸载：清空挂载状态，实例可再次 Mount
            void Unmount();
            // 格式名，诊断用
            const char* Format() const;
            // 能力：只读、支持目录遍历、不跟随符号链接、名字 ASCII 折叠比较
            Storage::FileSystemCaps Caps() const;
            // 卷标识：BPB 的 Volume ID（4 字节）+ Volume Label（11 字节去尾空格），共 15 字节后补零
            // 未挂载返回 InvalidArgument；两者都为空（无标识能力）返回 NotSupported
            Storage::Error VolumeIdentity(uint8_t volumeId[16]) const;
            // 按卷内绝对路径查询事实
            Storage::Error Stat(const char* path, Storage::FileInfo& info);
            // 按卷内绝对路径解析对象标识（目录项的字节偏移 + 1；根目录是 kRootObject）
            Storage::Error Open(const char* path, uint64_t& object);
            // 读常规文件：按 FAT 链逐簇读取，跨越文件尾时按实际长度回报
            Storage::Error Read(uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read);
            // 目录遍历：cursor 是目录数据内的字节偏移；"." 与 ".." 不作为目录项交出
            // 名字缓冲放不下返回 NameTooLong 且不推进游标；遍历结束返回 NotFound
            Storage::Error ReadDirectory(uint64_t object, uint64_t& cursor, char* name, uint32_t nameBytes, uint32_t& nameLength, Storage::FileInfo& info);
            // 按对象标识查询事实（目录遍历内部与宿主夹具使用）
            Storage::Error StatObject(uint64_t object, Storage::FileInfo& info);
            // 根目录对象标识
            uint64_t RootObject() const;

        private:
            // 一条目录项解析出的事实
            struct Entry {
                uint64_t offset;        // 目录项在卷内的字节偏移
                uint32_t firstCluster;  // 起始簇号；0 表示空文件
                uint64_t bytes;         // 文件字节数；目录为 0
                bool directory;         // 是否目录
                uint32_t nameBytes;     // 名字字节数（LFN 优先，否则 8.3）
                char name[kMaxNameBytes + 1];   // 显示名（LFN 优先，否则 8.3），NUL 结尾
                uint32_t shortNameBytes;        // 8.3 短名字节数，作为第二种命中形式
                char shortName[kMaxNameBytes + 1];  // 8.3 短名，NUL 结尾
            };

            // 目录遍历状态：沿 FAT 链推进，LFN 片段在遇到短项之前拼装
            struct WalkState {
                uint32_t cluster;          // 当前簇号
                uint32_t position;         // 簇内字节偏移
                bool end;                  // 目录结束（遇到 0x00 或链尾）
                bool lfnSeen;              // 本组已见到 LFN 片段
                uint8_t lfnChecksum;       // LFN 携带的短名校验和
                uint32_t lfnLastOrdinal;   // LFN 最后一个序号（带 0x40 标志）
                uint16_t lfnChars[260];    // 拼装后的 UTF-16 码元
            };

            // 从卷内字节偏移读一段（按设备逻辑块拼装）
            Storage::Error ReadBytes(uint64_t byteOffset, void* buffer, uint32_t bytes);
            // 读 FAT 表里的一个簇项（已去掉高 4 位保留位）
            Storage::Error FatEntry(uint32_t cluster, uint32_t& next);
            // 求 FAT 链上下一个簇；end 表示链结束
            Storage::Error NextCluster(uint32_t cluster, uint32_t& next, bool& end);
            // 读一个簇的字节
            Storage::Error ReadCluster(uint32_t cluster, void* buffer);
            // 簇在卷内的字节偏移
            uint64_t ClusterByteOffset(uint32_t cluster);
            // 沿 FAT 链跳过 skipClusters 个簇，返回目标簇号
            Storage::Error SeekCluster(uint32_t firstCluster, uint32_t skipClusters, uint32_t& cluster);
            // 解析目录项（offset 处的 32 字节；LFN 由调用方按序拼好）
            Storage::Error ReadEntry(uint64_t offset, Entry& entry);
            // 按对象标识取目录项事实：根目录用合成目录项（起始簇来自挂载解析）
            Storage::Error EntryForObject(uint64_t object, Entry& entry);
            // 初始化遍历状态：目录对象给出起始簇
            Storage::Error BeginWalk(uint64_t object, WalkState& state);
            // 取下一个可用目录项：跳过删除项、卷标与 LFN 片段，ok 为假表示遍历结束
            Storage::Error NextEntry(WalkState& state, Entry& entry, bool& ok);
            // 解析 8.3 短名：NAME    EXT -> NAME.EXT（小写保留原样，匹配时折叠）
            static void ShortName(const uint8_t* raw, uint32_t& nameBytes, char* name);
            // 短名校验和：LFN 与短名配套校验
            static uint8_t ShortNameChecksum(const uint8_t* raw);
            // 在目录里按折叠后的名字找一个直接子项
            Storage::Error FindChild(uint64_t directoryObject, const char* name, uint32_t nameBytes, uint64_t& child);
            // 用根目录与 FindChild 逐段解析统一路径
            Storage::Error ResolvePath(const char* path, uint64_t& object);

            FatDevice m_device;             // 底层设备（不拥有，使用期间须有效）
            bool m_mounted;                 // 是否已挂载
            uint32_t m_bytesPerSector;      // 每扇区字节数
            uint32_t m_sectorsPerCluster;   // 每簇扇区数
            uint32_t m_clusterBytes;        // 每簇字节数
            uint32_t m_reservedSectors;     // 保留扇区数
            uint32_t m_fatCount;            // FAT 份数
            uint32_t m_fatSectors;          // 每份 FAT 的扇区数
            uint32_t m_rootCluster;         // 根目录起始簇
            uint32_t m_clusterCount;        // 数据区簇数
            uint64_t m_dataStartLba;        // 数据区起始扇区
            uint8_t m_volumeId[4];          // BPB 卷序号
            uint8_t m_volumeLabel[11];      // BPB 卷标（空格填充）
            uint8_t m_unit[4096];           // 设备逻辑块读缓冲
            uint8_t m_cluster[kMaxClusterBytes];   // 簇读缓冲
        };
    }
}
