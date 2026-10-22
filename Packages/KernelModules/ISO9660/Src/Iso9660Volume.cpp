/* Iso9660Volume.cpp
    ISO9660 只读卷的实现：主卷描述符、目录记录、SUSP/Rock Ridge 与文件读取

    依据 ECMA-119 与 RRIP_1991A；只读、不写卷、不分配内存，缓冲都是实例内固定数组
    卷内标识符是 ISO 形式（文件记录带 ";1" 版本），真实名字由 Rock Ridge NM 给出；
    名字比较统一按 ASCII 折叠，比较前去掉版本后缀，两种形式都能命中
    未支持项遇到即拒绝：多区段文件（flags bit7）、符号链接（SL）、关联/保护属性
*/

#include <Iso9660Volume.hpp>

#include <Libc.hpp>

namespace KernelModules {
    namespace Iso9660 {
        namespace {
            // 目录记录内偏移（ECMA-119 目录记录布局）
            constexpr uint32_t kRecLen = 0x00;            // 记录长度
            constexpr uint32_t kRecExtAttrBytes = 0x01;   // 扩展属性记录长度
            constexpr uint32_t kRecExtent = 0x02;         // 数据起始逻辑块（双端序 32）
            constexpr uint32_t kRecDataBytes = 0x0A;      // 数据字节数（双端序 32）
            constexpr uint32_t kRecFlags = 0x19;          // 文件标志
            constexpr uint32_t kRecIdBytes = 0x20;        // 标识符长度
            constexpr uint32_t kRecId = 0x21;             // 标识符
            // 文件标志位
            constexpr uint8_t kFlagDirectory = 0x02;      // 目录
            constexpr uint8_t kFlagMultiExtent = 0x80;    // 多区段
            // 主卷描述符内偏移
            constexpr uint32_t kPvdVolumeId = 0x28;       // Volume Identifier，32 字节
            constexpr uint32_t kPvdVolumeBlocks = 0x50;   // Volume Space Size（双端序 32）
            constexpr uint32_t kPvdLogicalBlock = 0x80;   // Logical Block Size（双端序 16）
            // Rock Ridge 的 PX 权限位：目录与符号链接的类型位
            constexpr uint32_t kModeDirectory = 0040000u;
            constexpr uint32_t kModeSymlink = 0120000u;

            // 按小端读 16 位
            uint16_t Get16LE(const uint8_t* p) {
                return static_cast<uint16_t>(p[0] | (p[1] << 8));
            }

            // 按大端读 16 位
            uint16_t Get16BE(const uint8_t* p) {
                return static_cast<uint16_t>((p[0] << 8) | p[1]);
            }

            // 按小端读 32 位
            uint32_t Get32LE(const uint8_t* p) {
                uint32_t value = 0;
                for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (8 * i);
                return value;
            }

            // 按大端读 32 位
            uint32_t Get32BE(const uint8_t* p) {
                uint32_t value = 0;
                for (uint32_t i = 0; i < 4; ++i) value = (value << 8) | p[i];
                return value;
            }

            // ASCII 折叠成小写；非 ASCII 字节原样保留
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

            // 去掉 ISO 文件标识符的 ";<版本>" 后缀：返回剩余字节数
            uint32_t StripVersion(const char* name, uint32_t bytes) {
                if (bytes < 2 || name[bytes - 2] != ';') return bytes;
                const char last = name[bytes - 1];
                if (last < '0' || last > '9') return bytes;
                return bytes - 2;
            }

            // 逻辑块号与字节偏移的换算（块大小固定 2048，用移位）
            constexpr uint32_t kBlockShift = 11;
        }

        const char* Iso9660Volume::Format() const {
            return "iso9660";
        }

        Storage::FileSystemCaps Iso9660Volume::Caps() const {
            Storage::FileSystemCaps caps;
            caps.writable = false;
            caps.directoryListing = true;
            caps.symlinks = false;
            caps.nameCase = Storage::NameCase::Insensitive;
            return caps;
        }

        uint64_t Iso9660Volume::RootObject() const {
            return m_mounted ? m_rootObject : 0;
        }

        void Iso9660Volume::Unmount() {
            m_device = IsoDevice{};
            m_mounted = false;
            m_volumeBlocks = 0;
            m_rootObject = 0;
            for (uint32_t i = 0; i < kVolumeIdBytes; ++i) m_volumeId[i] = 0;
        }

