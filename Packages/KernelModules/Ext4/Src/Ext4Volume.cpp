/* Ext4Volume.cpp
    Ext4 只读卷的实现：superblock、块组描述符、inode、extent 树、线性目录查找与遍历

    从 BaleenCore 的只读实现移植（Packages/Baleen/Core/Src/Ext4.cpp），字段偏移与解析
    判据逐条保持；差异只有底层访问（Ext4Device 的函数指针）与对外形态（FileSystemOps）
    结构字段依据 Ext4 Disk Layout
    只做读取：不写卷、不回放日志、不分配内存；拒绝清单见 Storage/Ext4Features.hpp，
    当前按 kBaleenAccepted 判定（kKernelAccepted 是后续阶段的预留超集）
    挂载阶段先核完解析前提：特性必需位、卷干净位、64 位卷的高位块数与描述符大小，缺一即拒绝
    目标侧不牵入 64 位除法：块号换算用移位，inode 号与组号的除法都是 32 位
*/

#include <Ext4Volume.hpp>

#include <Libc.hpp>

#include <Storage/Ext4Features.hpp>

namespace KernelModules {
    namespace Ext4 {
        namespace {
            // superblock 的位置与字段偏移（字节偏移 1024 起 1024 字节）
            constexpr uint32_t kSuperOffset = 1024;               // superblock 在卷内的字节偏移
            constexpr uint32_t kSuperBytes = 1024;                // superblock 长度
            constexpr uint16_t kSuperMagic = 0xEF53;              // s_magic 的取值
            constexpr uint32_t kFieldMagic = 0x38;                // s_magic
            constexpr uint32_t kFieldState = 0x3A;                // s_state
            constexpr uint32_t kFieldInodesCount = 0x00;          // s_inodes_count
            constexpr uint32_t kFieldBlocksCountLo = 0x04;        // s_blocks_count_lo
            constexpr uint32_t kFieldLogBlockBytes = 0x18;        // s_log_block_size
            constexpr uint32_t kFieldBlocksPerGroup = 0x20;       // s_blocks_per_group
            constexpr uint32_t kFieldInodesPerGroup = 0x28;       // s_inodes_per_group
            constexpr uint32_t kFieldRevision = 0x4C;             // s_rev_level
            constexpr uint32_t kFieldInodeBytes = 0x58;           // s_inode_size（rev >= 1 时有效）
            constexpr uint32_t kFieldFeatureIncompat = 0x60;      // s_feature_incompat
            constexpr uint32_t kFieldFeatureRoCompat = 0x64;      // s_feature_ro_compat
            constexpr uint32_t kFieldUuid = 0x68;                 // s_uuid
            constexpr uint32_t kFieldDescBytes = 0xFE;            // s_desc_size（64 位特性时）
            constexpr uint32_t kFieldBlocksCountHi = 0x150;       // s_blocks_count_hi（64 位特性时）
            constexpr uint32_t kBlockBytesBase = 1024;            // 块大小 = 1024 << s_log_block_size
            constexpr uint16_t kStateValidFs = 0x0001;            // s_state 的 EXT4_VALID_FS 干净位

            // 块组描述符的字段偏移
            constexpr uint32_t kGroupInodeTableLo = 0x08;         // bg_inode_table_lo
            constexpr uint32_t kGroupInodeTableHi = 0x28;         // bg_inode_table_hi（64 位特性时）
            constexpr uint32_t kGroupDescBytesBase = 32;          // 32 位描述符长度
            constexpr uint32_t kGroupDescBytes64 = 64;            // 64 位描述符长度

            // inode 的字段偏移
            constexpr uint32_t kInodeMode = 0x00;                 // i_mode
            constexpr uint32_t kInodeBytesLo = 0x04;              // i_size_lo
            constexpr uint32_t kInodeFlags = 0x20;                // i_flags
            constexpr uint32_t kInodeBlock = 0x28;                // i_block[15]
            constexpr uint32_t kInodeBlockBytes = 60;             // i_block 的字节数
            constexpr uint32_t kInodeBytesHi = 0x6C;              // i_size_high

