/* Fat32Volume.cpp
    FAT32 只读卷的实现：BPB、FAT 链、目录项（8.3 与 LFN）与文件读取

    依据 Microsoft FAT32 File System Specification（fatgen103）；只读、不写卷、不分配内存
    名字命中统一按 ASCII 折叠；LFN 保留真实大小写，含非 ASCII 字符时退回 8.3 名（不猜字符集）
    FAT 链遍历都有上界（簇数），遇到自环、坏簇或提前结束按结构非法拒绝
*/

#include <Fat32Volume.hpp>

#include <Libc.hpp>

namespace KernelModules {
    namespace Fat32 {
        namespace {
            // BPB 字段偏移（FAT32 规范）
            constexpr uint32_t kBpbBytesPerSector = 0x0B;
            constexpr uint32_t kBpbSectorsPerCluster = 0x0D;
            constexpr uint32_t kBpbReservedSectors = 0x0E;
            constexpr uint32_t kBpbFatCount = 0x10;
            constexpr uint32_t kBpbRootEntries = 0x11;
            constexpr uint32_t kBpbTotalSectors16 = 0x13;
            constexpr uint32_t kBpbFatSize16 = 0x16;
            constexpr uint32_t kBpbTotalSectors32 = 0x20;
            constexpr uint32_t kBpbFatSize32 = 0x24;
            constexpr uint32_t kBpbRootCluster = 0x2C;
            constexpr uint32_t kBpbVolumeId = 0x43;
            constexpr uint32_t kBpbVolumeLabel = 0x47;
            constexpr uint32_t kBpbFsType = 0x52;
            // 目录项字段偏移
            constexpr uint32_t kEntryAttr = 0x0B;
            constexpr uint32_t kEntryClusterHigh = 0x14;
            constexpr uint32_t kEntryClusterLow = 0x1A;
            constexpr uint32_t kEntryBytes = 0x1C;
            // 目录项属性
            constexpr uint8_t kAttrReadOnly = 0x01;
            constexpr uint8_t kAttrHidden = 0x02;
            constexpr uint8_t kAttrSystem = 0x04;
            constexpr uint8_t kAttrVolumeLabel = 0x08;
            constexpr uint8_t kAttrDirectory = 0x10;
            constexpr uint8_t kAttrLongName = 0x0F;
            // FAT 项取值
            constexpr uint32_t kFatMask = 0x0FFFFFFFu;
            constexpr uint32_t kFatBad = 0x0FFFFFF7u;
            constexpr uint32_t kFatEndMin = 0x0FFFFFF8u;

            // 按小端读 16 位
            uint16_t Get16(const uint8_t* p) {
                return static_cast<uint16_t>(p[0] | (p[1] << 8));
            }

            // 按小端读 32 位
            uint32_t Get32(const uint8_t* p) {
                uint32_t value = 0;
                for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (8 * i);
                return value;
            }

            // 2 的幂判定
            bool IsPowerOfTwo(uint32_t value) {
                return value != 0 && (value & (value - 1)) == 0;
            }

            // ASCII 折叠成小写
            char FoldAscii(char byte) {
                return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte - 'A' + 'a') : byte;
            }

            // 名字是否相等：两侧都按 ASCII 折叠比较
            bool NameSameFold(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes) {
                if (leftBytes != rightBytes) return false;
                for (uint32_t i = 0; i < leftBytes; ++i) {
                    if (FoldAscii(left[i]) != FoldAscii(right[i])) return false;
                }
                return true;
            }
        }

        const char* Fat32Volume::Format() const {
            return "fat32";
        }

        Storage::FileSystemCaps Fat32Volume::Caps() const {
            Storage::FileSystemCaps caps;
            caps.writable = false;
            caps.directoryListing = true;
            caps.symlinks = false;
            caps.nameCase = Storage::NameCase::Insensitive;
            return caps;
        }