        Storage::Error Iso9660Volume::ReadBytes(uint64_t byteOffset, void* buffer, uint32_t bytes) {
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

        Storage::Error Iso9660Volume::ReadBlock(uint32_t lba, void* buffer) {
            if (lba >= m_volumeBlocks) return Storage::Error::Corrupt;
            return ReadBytes(static_cast<uint64_t>(lba) << kBlockShift, buffer, kBlockBytes);
        }

        Storage::Error Iso9660Volume::Mount(const IsoDevice& device) {
            Unmount();
            const uint32_t unit = device.blockBytes;
            if (device.Read == nullptr || unit == 0 || (unit & (unit - 1)) != 0 || unit > sizeof(m_unit)) return Storage::Error::InvalidArgument;
            if (device.capacityBytes < (static_cast<uint64_t>(kPvdLba) + 1) * kBlockBytes) return Storage::Error::InvalidArgument;
            m_device = device;

            // 主卷描述符在逻辑块 16：类型 1、标准标识 CD001、版本 1
            if (const Storage::Error error = ReadBytes(static_cast<uint64_t>(kPvdLba) << kBlockShift, m_block, kBlockBytes); error != Storage::Error::Ok) {
                Unmount();
                return error;
            }
            if (m_block[0] != 1 || m_block[1] != 'C' || m_block[2] != 'D' || m_block[3] != '0' || m_block[4] != '0' || m_block[5] != '1' || m_block[6] != 1) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            // 逻辑块大小必须是 2048：本实现按固定 2048 解析，其它值明确拒绝而不是按 512/1024 猜
            if (Get16LE(m_block + kPvdLogicalBlock) != kBlockBytes || Get16BE(m_block + kPvdLogicalBlock + 2) != kBlockBytes) {
                Unmount();
                return Storage::Error::NotSupported;
            }
            // 双端序字段两侧必须一致：不一致说明结构被破坏
            m_volumeBlocks = Get32LE(m_block + kPvdVolumeBlocks);
            if (Get32BE(m_block + kPvdVolumeBlocks + 4) != m_volumeBlocks) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            if (m_volumeBlocks == 0) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            for (uint32_t i = 0; i < kVolumeIdBytes; ++i) m_volumeId[i] = m_block[kPvdVolumeId + i];
            m_rootObject = (static_cast<uint64_t>(kPvdLba) << kBlockShift) + kPvdRootRecord;
            // 根目录记录必须自洽：目录标志、长度覆盖、extent 在卷内
            Record root{};
            if (const Storage::Error error = ParseRecord(m_block, kPvdRootRecord, m_rootObject, root); error != Storage::Error::Ok) {
                Unmount();
                return error;
            }
            if (!root.directory || root.extentLba == 0 || root.extentLba >= m_volumeBlocks) {
                Unmount();
                return Storage::Error::Corrupt;
            }
            m_mounted = true;
            return Storage::Error::Ok;
        }

        Storage::Error Iso9660Volume::VolumeIdentity(uint8_t volumeId[16]) const {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (volumeId == nullptr) return Storage::Error::InvalidArgument;
            // 卷标识是 32 字节空格填充文本：去掉尾部空格/NUL，取前 16 字节，其余补零
            uint32_t bytes = kVolumeIdBytes;
            while (bytes > 0 && (m_volumeId[bytes - 1] == ' ' || m_volumeId[bytes - 1] == 0)) --bytes;
            if (bytes == 0) return Storage::Error::NotSupported;
            for (uint32_t i = 0; i < 16; ++i) volumeId[i] = i < bytes ? m_volumeId[i] : 0;
            return Storage::Error::Ok;
        }

        Storage::Error Iso9660Volume::ParseSystemUse(const uint8_t* systemUse, uint32_t bytes, Record& record, bool allowContinuation) {
            uint32_t position = 0;
            while (position + 4 <= bytes) {
                const uint8_t* const entry = systemUse + position;
                const uint32_t length = entry[2];
                if (length < 4 || position + length > bytes) return Storage::Error::Corrupt;
                const uint8_t* const data = entry + 4;
                const uint32_t dataBytes = length - 4;
                if (entry[0] == 'R' && entry[1] == 'R' && dataBytes >= 1) {
                    // RR 标志声明本记录带哪些 RRIP 项；读取方按它决定是否采用 NM 等项
                    const uint8_t flags = data[0];
                    if ((flags & 0x01) != 0 && dataBytes >= 4) {
                        // PX：mode 的低 16 位给权限与类型
                        const uint32_t mode = Get32LE(data) & 0xFFFFu;
                        if ((mode & kModeDirectory) != 0) record.directory = true;
                        if ((mode & kModeSymlink) != 0) record.symlink = true;
                    }
                } else if (entry[0] == 'N' && entry[1] == 'M' && dataBytes >= 1) {
                    // NM：flags + 名字片段；bit0 表示后续还有片段，bit1/bit2 是 "." 与 ".."
                    const uint8_t flags = data[0];
                    if ((flags & 0x02) != 0) {
                        record.nameBytes = 1;
                        record.name[0] = '.';
                        record.name[1] = '\0';
                    } else if ((flags & 0x04) != 0) {
                        record.nameBytes = 2;
                        record.name[0] = '.';
                        record.name[1] = '.';
                        record.name[2] = '\0';
                    } else {
                        // NM 取代 ISO 标识符：第一个片段先把标识符名字清掉，续接片段继续追加
                        if (!record.nmSeen) {
                            record.nameBytes = 0;
                            record.nmSeen = true;
                        }
                        for (uint32_t i = 1; i < dataBytes; ++i) {
                            if (record.nameBytes >= kMaxNameBytes) return Storage::Error::Corrupt;
                            record.name[record.nameBytes++] = static_cast<char>(data[i]);
                        }
                        record.name[record.nameBytes] = '\0';
                    }
                } else if (entry[0] == 'S' && entry[1] == 'L') {
                    record.symlink = true;
                } else if (entry[0] == 'C' && entry[1] == 'E' && dataBytes >= 24 && allowContinuation) {
                    // CE：续接区（块号、块内偏移、长度，各 8 字节双端序）
                    const uint32_t block = Get32LE(data);
                    const uint32_t offset = Get32LE(data + 8);
                    const uint32_t length = Get32LE(data + 16);
                    if (length == 0 || length > sizeof(m_continuation)) return Storage::Error::Corrupt;
                    if (const Storage::Error error = ReadBytes((static_cast<uint64_t>(block) << kBlockShift) + offset, m_continuation, length); error != Storage::Error::Ok) return error;
                    if (const Storage::Error error = ParseSystemUse(m_continuation, length, record, false); error != Storage::Error::Ok) return error;
                }
                position += length;
            }
            return Storage::Error::Ok;
        }

        Storage::Error Iso9660Volume::ParseRecord(const uint8_t* block, uint32_t offsetInBlock, uint64_t recordOffset, Record& record) {
            if (offsetInBlock + 33 > kBlockBytes) return Storage::Error::Corrupt;
            const uint32_t length = block[offsetInBlock + kRecLen];
            if (length < 33 || offsetInBlock + length > kBlockBytes) return Storage::Error::Corrupt;
            if (block[offsetInBlock + kRecExtAttrBytes] != 0) return Storage::Error::NotSupported;   // 扩展属性记录不在范围内
            record = Record{};
            record.offset = recordOffset;
            record.extentLba = Get32LE(block + offsetInBlock + kRecExtent);
            if (Get32BE(block + offsetInBlock + kRecExtent + 4) != record.extentLba) return Storage::Error::Corrupt;
            record.dataBytes = Get32LE(block + offsetInBlock + kRecDataBytes);
            if (Get32BE(block + offsetInBlock + kRecDataBytes + 4) != record.dataBytes) return Storage::Error::Corrupt;
            const uint8_t flags = block[offsetInBlock + kRecFlags];
            record.directory = (flags & kFlagDirectory) != 0;
            record.multiExtent = (flags & kFlagMultiExtent) != 0;
            const uint32_t idBytes = block[offsetInBlock + kRecIdBytes];
            if (idBytes == 0 || 33 + idBytes > length) return Storage::Error::Corrupt;
            const uint8_t* const id = block + offsetInBlock + kRecId;
            if (idBytes == 1 && (id[0] == 0 || id[0] == 1)) {
                // 自指项：0x00 是 "."，0x01 是 ".."
                record.nameBytes = 1;
                record.name[0] = '.';
                if (id[0] == 1) {
                    record.nameBytes = 2;
                    record.name[1] = '.';
                }
                record.name[record.nameBytes] = '\0';
                record.idNameBytes = record.nameBytes;
                memcpy(record.idName, record.name, record.nameBytes + 1);
            } else {
                // ISO 标识符：先作为显示名与第二种命中形式（文件去掉 ";1" 版本后缀）
                for (uint32_t i = 0; i < idBytes && i < kMaxNameBytes; ++i) record.name[record.nameBytes++] = static_cast<char>(id[i]);
                record.name[record.nameBytes] = '\0';
                if (!record.directory) record.nameBytes = StripVersion(record.name, record.nameBytes);
                record.name[record.nameBytes] = '\0';
                record.idNameBytes = record.nameBytes;
                memcpy(record.idName, record.name, record.nameBytes + 1);
            }
            // 系统用区：标识符之后按需补一个填充字节（标识符长度为偶数时）
            const uint32_t systemUseOffset = 33 + idBytes + ((idBytes % 2 == 0) ? 1 : 0);
            if (systemUseOffset < length) {
                if (const Storage::Error error = ParseSystemUse(block + offsetInBlock + systemUseOffset, length - systemUseOffset, record, true); error != Storage::Error::Ok) return error;
            }
            return Storage::Error::Ok;
        }

        Storage::Error Iso9660Volume::ReadRecord(uint64_t offset, Record& record) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            const uint32_t lba = static_cast<uint32_t>(offset >> kBlockShift);
            const uint32_t inside = static_cast<uint32_t>(offset & (kBlockBytes - 1));
            if (const Storage::Error error = ReadBlock(lba, m_block); error != Storage::Error::Ok) return error;
            return ParseRecord(m_block, inside, offset, record);
        }