            // inode 标志与模式
            constexpr uint32_t kInodeFlagIndex = 0x00001000;      // htree 索引目录
            constexpr uint32_t kInodeFlagExtents = 0x00080000;    // 使用 extent 树
            constexpr uint32_t kInodeFlagInlineData = 0x10000000; // 内联数据
            constexpr uint16_t kModeTypeMask = 0xF000;            // 类型位
            constexpr uint16_t kModeRegular = 0x8000;             // 常规文件
            constexpr uint16_t kModeDirectory = 0x4000;           // 目录
            constexpr uint16_t kModeSymlink = 0xA000;             // 符号链接

            // extent 树的头与项
            constexpr uint16_t kExtentMagic = 0xF30A;             // eh_magic
            constexpr uint32_t kExtentHeaderBytes = 12;           // 节点头长度
            constexpr uint32_t kExtentEntryBytes = 12;            // 索引项与叶子项长度
            constexpr uint32_t kExtentMaxDepth = 5;               // 规范允许的最大深度
            constexpr uint32_t kExtentUnwritten = 32768;          // ee_len 的未初始化标志

            // 目录项
            constexpr uint32_t kDirEntryBytes = 8;                // 定长部分
            constexpr uint8_t kFileTypeRegular = 1;               // 常规文件
            constexpr uint8_t kFileTypeDirectory = 2;             // 目录
            constexpr uint8_t kFileTypeSymlink = 7;               // 符号链接

            // 按小端读取 16 位
            uint16_t Get16(const uint8_t* p) {
                return static_cast<uint16_t>(p[0] | (p[1] << 8));
            }

            // 按小端读取 32 位
            uint32_t Get32(const uint8_t* p) {
                uint32_t value = 0;
                for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (8 * i);
                return value;
            }

            // 2 的幂的移位量
            uint32_t ShiftOf(uint32_t value) {
                return static_cast<uint32_t>(__builtin_ctz(value));
            }

            // 名字逐字节比较：Ext4 区分大小写，卷内名字原样保存
            bool NameSame(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes) {
                if (leftBytes != rightBytes) return false;
                for (uint32_t i = 0; i < leftBytes; ++i) if (left[i] != right[i]) return false;
                return true;
            }

            // 是不是 "." 与 ".." 这两个自指项：统一路径的分量不含它们，目录遍历不把它们当目录项交出
            bool IsSelfEntry(const char* name, uint32_t nameBytes) {
                if (nameBytes == 1 && name[0] == '.') return true;
                return nameBytes == 2 && name[0] == '.' && name[1] == '.';
            }
        }

        Storage::FileSystemCaps Ext4Volume::Caps() const {
            Storage::FileSystemCaps caps;
            caps.writable = false;
            caps.directoryListing = true;
            caps.symlinks = false;
            caps.nameCase = Storage::NameCase::Sensitive;
            return caps;
        }

        const char* Ext4Volume::Format() const {
            return "ext4";
        }

        Storage::Error Ext4Volume::VolumeIdentity(uint8_t volumeId[16]) const {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (volumeId == nullptr) return Storage::Error::InvalidArgument;
            memcpy(volumeId, m_uuid, sizeof(m_uuid));
            return Storage::Error::Ok;
        }

        uint32_t Ext4Volume::BlockBytes() const {
            return m_mounted ? m_blockBytes : 0;
        }

        uint32_t Ext4Volume::InodesCount() const {
            return m_mounted ? m_inodesCount : 0;
        }

        uint64_t Ext4Volume::BlocksCount() const {
            return m_mounted ? m_blocksCount : 0;
        }

