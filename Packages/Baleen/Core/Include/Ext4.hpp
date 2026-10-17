/* Ext4.hpp
    BaleenCore 的 Ext4 只读最小实现：识别并挂载卷、按路径查找、按 extent 读常规文件、按游标列目录

    能力边界见 Docs/Specs/Baleen/README.md 第五节：识别 superblock 与特性标志、读块组描述符
    与 inode、走 extent 树、解析线性目录项、读常规文件、列线性目录；不支持的特性、
    htree 索引目录、符号链接与内联数据都明确拒绝，不做兼容性猜测
    卷格式的接受与拒绝清单取 Packages/Common/Include/Storage/Ext4Features.hpp，与内核实现同源
    无运行库、不动态分配：缓冲都是实例内的固定数组，实例由调用方就地构造
*/

#pragma once
#include <stdint.h>

#include <Storage/BlockDevice.hpp>
#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        // Ext4 只读最小实现
        class Ext4FileSystem final : public Storage::FileSystem {
        public:
            // 卷内逻辑块大小的范围（Ext4 规范：1024、2048、4096）
            static constexpr uint32_t kMinBlockBytes = 1024;
            static constexpr uint32_t kMaxBlockBytes = 4096;
            // inode 大小的范围（128 或 256）
            static constexpr uint32_t kMaxInodeBytes = 256;

            // 挂载：读 superblock、校验魔数与特性清单；失败返回原因且实例保持未挂载
            Storage::Error Mount(Storage::BlockDevice& device, Storage::MountMode mode) override;
            // 卸载：清空挂载状态，实例可再次 Mount
            void Unmount() override;
            // 格式名，诊断用
            const char* Format() const override { return "ext4"; }
            // 能力：只读、支持线性目录遍历，不跟随符号链接，名字逐字节比较
            Storage::FileSystemCaps Caps() const override;
            // 读常规文件：按 extent 映射逐块读取，跨越文件尾时按实际长度回报
            Storage::Error Read(const Storage::FileId& id, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) override;
            // 目录遍历：从 start 起交出一个目录项，名字写入 name，游标推进到下一项
            // 游标是目录数据的字节偏移：调用方从 0 开始，把上一次的 start 原样传回，遍历结束返回 NotFound
            // "." 与 ".." 不作为目录项交出——统一路径的分量不含它们，目录项本身也不带来新信息
            // name 要放得下名字与结尾的空字符，放不下返回 NameTooLong 且不推进游标
            // 符号链接与非常规、非目录项返回 NotSupported，不静默跳过；htree 索引目录返回 NotSupported
            Storage::Error ReadDirectory(const Storage::FileId& id, uint64_t& start, char* name, uint32_t nameBytes, uint32_t& nameLength, Storage::FileInfo& info) override;
            // 卷标识（superblock 的 s_uuid，16 字节）；未挂载返回空
            const uint8_t* Uuid() const { return m_mounted ? m_uuid : nullptr; }
            // 卷内逻辑块大小；未挂载返回 0
            uint32_t BlockBytes() const { return m_blockBytes; }
            // 卷内 inode 总数；未挂载返回 0
            uint32_t InodesCount() const { return m_inodesCount; }
            // 卷内块总数；未挂载返回 0
            uint64_t BlocksCount() const { return m_blocksCount; }

        protected:
            // 根目录标识：inode 2
            Storage::Error RootId(Storage::FileId& id) override;
            // 一层查找：在线性目录里按名字逐字节比较
            Storage::Error FindChild(const Storage::FileId& directory, const char* name, uint32_t nameBytes,
                                     Storage::FileId& child) override;
            // 事实：inode 的类型与大小
            Storage::Error StatId(const Storage::FileId& id, Storage::FileInfo& info) override;

        private:
            // 解析出来的 inode 事实
            struct Inode {
                uint16_t mode = 0;        // 类型与权限
                uint32_t flags = 0;       // inode 标志
                uint64_t bytes = 0;       // 文件字节数（size_lo 与 size_high 合成）
                uint8_t block[60] = {};   // i_block：extent 树的根或传统块映射
            };

            // 从底层设备按字节读一段：接口要求按块对齐，本层按设备块换算并拼装
            Storage::Error ReadBytes(uint64_t byteOffset, void* buffer, uint32_t bytes);
            // 读块组描述符里的 inode 表位置
            Storage::Error GroupInodeTable(uint32_t group, uint64_t& table);
            // 读 inode 表里的一个 inode（inode 号从 1 起）
            Storage::Error ReadInode(uint64_t ino, Inode& inode);
            // extent 树查找：文件内逻辑块映射到物理块；physical 为 0 表示洞或未初始化
            Storage::Error MapBlock(const Inode& inode, uint32_t logical, uint64_t& physical);
            // extent 树查找的完整结果：物理块、同一 extent 内从该块起可连续读的块数与是否空洞
            // physical 为 0 表示洞或未初始化 extent；runBlocks 至少为 1，读到段尾为止
            Storage::Error MapRun(const Inode& inode, uint32_t logical, uint64_t& physical, uint32_t& runBlocks, bool& hole);
            // 按 inode 的数据映射读一段（目录与常规文件共用）
            Storage::Error ReadInodeBytes(const Inode& inode, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read);
            // 读一个卷内逻辑块到块缓冲
            Storage::Error ReadBlock(uint64_t block, void* buffer);

            Storage::BlockDevice* m_device = nullptr;   // 底层块设备（不拥有，使用期间须有效）
            bool m_mounted = false;                     // 是否已挂载
            uint32_t m_blockBytes = 0;                  // 卷内逻辑块大小
            uint32_t m_blocksPerGroup = 0;              // 每组块数
            uint32_t m_inodesPerGroup = 0;              // 每组 inode 数
            uint32_t m_inodesCount = 0;                 // inode 总数
            uint32_t m_inodeBytes = 0;                  // inode 大小
            uint32_t m_descBytes = 32;                  // 块组描述符大小
            uint32_t m_incompat = 0;                    // INCOMPAT 特性位
            uint64_t m_blocksCount = 0;                 // 卷内块总数
            uint8_t m_uuid[16] = {};                    // 卷标识
            uint8_t m_super[1024] = {};                 // superblock 原字节
            uint8_t m_unitBuffer[4096] = {};            // 底层设备单位缓冲（superblock、inode、描述符读取）
            uint8_t m_blockBuffer[kMaxBlockBytes] = {}; // 卷内块缓冲（extent 索引块、目录块、文件数据块）
        };
    }
}