        Storage::Error Iso9660Volume::StatObject(uint64_t object, Storage::FileInfo& info) {
            Record record{};
            if (const Storage::Error error = ReadRecord(object, record); error != Storage::Error::Ok) return error;
            if (record.symlink) return Storage::Error::NotSupported;   // 不跟随符号链接
            if (record.multiExtent) return Storage::Error::NotSupported;
            info.id = object;
            info.directory = record.directory;
            info.bytes = record.directory ? 0 : record.dataBytes;
            return Storage::Error::Ok;
        }

        Storage::Error Iso9660Volume::Stat(const char* path, Storage::FileInfo& info) {
            uint64_t object = 0;
            if (const Storage::Error error = ResolvePath(path, object); error != Storage::Error::Ok) return error;
            return StatObject(object, info);
        }

        Storage::Error Iso9660Volume::Open(const char* path, uint64_t& object) {
            return ResolvePath(path, object);
        }

        Storage::Error Iso9660Volume::ResolvePath(const char* path, uint64_t& object) {
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (path == nullptr || path[0] != '/') return Storage::Error::InvalidArgument;
            uint32_t length = 0;
            while (path[length] != '\0') {
                if (length + 1 > Storage::PathRules::kMaxBytes) return Storage::Error::NameTooLong;
                ++length;
            }
            if (length > 1 && path[length - 1] == '/') return Storage::Error::InvalidArgument;
            uint64_t current = m_rootObject;
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

        Storage::Error Iso9660Volume::FindChild(uint64_t directoryObject, const char* name, uint32_t nameBytes, uint64_t& child) {
            Record directory{};
            if (const Storage::Error error = ReadRecord(directoryObject, directory); error != Storage::Error::Ok) return error;
            if (!directory.directory) return Storage::Error::NotDirectory;
            const uint32_t queryBytes = StripVersion(name, nameBytes);
            uint32_t cursor = 0;
            while (cursor < directory.dataBytes) {
                const uint32_t blockIndex = cursor >> kBlockShift;
                const uint32_t inBlock = cursor & (kBlockBytes - 1);
                const uint32_t lba = directory.extentLba + blockIndex;
                if (const Storage::Error error = ReadBlock(lba, m_block); error != Storage::Error::Ok) return error;
                const uint64_t blockOffset = static_cast<uint64_t>(lba) << kBlockShift;
                uint32_t position = inBlock;
                while (position < kBlockBytes) {
                    const uint32_t length = m_block[position + kRecLen];
                    if (length == 0) break;   // 本块剩余部分是填充
                    Record record{};
                    if (const Storage::Error error = ParseRecord(m_block, position, blockOffset + position, record); error != Storage::Error::Ok) return error;
                    // 两种命中形式：Rock Ridge 显示名（NM）与 ISO 标识符（去版本后缀），都按 ASCII 折叠比较
                    const uint32_t recordNameBytes = StripVersion(record.name, record.nameBytes);
                    const uint32_t recordIdBytes = StripVersion(record.idName, record.idNameBytes);
                    if (NameSameFold(record.name, recordNameBytes, name, queryBytes)
                        || NameSameFold(record.idName, recordIdBytes, name, queryBytes)) {
                        if (record.symlink) return Storage::Error::NotSupported;
                        if (record.multiExtent) return Storage::Error::NotSupported;
                        child = record.offset;
                        return Storage::Error::Ok;
                    }
                    position += length;
                }
                cursor = (blockIndex + 1) << kBlockShift;
                if (cursor > directory.dataBytes) break;
            }
            return Storage::Error::NotFound;
        }

        Storage::Error Iso9660Volume::Read(uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) {
            read = 0;
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            Record record{};
            if (const Storage::Error error = ReadRecord(object, record); error != Storage::Error::Ok) return error;
            if (record.directory) return Storage::Error::IsDirectory;
            if (record.symlink || record.multiExtent) return Storage::Error::NotSupported;
            if (offset >= record.dataBytes) return Storage::Error::Ok;   // 文件尾之后读到 0
            uint64_t available = record.dataBytes - offset;
            uint32_t want = bytes;
            if (available < want) want = static_cast<uint32_t>(available);
            const uint64_t dataOffset = (static_cast<uint64_t>(record.extentLba) << kBlockShift) + offset;
            if (const Storage::Error error = ReadBytes(dataOffset, buffer, want); error != Storage::Error::Ok) return error;
            read = want;
            return Storage::Error::Ok;
        }

        Storage::Error Iso9660Volume::ReadDirectory(uint64_t object, uint64_t& cursor, char* name, uint32_t nameBytes, uint32_t& nameLength, Storage::FileInfo& info) {
            nameLength = 0;
            if (!m_mounted) return Storage::Error::InvalidArgument;
            if (name == nullptr || nameBytes == 0) return Storage::Error::InvalidArgument;
            Record directory{};
            if (const Storage::Error error = ReadRecord(object, directory); error != Storage::Error::Ok) return error;
            if (!directory.directory) return Storage::Error::NotDirectory;
            while (cursor < directory.dataBytes) {
                const uint32_t blockIndex = static_cast<uint32_t>(cursor >> kBlockShift);
                const uint32_t inBlock = static_cast<uint32_t>(cursor & (kBlockBytes - 1));
                const uint32_t lba = directory.extentLba + blockIndex;
                if (const Storage::Error error = ReadBlock(lba, m_block); error != Storage::Error::Ok) return error;
                const uint64_t blockOffset = static_cast<uint64_t>(lba) << kBlockShift;
                uint32_t position = inBlock;
                while (position < kBlockBytes) {
                    const uint32_t length = m_block[position + kRecLen];
                    if (length == 0) break;   // 本块剩余部分是填充：推进到下一块
                    Record record{};
                    if (const Storage::Error error = ParseRecord(m_block, position, blockOffset + position, record); error != Storage::Error::Ok) return error;
                    const bool self = record.nameBytes == 1 && record.name[0] == '.';
                    const bool parent = record.nameBytes == 2 && record.name[0] == '.' && record.name[1] == '.';
                    if (!self && !parent) {
                        if (record.nameBytes + 1 > nameBytes) return Storage::Error::NameTooLong;   // 不推进游标，换大缓冲可重试
                        for (uint32_t i = 0; i < record.nameBytes; ++i) name[i] = record.name[i];
                        name[record.nameBytes] = '\0';
                        Storage::FileInfo child{};
                        child.id = record.offset;
                        child.directory = record.directory;
                        child.bytes = record.directory ? 0 : record.dataBytes;
                        nameLength = record.nameBytes;
                        info = child;
                        cursor = (blockOffset + position + length) - (static_cast<uint64_t>(directory.extentLba) << kBlockShift);
                        return Storage::Error::Ok;
                    }
                    position += length;
                }
                cursor = static_cast<uint64_t>(blockIndex + 1) << kBlockShift;
                if (cursor > directory.dataBytes) break;
            }
            return Storage::Error::NotFound;
        }
    }
}