        void Ext4Volume::Unmount() {
            m_device = Ext4Device{};
            m_mounted = false;
            m_blockBytes = 0;
            m_blocksPerGroup = 0;
            m_inodesPerGroup = 0;
            m_inodesCount = 0;
            m_inodeBytes = 0;
            m_descBytes = kGroupDescBytesBase;
            m_incompat = 0;
            m_blocksCount = 0;
            for (uint32_t i = 0; i < sizeof(m_uuid); ++i) m_uuid[i] = 0;
        }

        Storage::Error Ext4Volume::Mount(const Ext4Device& device) {
            Unmount();
            // 底层设备事实：逻辑块大小是 2 的幂、缓冲放得下；读原语不能为空
            const uint32_t unit = device.blockBytes;
            if (device.Read == nullptr || unit == 0 || (unit & (unit - 1)) != 0 || unit > sizeof(m_unitBuffer)) return Storage::Error::InvalidArgument;
            if (device.capacityBytes < kSuperOffset + kSuperBytes) return Storage::Error::InvalidArgument;
            m_device = device;

            // superblock 在字节偏移 1024，跨越底层设备块读取
            if (const Storage::Error error = ReadBytes(kSuperOffset, m_super, kSuperBytes); error != Storage::Error::Ok) {
                Unmount();
                return error;
            }
            if (Get16(m_super + kFieldMagic) != kSuperMagic) {
                Unmount();
                return Storage::Error::Corrupt;
            }

            // 特性清单：未知或拒绝的 INCOMPAT 位、改变解析方式的 RO_COMPAT 位都拒绝
            const uint32_t incompat = Get32(m_super + kFieldFeatureIncompat);
            const uint32_t roCompat = Get32(m_super + kFieldFeatureRoCompat);
            // 当前只读挂载按 kBaleenAccepted 判定；kKernelAccepted 是后续阶段的预留超集，不在此接受
            if (!Storage::Ext4Features::IncompatSupported(incompat, Storage::Ext4Features::kBaleenAccepted)
                || !Storage::Ext4Features::RoCompatSupported(roCompat)) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            // 解析前提位：目录项与 extent 树的布局前提，缺失即在挂载阶段拒绝，不等读到目录项或 extent 才报
            if (!Storage::Ext4Features::IncompatHasRequired(incompat)) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            // 干净位：未干净卸载的卷需要日志回放才能得到一致视图，只读路径不回放，与 RECOVER 位同样拒绝
            if ((Get16(m_super + kFieldState) & kStateValidFs) == 0) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            m_incompat = incompat;

            // 块大小与基本字段
            const uint32_t logBlockBytes = Get32(m_super + kFieldLogBlockBytes);
            if (logBlockBytes > 2) {   // 1024 << 0..2
                Unmount();
                return Storage::Error::NotSupported;
            }
            const bool has64Bit = (incompat & Storage::Ext4Features::kIncompat64Bit) != 0;
            // 高 32 位块数非零即卷超过 2 的 32 次方块：读路径的块号与 LBA 表示不了，明确拒绝而不是截断解析
            if (has64Bit && Get32(m_super + kFieldBlocksCountHi) != 0) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            m_blockBytes = kBlockBytesBase << logBlockBytes;
            m_inodesCount = Get32(m_super + kFieldInodesCount);
            m_blocksCount = Get32(m_super + kFieldBlocksCountLo);
            m_blocksPerGroup = Get32(m_super + kFieldBlocksPerGroup);
            m_inodesPerGroup = Get32(m_super + kFieldInodesPerGroup);
            const uint32_t revision = Get32(m_super + kFieldRevision);
            m_inodeBytes = revision == 0 ? 128 : Get16(m_super + kFieldInodeBytes);
            m_descBytes = has64Bit ? Get16(m_super + kFieldDescBytes) : kGroupDescBytesBase;

            // 字段一致性：任一项越界都按结构非法拒绝，不猜测
            if (m_descBytes < kGroupDescBytesBase || m_descBytes > kGroupDescBytes64 || (m_descBytes & 1) != 0) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            // 64 位卷的描述符必须恰为 64 字节：32 字节会让 bg_inode_table_hi 被丢弃，块组号大于 0 的 inode 表读错位置
            if (has64Bit && m_descBytes != kGroupDescBytes64) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            if (m_inodeBytes < 128 || m_inodeBytes > kMaxInodeBytes || (m_inodeBytes & 3) != 0) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            if (m_blocksPerGroup == 0 || m_inodesPerGroup == 0 || m_inodesCount == 0 || m_blocksCount == 0) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            memcpy(m_uuid, m_super + kFieldUuid, sizeof(m_uuid));
            m_mounted = true;
            return Storage::Error::Ok;
        }