        uint64_t Fat32Volume::RootObject() const {
            return m_mounted ? kRootObject : 0;
        }

        void Fat32Volume::Unmount() {
            m_device = FatDevice{};
            m_mounted = false;
            m_bytesPerSector = 0;
            m_sectorsPerCluster = 0;
            m_clusterBytes = 0;
            m_reservedSectors = 0;
            m_fatCount = 0;
            m_fatSectors = 0;
            m_rootCluster = 0;
            m_clusterCount = 0;
            m_dataStartLba = 0;
            for (uint32_t i = 0; i < 4; ++i) m_volumeId[i] = 0;
            for (uint32_t i = 0; i < 11; ++i) m_volumeLabel[i] = 0;
        }

        Storage::Error Fat32Volume::ReadBytes(uint64_t byteOffset, void* buffer, uint32_t bytes) {
            const uint32_t unit = m_device.blockBytes;
            if (m_device.Read == nullptr || unit == 0 || (unit & (unit - 1)) != 0 || unit > sizeof(m_unit)) return Storage::Error::InvalidArgument;
            const uint32_t shift = static_cast<uint32_t>(__builtin_ctz(unit));
            uint8_t* dest = static_cast<uint8_t*>(buffer);
            uint32_t done = 0;
            while (done < bytes) {
                const uint64_t at = byteOffset + done;
                const uint64_t block = at >> shift;
                const uint32_t inside = static_cast<uint32_t>(at & (unit - 1));
                const uint32_t chunk = (unit - inside) < (bytes - done) ? (unit - inside) : (bytes - done);
                if (const Storage::Error error = m_device.Read(m_device.context, block << shift, m_unit, unit); error != Storage::Error::Ok) return error;
                memcpy(dest + done, m_unit + inside, chunk);
                done += chunk;
            }
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::Mount(const FatDevice& device) {
            Unmount();
            const uint32_t unit = device.blockBytes;
            if (device.Read == nullptr || unit == 0 || (unit & (unit - 1)) != 0 || unit > sizeof(m_unit)) return Storage::Error::InvalidArgument;
            if (device.capacityBytes < 512) return Storage::Error::InvalidArgument;
            m_device = device;

            // BPB 在卷起始的 512 字节里
            uint8_t bpb[512] = {};
            if (const Storage::Error error = ReadBytes(0, bpb, sizeof(bpb)); error != Storage::Error::Ok) {
                Unmount();
                return error;
            }
            m_bytesPerSector = Get16(bpb + kBpbBytesPerSector);
            m_sectorsPerCluster = bpb[kBpbSectorsPerCluster];
            m_reservedSectors = Get16(bpb + kBpbReservedSectors);
            m_fatCount = bpb[kBpbFatCount];
            const uint16_t rootEntries = Get16(bpb + kBpbRootEntries);
            const uint16_t totalSectors16 = Get16(bpb + kBpbTotalSectors16);
            const uint16_t fatSize16 = Get16(bpb + kBpbFatSize16);
            const uint32_t totalSectors32 = Get32(bpb + kBpbTotalSectors32);
            m_fatSectors = Get32(bpb + kBpbFatSize32);
            m_rootCluster = Get32(bpb + kBpbRootCluster);
            for (uint32_t i = 0; i < 4; ++i) m_volumeId[i] = bpb[kBpbVolumeId + i];
            for (uint32_t i = 0; i < 11; ++i) m_volumeLabel[i] = bpb[kBpbVolumeLabel + i];

            // 只做 FAT32：扇区/簇几何合法，根目录必须是簇链（rootEntries 为 0），FAT32 字段非零
            if (!IsPowerOfTwo(m_bytesPerSector) || m_bytesPerSector < 512 || m_bytesPerSector > 4096) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            if (!IsPowerOfTwo(m_sectorsPerCluster) || m_sectorsPerCluster == 0 || m_sectorsPerCluster > 128) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            m_clusterBytes = m_bytesPerSector * m_sectorsPerCluster;
            if (m_clusterBytes > kMaxClusterBytes) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            if (m_fatCount == 0 || m_fatCount > 4 || m_fatSectors == 0 || m_reservedSectors == 0) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            if (rootEntries != 0 || fatSize16 != 0) {
                Unmount();
                return Storage::Error::NotSupported;   // FAT12/16 形态，不在本模块范围
            }
            if (bpb[kBpbFsType] != 'F' || bpb[kBpbFsType + 1] != 'A' || bpb[kBpbFsType + 2] != 'T'
                || bpb[kBpbFsType + 3] != '3' || bpb[kBpbFsType + 4] != '2') {
                Unmount();
                return Storage::Error::NotSupported;
            }
            const uint64_t totalSectors = totalSectors16 != 0 ? totalSectors16 : totalSectors32;
            m_dataStartLba = static_cast<uint64_t>(m_reservedSectors) + static_cast<uint64_t>(m_fatCount) * m_fatSectors;
            if (totalSectors <= m_dataStartLba) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            m_clusterCount = static_cast<uint32_t>((totalSectors - m_dataStartLba) / m_sectorsPerCluster);
            if (m_clusterCount == 0 || m_rootCluster < 2 || m_rootCluster >= m_clusterCount + 2) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            if (totalSectors * m_bytesPerSector > device.capacityBytes) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            m_mounted = true;
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::VolumeIdentity(uint8_t volumeId[16]) const {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (volumeId == nullptr) return Storage::Error::InvalidArgument;
            // 卷标去掉尾部空格；序号与卷标都为空时按“无标识能力”返回
            uint32_t labelBytes = 11;
            while (labelBytes > 0 && (m_volumeLabel[labelBytes - 1] == ' ' || m_volumeLabel[labelBytes - 1] == 0)) --labelBytes;
            bool serialZero = true;
            for (uint32_t i = 0; i < 4; ++i) if (m_volumeId[i] != 0) serialZero = false;
            if (serialZero && labelBytes == 0) return Storage::Error::NotSupported;
            for (uint32_t i = 0; i < 16; ++i) volumeId[i] = 0;
            for (uint32_t i = 0; i < 4; ++i) volumeId[i] = m_volumeId[i];
            for (uint32_t i = 0; i < labelBytes && 4 + i < 16; ++i) volumeId[4 + i] = m_volumeLabel[i];
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::FatEntry(uint32_t cluster, uint32_t& next) {
            if (cluster < 2 || cluster >= m_clusterCount + 2) return Storage::Error::Corrupt;
            const uint64_t offset = static_cast<uint64_t>(m_reservedSectors) * m_bytesPerSector + static_cast<uint64_t>(cluster) * 4;
            uint8_t raw[4] = {};
            if (const Storage::Error error = ReadBytes(offset, raw, sizeof(raw)); error != Storage::Error::Ok) return error;
            next = Get32(raw) & kFatMask;
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::NextCluster(uint32_t cluster, uint32_t& next, bool& end) {
            uint32_t value = 0;
            if (const Storage::Error error = FatEntry(cluster, value); error != Storage::Error::Ok) return error;
            if (value >= kFatEndMin) {
                end = true;
                return Storage::Error::Ok;
            }
            if (value == kFatBad || value < 2 || value >= m_clusterCount + 2 || value == cluster) return Storage::Error::Corrupt;   // 自环按结构非法拒绝
            end = false;
            next = value;
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::ReadCluster(uint32_t cluster, void* buffer) {
            if (cluster < 2 || cluster >= m_clusterCount + 2) return Storage::Error::Corrupt;
            const uint64_t lba = m_dataStartLba + static_cast<uint64_t>(cluster - 2) * m_sectorsPerCluster;
            return ReadBytes(lba * m_bytesPerSector, buffer, m_clusterBytes);
        }

        Storage::Error Fat32Volume::SeekCluster(uint32_t firstCluster, uint32_t skipClusters, uint32_t& cluster) {
            uint32_t current = firstCluster;
            for (uint32_t step = 0; step < skipClusters; ++step) {
                uint32_t next = 0;
                bool end = false;
                if (const Storage::Error error = NextCluster(current, next, end); error != Storage::Error::Ok) return error;
                if (end) return Storage::Error::Corrupt;   // 链比预期短
                current = next;
            }
            cluster = current;
            return Storage::Error::Ok;
        }

        void Fat32Volume::ShortName(const uint8_t* raw, uint32_t& nameBytes, char* name) {
            // 8.3：主名 8 字节 + 扩展名 3 字节，空格填充；0x05 是 0xE5 的转义
            const bool lowerBase = (raw[12] & 0x08) != 0;
            const bool lowerExt = (raw[12] & 0x10) != 0;
            char base[8] = {};
            uint32_t baseBytes = 0;
            for (uint32_t i = 0; i < 8; ++i) {
                uint8_t byte = raw[i];
                if (i == 0 && byte == 0x05) byte = 0xE5;
                if (byte == ' ') break;
                base[baseBytes++] = lowerBase ? FoldAscii(static_cast<char>(byte)) : static_cast<char>(byte);
            }
            char ext[3] = {};
            uint32_t extBytes = 0;
            for (uint32_t i = 0; i < 3; ++i) {
                const uint8_t byte = raw[8 + i];
                if (byte == ' ') break;
                ext[extBytes++] = lowerExt ? FoldAscii(static_cast<char>(byte)) : static_cast<char>(byte);
            }
            nameBytes = 0;
            for (uint32_t i = 0; i < baseBytes; ++i) name[nameBytes++] = base[i];
            if (extBytes != 0) {
                name[nameBytes++] = '.';
                for (uint32_t i = 0; i < extBytes; ++i) name[nameBytes++] = ext[i];
            }
            name[nameBytes] = '\0';
        }

        uint8_t Fat32Volume::ShortNameChecksum(const uint8_t* raw) {
            uint8_t sum = 0;
            for (uint32_t i = 0; i < 11; ++i) sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + raw[i]);
            return sum;
        }

        // 簇在卷内的字节偏移
        uint64_t Fat32Volume::ClusterByteOffset(uint32_t cluster) {
            const uint64_t lba = m_dataStartLba + static_cast<uint64_t>(cluster - 2) * m_sectorsPerCluster;
            return lba * m_bytesPerSector;
        }

        // 读目录项：offset 处的 32 字节；可用性由遍历逻辑保证
        Storage::Error Fat32Volume::ReadEntry(uint64_t offset, Entry& entry) {
            uint8_t raw[32] = {};
            if (const Storage::Error error = ReadBytes(offset, raw, sizeof(raw)); error != Storage::Error::Ok) return error;
            entry = Entry{};
            entry.offset = offset;
            entry.directory = (raw[kEntryAttr] & kAttrDirectory) != 0;
            entry.bytes = entry.directory ? 0 : Get32(raw + kEntryBytes);
            entry.firstCluster = (static_cast<uint32_t>(Get16(raw + kEntryClusterHigh)) << 16) | Get16(raw + kEntryClusterLow);
            // 目录项首簇为 0 只在 ".." 指向根目录时合法；真实目录的首簇在 BeginWalk 里核对
            if (!entry.directory && entry.bytes != 0 && entry.firstCluster == 0) return Storage::Error::Corrupt;
            // 先按 8.3 填显示名与短名；NextEntry 若发现合法 LFN 会用 LFN 覆盖显示名
            ShortName(raw, entry.nameBytes, entry.name);
            entry.shortNameBytes = entry.nameBytes;
            memcpy(entry.shortName, entry.name, entry.nameBytes + 1);
            return Storage::Error::Ok;
        }

        // 按对象标识取目录项事实：根目录用合成目录项（起始簇来自挂载解析），其余读目录项
        Storage::Error Fat32Volume::EntryForObject(uint64_t object, Entry& entry) {
            if (object == kRootObject) {
                entry = Entry{};
                entry.offset = 0;
                entry.firstCluster = m_rootCluster;
                entry.bytes = 0;
                entry.directory = true;
                entry.nameBytes = 1;
                entry.name[0] = '/';
                entry.name[1] = '\0';
                return Storage::Error::Ok;
            }
            if (object == 0) return Storage::Error::InvalidArgument;
            return ReadEntry(object - 1, entry);
        }

        // 初始化遍历状态：目录对象给出起始簇
        Storage::Error Fat32Volume::BeginWalk(uint64_t object, WalkState& state) {
            state = WalkState{};
            Entry entry{};
            if (const Storage::Error error = EntryForObject(object, entry); error != Storage::Error::Ok) return error;
            if (!entry.directory) return Storage::Error::NotDirectory;
            state.cluster = entry.firstCluster;
            if (state.cluster < 2 || state.cluster >= m_clusterCount + 2) return Storage::Error::Corrupt;
            return Storage::Error::Ok;
        }

        // 取下一个可用目录项：跳过删除项、卷标与 LFN 片段；LFN 与短项校验和相符才采用
        Storage::Error Fat32Volume::NextEntry(WalkState& state, Entry& entry, bool& ok) {
            ok = false;
            for (;;) {
                if (state.end) return Storage::Error::Ok;
                if (state.position >= m_clusterBytes) {
                    // 当前簇读完：沿 FAT 链推进
                    uint32_t next = 0;
                    bool end = false;
                    if (const Storage::Error error = NextCluster(state.cluster, next, end); error != Storage::Error::Ok) return error;
                    if (end) {
                        state.end = true;
                        return Storage::Error::Ok;
                    }
                    state.cluster = next;
                    state.position = 0;
                }
                // 每次调用都重读当前簇：调用方可能在两次遍历调用之间读了文件，簇缓冲会被复用
                if (const Storage::Error error = ReadCluster(state.cluster, m_cluster); error != Storage::Error::Ok) return error;
                const uint8_t* const raw = m_cluster + state.position;
                const uint64_t entryOffset = ClusterByteOffset(state.cluster) + state.position;
                state.position += 32;
                if (raw[0] == 0x00) {
                    state.end = true;   // 目录结束标记
                    return Storage::Error::Ok;
                }
                if (raw[0] == 0xE5) {
                    state.lfnSeen = false;   // 已删除的目录项
                    continue;
                }
                const uint8_t attr = raw[kEntryAttr];
                if (attr == kAttrLongName) {
                    // LFN 片段：序号、校验和与 13 个 UTF-16 码元
                    const uint32_t ordinal = raw[0] & 0x1F;
                    if (ordinal == 0 || ordinal > 20) return Storage::Error::Corrupt;
                    if ((raw[0] & 0x40) != 0) state.lfnLastOrdinal = ordinal;
                    state.lfnChecksum = raw[13];
                    state.lfnSeen = true;
                    static const uint32_t kCharOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (uint32_t i = 0; i < 13; ++i) {
                        const uint32_t index = (ordinal - 1) * 13 + i;
                        if (index >= 260) return Storage::Error::Corrupt;
                        state.lfnChars[index] = Get16(raw + kCharOffsets[i]);
                    }
                    continue;
                }
                if ((attr & kAttrVolumeLabel) != 0) {
                    state.lfnSeen = false;   // 卷标项不是文件
                    continue;
                }
                Entry parsed{};
                if (const Storage::Error error = ReadEntry(entryOffset, parsed); error != Storage::Error::Ok) return error;
                if (state.lfnSeen && state.lfnChecksum == ShortNameChecksum(raw) && state.lfnLastOrdinal >= 1) {
                    // 只接受 ASCII 子集：出现非 ASCII 码元时退回 8.3 名，不猜字符集
                    uint32_t nameBytes = 0;
                    bool ascii = true;
                    for (uint32_t i = 0; i < state.lfnLastOrdinal * 13; ++i) {
                        const uint16_t unit = state.lfnChars[i];
                        if (unit == 0) break;
                        if (unit > 0x7F || nameBytes >= kMaxNameBytes) {
                            ascii = false;
                            break;
                        }
                        parsed.name[nameBytes++] = static_cast<char>(unit);
                    }
                    if (ascii && nameBytes != 0) {
                        parsed.name[nameBytes] = '\0';
                        parsed.nameBytes = nameBytes;
                    }
                }
                state.lfnSeen = false;
                entry = parsed;
                ok = true;
                return Storage::Error::Ok;
            }
        }

        Storage::Error Fat32Volume::StatObject(uint64_t object, Storage::FileInfo& info) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (object == kRootObject) {
                info.id = object;
                info.directory = true;
                info.bytes = 0;
                return Storage::Error::Ok;
            }
            if (object == 0) return Storage::Error::InvalidArgument;
            Entry entry{};
            if (const Storage::Error error = EntryForObject(object, entry); error != Storage::Error::Ok) return error;
            info.id = object;
            info.directory = entry.directory;
            info.bytes = entry.directory ? 0 : entry.bytes;
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::Stat(const char* path, Storage::FileInfo& info) {
            uint64_t object = 0;
            if (const Storage::Error error = ResolvePath(path, object); error != Storage::Error::Ok) return error;
            return StatObject(object, info);
        }

        Storage::Error Fat32Volume::Open(const char* path, uint64_t& object) {
            return ResolvePath(path, object);
        }

        Storage::Error Fat32Volume::ResolvePath(const char* path, uint64_t& object) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (path == nullptr || path[0] != '/') return Storage::Error::InvalidArgument;
            uint32_t length = 0;
            while (path[length] != '\0') {
                if (length + 1 > Storage::PathRules::kMaxBytes) return Storage::Error::NameTooLong;
                ++length;
            }
            if (length > 1 && path[length - 1] == '/') return Storage::Error::InvalidArgument;
            uint64_t current = kRootObject;
            uint32_t position = 1;
            while (position < length) {
                const uint32_t nameBegin = position;
                while (position < length && path[position] != '/') ++position;
                const uint32_t nameBytes = position - nameBegin;
                if (nameBytes == 0) return Storage::Error::InvalidArgument;
                if (nameBytes > Storage::PathRules::kMaxNameBytes) return Storage::Error::NameTooLong;
                if (nameBytes == 1 && path[nameBegin] == '.') return Storage::Error::InvalidArgument;
                if (nameBytes == 2 && path[nameBegin] == '.' && path[nameBegin + 1] == '.') return Storage::Error::InvalidArgument;
                uint64_t child = 0;
                if (const Storage::Error error = FindChild(current, path + nameBegin, nameBytes, child); error != Storage::Error::Ok) return error;
                current = child;
                if (position < length) ++position;
            }
            object = current;
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::FindChild(uint64_t directoryObject, const char* name, uint32_t nameBytes, uint64_t& child) {
            WalkState state{};
            if (const Storage::Error error = BeginWalk(directoryObject, state); error != Storage::Error::Ok) return error;
            for (;;) {
                bool ok = false;
                Entry entry{};
                if (const Storage::Error error = NextEntry(state, entry, ok); error != Storage::Error::Ok) return error;
                if (!ok) return Storage::Error::NotFound;
                if (entry.directory && entry.nameBytes == 1 && entry.name[0] == '.') continue;
                if (entry.directory && entry.nameBytes == 2 && entry.name[0] == '.' && entry.name[1] == '.') continue;
                // 两种命中形式：显示名（LFN 优先）与 8.3 短名，都按 ASCII 折叠比较
                if (NameSameFold(entry.name, entry.nameBytes, name, nameBytes)
                    || NameSameFold(entry.shortName, entry.shortNameBytes, name, nameBytes)) {
                    child = entry.offset + 1;
                    return Storage::Error::Ok;
                }
            }
        }

        Storage::Error Fat32Volume::Read(uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) {
            read = 0;
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            if (object == kRootObject) return Storage::Error::IsDirectory;
            if (object == 0) return Storage::Error::InvalidArgument;
            Entry entry{};
            if (const Storage::Error error = ReadEntry(object - 1, entry); error != Storage::Error::Ok) return error;
            if (entry.directory) return Storage::Error::IsDirectory;
            if (offset >= entry.bytes) return Storage::Error::Ok;   // 文件尾之后读到 0
            uint64_t available = entry.bytes - offset;
            uint32_t want = bytes;
            if (available < want) want = static_cast<uint32_t>(available);
            uint32_t cluster = entry.firstCluster;
            const uint32_t skipClusters = static_cast<uint32_t>(offset / m_clusterBytes);
            uint32_t inside = static_cast<uint32_t>(offset % m_clusterBytes);
            if (const Storage::Error error = SeekCluster(cluster, skipClusters, cluster); error != Storage::Error::Ok) return error;
            uint8_t* dest = static_cast<uint8_t*>(buffer);
            uint32_t done = 0;
            uint32_t steps = 0;
            while (done < want) {
                if (const Storage::Error error = ReadCluster(cluster, m_cluster); error != Storage::Error::Ok) return error;
                const uint32_t chunk = (m_clusterBytes - inside) < (want - done) ? (m_clusterBytes - inside) : (want - done);
                memcpy(dest + done, m_cluster + inside, chunk);
                done += chunk;
                inside = 0;
                if (done >= want) break;
                // 链步数上界：自环或异常长的链按结构非法拒绝，不做无界循环
                if (++steps > m_clusterCount) return Storage::Error::Corrupt;
                uint32_t next = 0;
                bool end = false;
                if (const Storage::Error error = NextCluster(cluster, next, end); error != Storage::Error::Ok) return error;
                if (end) return Storage::Error::Corrupt;   // 链比声明的长度短
                cluster = next;
            }
            read = done;
            return Storage::Error::Ok;
        }

        Storage::Error Fat32Volume::ReadDirectory(uint64_t object, uint64_t& cursor, char* name, uint32_t nameBytes, uint32_t& nameLength, Storage::FileInfo& info) {
            nameLength = 0;
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (name == nullptr || nameBytes == 0) return Storage::Error::InvalidArgument;
            WalkState state{};
            if (const Storage::Error error = BeginWalk(object, state); error != Storage::Error::Ok) return error;
            for (;;) {
                bool ok = false;
                Entry entry{};
                if (const Storage::Error error = NextEntry(state, entry, ok); error != Storage::Error::Ok) return error;
                if (!ok) return Storage::Error::NotFound;
                if (entry.offset + 32 <= cursor) continue;   // 已交出的目录项：跳过
                if (entry.directory && entry.nameBytes == 1 && entry.name[0] == '.') {
                    cursor = entry.offset + 32;
                    continue;
                }
                if (entry.directory && entry.nameBytes == 2 && entry.name[0] == '.' && entry.name[1] == '.') {
                    cursor = entry.offset + 32;
                    continue;
                }
                if (entry.nameBytes + 1 > nameBytes) return Storage::Error::NameTooLong;   // 不推进游标，换大缓冲可重试
                for (uint32_t i = 0; i < entry.nameBytes; ++i) name[i] = entry.name[i];
                name[entry.nameBytes] = '\0';
                nameLength = entry.nameBytes;
                info.id = entry.offset + 1;
                info.directory = entry.directory;
                info.bytes = entry.directory ? 0 : entry.bytes;
                cursor = entry.offset + 32;
                return Storage::Error::Ok;
            }
        }
    }
}