        Storage::Error Ext4Volume::ReadBytes(uint64_t byteOffset, void* buffer, uint32_t bytes) {
            if (!m_mounted && m_device.Read == nullptr) return Storage::Error::InvalidArgument;
            const uint32_t unit = m_device.blockBytes;
            if (unit == 0 || (unit & (unit - 1)) != 0 || unit > sizeof(m_unitBuffer)) return Storage::Error::InvalidArgument;
            const uint32_t shift = ShiftOf(unit);
            uint8_t* dest = static_cast<uint8_t*>(buffer);
            uint32_t done = 0;
            while (done < bytes) {
                const uint64_t at = byteOffset + done;
                const uint64_t block = at >> shift;
                const uint32_t inside = static_cast<uint32_t>(at & (unit - 1));
                const uint32_t chunk = (unit - inside) < (bytes - done) ? (unit - inside) : (bytes - done);
                if (const Storage::Error error = m_device.Read(m_device.context, block << shift, m_unitBuffer, unit); error != Storage::Error::Ok) return error;
                memcpy(dest + done, m_unitBuffer + inside, chunk);
                done += chunk;
            }
            return Storage::Error::Ok;
        }

        Storage::Error Ext4Volume::GroupInodeTable(uint32_t group, uint64_t& table) {
            // 块组描述符表：块大小为 1024 时 superblock 占块 1、表从块 2 起；否则表从块 1 起
            const uint64_t tableBlock = m_blockBytes == kBlockBytesBase ? 2 : 1;
            const uint64_t offset = (tableBlock << ShiftOf(m_blockBytes)) + static_cast<uint64_t>(group) * m_descBytes;
            uint8_t raw[kGroupDescBytes64] = {};
            if (const Storage::Error error = ReadBytes(offset, raw, m_descBytes); error != Storage::Error::Ok) return error;
            table = Get32(raw + kGroupInodeTableLo);
            if (m_descBytes >= kGroupDescBytes64) table |= static_cast<uint64_t>(Get32(raw + kGroupInodeTableHi)) << 32;
            if (table == 0 || table >= m_blocksCount) return Storage::Error::Corrupt;
            return Storage::Error::Ok;
        }

        Storage::Error Ext4Volume::ReadInode(uint64_t ino, Inode& inode) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (ino == 0 || ino > m_inodesCount) return Storage::Error::Corrupt;
            const uint32_t index = static_cast<uint32_t>(ino - 1);
            const uint32_t group = index / m_inodesPerGroup;   // 32 位除法，不牵入 64 位辅助
            const uint32_t inGroup = index % m_inodesPerGroup;
            uint64_t table = 0;
            if (const Storage::Error error = GroupInodeTable(group, table); error != Storage::Error::Ok) return error;
            const uint64_t offset = (table << ShiftOf(m_blockBytes)) + static_cast<uint64_t>(inGroup) * m_inodeBytes;
            uint8_t raw[kMaxInodeBytes] = {};
            if (const Storage::Error error = ReadBytes(offset, raw, m_inodeBytes); error != Storage::Error::Ok) return error;
            inode.mode = Get16(raw + kInodeMode);
            inode.flags = Get32(raw + kInodeFlags);
            inode.bytes = Get32(raw + kInodeBytesLo) | (static_cast<uint64_t>(Get32(raw + kInodeBytesHi)) << 32);
            memcpy(inode.block, raw + kInodeBlock, kInodeBlockBytes);
            return Storage::Error::Ok;
        }

        Storage::Error Ext4Volume::MapRun(const Inode& inode, uint32_t logical, uint64_t& physical, uint32_t& runBlocks, bool& hole) {
            // 只支持 extent 树：传统块映射、内联数据都不在最小实现范围内
            if ((inode.flags & kInodeFlagInlineData) != 0) return Storage::Error::NotSupported;
            if ((inode.flags & kInodeFlagExtents) == 0) return Storage::Error::NotSupported;
            const uint8_t* node = inode.block;
            uint32_t nodeBytes = kInodeBlockBytes;
            for (uint32_t depth = 0; depth <= kExtentMaxDepth; ++depth) {
                if (nodeBytes < kExtentHeaderBytes) return Storage::Error::Corrupt;
                if (Get16(node) != kExtentMagic) return Storage::Error::Corrupt;
                const uint32_t entries = Get16(node + 2);
                const uint32_t maximum = Get16(node + 4);
                const uint32_t height = Get16(node + 6);
                if (entries > maximum) return Storage::Error::Corrupt;
                if (kExtentHeaderBytes + entries * kExtentEntryBytes > nodeBytes) return Storage::Error::Corrupt;
                if (height == 0) {
                    // 叶子节点：按逻辑块落在哪个 extent 里取物理块与段内剩余块数
                    for (uint32_t i = 0; i < entries; ++i) {
                        const uint8_t* const entry = node + kExtentHeaderBytes + i * kExtentEntryBytes;
                        const uint32_t first = Get32(entry);
                        uint32_t length = Get16(entry + 4);
                        const bool unwritten = length > kExtentUnwritten;
                        if (unwritten) length -= kExtentUnwritten;
                        if (length == 0 || logical < first) continue;
                        const uint32_t delta = logical - first;
                        if (delta >= length) continue;
                        runBlocks = length - delta;   // 同一 extent 内从本块起可连续读的块数
                        // 未初始化 extent 按读零处理
                        if (unwritten) {
                            hole = true;
                            physical = 0;
                            return Storage::Error::Ok;
                        }
                        hole = false;
                        physical = Get32(entry + 8) | (static_cast<uint64_t>(Get16(entry + 6)) << 32);
                        physical += delta;
                        if (physical >= m_blocksCount) return Storage::Error::Corrupt;
                        return Storage::Error::Ok;
                    }
                    hole = true;        // 没有映射：洞，读为零；段长未知，按单块处理
                    runBlocks = 1;
                    physical = 0;
                    return Storage::Error::Ok;
                }
                // 索引节点：取键值不大于目标逻辑块的最后一项，下降到子块
                bool found = false;
                uint64_t child = 0;
                for (uint32_t i = 0; i < entries; ++i) {
                    const uint8_t* const entry = node + kExtentHeaderBytes + i * kExtentEntryBytes;
                    const uint32_t key = Get32(entry);
                    if (key > logical) break;
                    child = Get32(entry + 4) | (static_cast<uint64_t>(Get16(entry + 8)) << 32);
                    found = true;
                }
                if (!found || child == 0 || child >= m_blocksCount) return Storage::Error::Corrupt;
                if (const Storage::Error error = ReadBlock(child, m_blockBuffer); error != Storage::Error::Ok) return error;
                node = m_blockBuffer;
                nodeBytes = m_blockBytes;
            }
            return Storage::Error::Corrupt;   // 深度超出规范上限
        }

        Storage::Error Ext4Volume::ReadBlock(uint64_t block, void* buffer) {
            if (m_device.Read == nullptr || m_blockBytes == 0) return Storage::Error::InvalidArgument;
            if (block >= m_blocksCount) return Storage::Error::Corrupt;
            return m_device.Read(m_device.context, block << ShiftOf(m_blockBytes), buffer, m_blockBytes);
        }

        Storage::Error Ext4Volume::ReadInodeBytes(const Inode& inode, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) {
            read = 0;
            if (inode.bytes <= offset) return Storage::Error::Ok;   // 已到文件尾
            uint64_t available = inode.bytes - offset;
            uint32_t want = bytes;
            if (available < want) want = static_cast<uint32_t>(available);
            if ((offset >> 32) != 0) return Storage::Error::NotSupported;   // 4 GiB 以上的文件不在最小范围内
            const uint32_t shift = ShiftOf(m_blockBytes);
            uint8_t* dest = static_cast<uint8_t*>(buffer);
            uint32_t done = 0;
            while (done < want) {
                const uint64_t position = offset + done;
                const uint32_t logical = static_cast<uint32_t>(position) >> shift;
                const uint32_t inside = static_cast<uint32_t>(position) & (m_blockBytes - 1);
                const uint32_t chunk = (m_blockBytes - inside) < (want - done) ? (m_blockBytes - inside) : (want - done);
                uint64_t physical = 0;
                uint32_t runBlocks = 0;
                bool hole = false;
                if (const Storage::Error error = MapRun(inode, logical, physical, runBlocks, hole); error != Storage::Error::Ok) return error;
                if (hole) {
                    // 洞与未初始化 extent 读为零；整块对齐时按整段清零，不逐块映射
                    uint32_t zeros = chunk;
                    if (inside == 0) {
                        const uint32_t runBytes = runBlocks * m_blockBytes;
                        const uint32_t remain = want - done;
                        zeros = runBytes < remain ? runBytes : remain;
                    }
                    memset(dest + done, 0, zeros);
                    done += zeros;
                    continue;
                }
                // 整块对齐且同一 extent 内还有后续块：一次读进调用方缓冲，块设备往返按段合并
                // 尾部不足一块的部分留给下一轮按块读，保持块对齐前提
                if (inside == 0 && runBlocks > 1) {
                    const uint32_t runBytes = runBlocks * m_blockBytes;
                    const uint32_t remain = want - done;
                    uint32_t readBytes = runBytes < remain ? runBytes : remain;
                    readBytes -= readBytes % m_blockBytes;
                    if (readBytes > 0) {
                        if (const Storage::Error error = m_device.Read(m_device.context, physical << shift, dest + done, readBytes); error != Storage::Error::Ok) return error;
                        done += readBytes;
                        continue;
                    }
                }
                if (const Storage::Error error = ReadBlock(physical, m_blockBuffer); error != Storage::Error::Ok) return error;
                memcpy(dest + done, m_blockBuffer + inside, chunk);
                done += chunk;
            }
            read = done;
            return Storage::Error::Ok;
        }

        Storage::Error Ext4Volume::ResolvePath(const char* path, uint64_t& object) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (path == nullptr || path[0] != '/') return Storage::Error::InvalidArgument;
            uint32_t length = 0;
            while (path[length] != '\0') {
                if (length + 1 > Storage::PathRules::kMaxBytes) return Storage::Error::NameTooLong;
                ++length;
            }
            // 统一路径语法：绝对、分量非空、不含 "." 与 ".."、不以分隔符结尾；Vfs 已预校验，本层再核一次
            // 根路径 "/" 直接给出根目录；其余从根分隔符之后逐段查找
            if (length > 1 && path[length - 1] == '/') return Storage::Error::InvalidArgument;
            uint64_t current = kRootInode;
            uint32_t position = 1;
            while (position < length) {
                const uint32_t nameBegin = position;
                while (position < length && path[position] != '/') ++position;
                const uint32_t nameBytes = position - nameBegin;
                if (nameBytes == 0) return Storage::Error::InvalidArgument;   // 连续分隔符留下空分量
                if (nameBytes > Storage::PathRules::kMaxNameBytes) return Storage::Error::NameTooLong;
                if (IsSelfEntry(path + nameBegin, nameBytes)) return Storage::Error::InvalidArgument;
                uint64_t child = 0;
                if (const Storage::Error error = FindChild(current, path + nameBegin, nameBytes, child); error != Storage::Error::Ok) return error;
                current = child;
                if (position < length) ++position;   // 跳过分量后的分隔符
            }
            object = current;
            return Storage::Error::Ok;
        }

        Storage::Error Ext4Volume::StatInode(uint64_t object, Storage::FileInfo& info) {
            Inode inode;
            if (const Storage::Error error = ReadInode(object, inode); error != Storage::Error::Ok) return error;
            const uint16_t type = static_cast<uint16_t>(inode.mode & kModeTypeMask);
            if (type == kModeSymlink) return Storage::Error::NotSupported;   // 不跟随符号链接
            if (type != kModeRegular && type != kModeDirectory) return Storage::Error::NotSupported;
            info.id = object;
            info.directory = type == kModeDirectory;
            info.bytes = info.directory ? 0 : inode.bytes;
            return Storage::Error::Ok;
        }

        Storage::Error Ext4Volume::StatObject(uint64_t object, Storage::FileInfo& info) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            return StatInode(object, info);
        }

        Storage::Error Ext4Volume::Stat(const char* path, Storage::FileInfo& info) {
            uint64_t object = 0;
            if (const Storage::Error error = ResolvePath(path, object); error != Storage::Error::Ok) return error;
            return StatInode(object, info);
        }

        Storage::Error Ext4Volume::Open(const char* path, uint64_t& object) {
            return ResolvePath(path, object);
        }

        Storage::Error Ext4Volume::FindChild(uint64_t directory, const char* name, uint32_t nameBytes, uint64_t& child) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (name == nullptr || nameBytes == 0) return Storage::Error::InvalidArgument;
            Inode dirInode;
            if (const Storage::Error error = ReadInode(directory, dirInode); error != Storage::Error::Ok) return error;
            if ((dirInode.mode & kModeTypeMask) != kModeDirectory) return Storage::Error::NotDirectory;
            if ((dirInode.flags & kInodeFlagIndex) != 0) return Storage::Error::NotSupported;   // htree 索引目录不在最小范围内
            uint64_t position = 0;
            while (position < dirInode.bytes) {
                uint32_t got = 0;
                if (const Storage::Error error = ReadInodeBytes(dirInode, position, m_blockBuffer, m_blockBytes, got); error != Storage::Error::Ok) return error;
                if (got < kDirEntryBytes) break;   // 读不出完整目录项即结束
                uint32_t cursor = 0;
                while (cursor + kDirEntryBytes <= got) {
                    const uint32_t ino = Get32(m_blockBuffer + cursor);
                    const uint32_t recLen = Get16(m_blockBuffer + cursor + 4);
                    const uint32_t nameLen = m_blockBuffer[cursor + 6];
                    const uint8_t fileType = m_blockBuffer[cursor + 7];
                    // 目录项长度按 4 对齐、落在本块内；非法的 rec_len 视为结构损坏
                    if (recLen < kDirEntryBytes || (recLen & 3) != 0 || cursor + recLen > got) return Storage::Error::Corrupt;
                    if (ino != 0 && nameLen <= recLen - kDirEntryBytes) {
                        const char* const entryName = reinterpret_cast<const char*>(m_blockBuffer + cursor + kDirEntryBytes);
                        if (NameSame(entryName, nameLen, name, nameBytes)) {
                            if (fileType == kFileTypeSymlink) return Storage::Error::NotSupported;
                            if (fileType != kFileTypeRegular && fileType != kFileTypeDirectory) return Storage::Error::NotSupported;
                            child = ino;
                            return Storage::Error::Ok;
                        }
                    }
                    cursor += recLen;
                }
                position += got;
            }
            return Storage::Error::NotFound;
        }

        Storage::Error Ext4Volume::Read(uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) {
            read = 0;
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            Inode inode;
            if (const Storage::Error error = ReadInode(object, inode); error != Storage::Error::Ok) return error;
            const uint16_t type = static_cast<uint16_t>(inode.mode & kModeTypeMask);
            if (type == kModeDirectory) return Storage::Error::IsDirectory;
            if (type != kModeRegular) return Storage::Error::NotSupported;
            return ReadInodeBytes(inode, offset, buffer, bytes, read);
        }

        Storage::Error Ext4Volume::ReadDirectory(uint64_t object, uint64_t& cursor, char* name, uint32_t nameBytes, uint32_t& nameLength, Storage::FileInfo& info) {
            nameLength = 0;
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (name == nullptr || nameBytes == 0) return Storage::Error::InvalidArgument;
            Inode dirInode;
            if (const Storage::Error error = ReadInode(object, dirInode); error != Storage::Error::Ok) return error;
            if ((dirInode.mode & kModeTypeMask) != kModeDirectory) return Storage::Error::NotDirectory;
            if ((dirInode.flags & kInodeFlagIndex) != 0) return Storage::Error::NotSupported;   // htree 索引目录不在最小范围内
            // 游标是目录数据的字节偏移：每次从它所在的那一块起，交出一个合格目录项
            while (cursor < dirInode.bytes) {
                // 块大小是 2 的幂：用位与取块基址，不牵入 64 位除法
                const uint64_t blockBase = cursor & ~static_cast<uint64_t>(m_blockBytes - 1);
                uint32_t got = 0;
                if (const Storage::Error error = ReadInodeBytes(dirInode, blockBase, m_blockBuffer, m_blockBytes, got); error != Storage::Error::Ok) return error;
                if (got < kDirEntryBytes) return Storage::Error::NotFound;   // 连定长部分都读不出：遍历到此为止
                const uint32_t limit = got;
                uint32_t position = static_cast<uint32_t>(cursor - blockBase);
                while (position + kDirEntryBytes <= limit) {
                    const uint32_t ino = Get32(m_blockBuffer + position);
                    const uint32_t recLen = Get16(m_blockBuffer + position + 4);
                    const uint32_t nameLen = m_blockBuffer[position + 6];
                    // 与查找同一条判据：长度按 4 对齐、落在本块内，名字放得进本项
                    if (recLen < kDirEntryBytes || (recLen & 3) != 0 || position + recLen > limit) return Storage::Error::Corrupt;
                    const bool usable = ino != 0 && nameLen != 0 && nameLen <= recLen - kDirEntryBytes;
                    if (usable) {
                        const char* const entryName = reinterpret_cast<const char*>(m_blockBuffer + position + kDirEntryBytes);
                        if (!IsSelfEntry(entryName, nameLen)) {
                            // 名字先拷出来再取子项事实：取事实会复用实例内的块缓冲
                            if (nameLen + 1 > nameBytes) return Storage::Error::NameTooLong;   // 不推进游标，换大缓冲可以重试
                            for (uint32_t i = 0; i < nameLen; ++i) name[i] = entryName[i];
                            name[nameLen] = '\0';
                            Storage::FileInfo child{};
                            if (const Storage::Error error = StatInode(ino, child); error != Storage::Error::Ok) return error;
                            nameLength = nameLen;
                            info = child;
                            cursor = blockBase + position + recLen;
                            return Storage::Error::Ok;
                        }
                    }
                    position += recLen;
                }
                // 本块剩下的一整段里没有合格目录项：推进到本块之后继续
                cursor = blockBase + limit;
                if (got < m_blockBytes) break;   // 目录数据到此为止
            }
            return Storage::Error::NotFound;
        }
    }
}
