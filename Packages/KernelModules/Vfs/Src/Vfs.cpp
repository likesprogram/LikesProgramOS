/* Vfs.cpp
    Fs.Vfs v1 的实现：文件系统登记、挂载表、统一路径解析、文件引用与目录遍历

    路径按共用 Storage::Path 语法校验与切分；挂载点裁剪由本层负责，交给 FileSystemOps
    的是卷内绝对路径；当前公共入口
    只接受挂载点 "/" 与只读模式（多挂载点属后续版本），内部按最长挂载点匹配解析
    文件引用是 VFS 自有的不透明 u64（表下标 + 1，0 非法）；错误码原样透传 Storage::Error，
    不另造错误码、不泄漏 KernelError；固定容量表耗尽报 NoSpace
*/

#include <VfsCore.hpp>

namespace {
    // 文件系统登记项：ops 与实例 context
    struct FileSystemEntry {
        bool used;                  // 是否占用
        FileSystemId id;            // 登记标识，非零
        const FileSystemOps* ops;   // 调用表
        void* context;              // 实例上下文
    };

    // 挂载表项：挂载点、文件系统与设备
    struct MountEntry {
        bool used;                                 // 是否占用
        FileSystemId fsId;                         // 文件系统登记标识
        DeviceHandle device;                       // 被挂载的块设备句柄
        uint32_t mode;                             // 挂载模式
        uint32_t mountPointBytes;                  // 挂载点字节数
        char mountPoint[VfsModule::kMaxPathBytes + 1]; // 挂载点绝对路径，NUL 结尾
    };

    // 文件引用表项
    struct FileEntry {
        bool used;              // 是否占用
        uint32_t mountIndex;    // 所属挂载表下标
        uint64_t object;        // 文件系统内部对象标识
    };

    // 文件系统登记表、挂载表与文件引用表：固定上界
    FileSystemEntry g_fileSystems[VfsModule::kMaxFileSystems];
    MountEntry g_mounts[VfsModule::kMaxMounts];
    FileEntry g_files[VfsModule::kMaxFiles];
    // 下一个文件系统登记标识：从 1 起，0 表示无效
    FileSystemId g_nextFileSystemId = 1;

    // 计算以 NUL 结尾文本的字节数（不含 NUL）
    uint32_t TextLength(const char* text) {
        uint32_t length = 0;
        while (text[length] != '\0') ++length;
        return length;
    }

    // 比较两条以 NUL 结尾文本是否相等
    bool TextEquals(const char* left, const char* right) {
        if (left == nullptr || right == nullptr) return false;
        uint32_t index = 0;
        while (left[index] != '\0' && right[index] != '\0') {
            if (left[index] != right[index]) return false;
            ++index;
        }
        return left[index] == right[index];
    }

    // 把一段文本复制进定长缓冲并补 NUL；超限返回 NameTooLong
    Storage::Error CopyText(char* target, uint32_t targetBytes, const char* source, uint32_t sourceBytes) {
        if (sourceBytes >= targetBytes) return Storage::Error::NameTooLong;
        for (uint32_t index = 0; index < sourceBytes; ++index) target[index] = source[index];
        target[sourceBytes] = '\0';
        return Storage::Error::Ok;
    }

    // 查找文件系统登记项；未登记返回空
    FileSystemEntry* FindFileSystem(FileSystemId id) {
        if (id == 0) return nullptr;
        for (uint32_t index = 0; index < VfsModule::kMaxFileSystems; ++index) {
            if (g_fileSystems[index].used && g_fileSystems[index].id == id) return &g_fileSystems[index];
        }
        return nullptr;
    }

    // 查找挂载表中的空闲项；表满返回空
    MountEntry* AllocateMount() {
        for (uint32_t index = 0; index < VfsModule::kMaxMounts; ++index) {
            if (!g_mounts[index].used) return &g_mounts[index];
        }
        return nullptr;
    }

    // 按挂载点字节查找挂载表下标；找不到返回 kMaxMounts
    uint32_t FindMountIndex(const char* mountPoint, uint32_t mountPointBytes) {
        for (uint32_t index = 0; index < VfsModule::kMaxMounts; ++index) {
            const MountEntry& mount = g_mounts[index];
            if (!mount.used || mount.mountPointBytes != mountPointBytes) continue;
            bool same = true;
            for (uint32_t byte = 0; byte < mountPointBytes; ++byte) {
                if (mount.mountPoint[byte] != mountPoint[byte]) {
                    same = false;
                    break;
                }
            }
            if (same) return index;
        }
        return VfsModule::kMaxMounts;
    }

    // 文件引用到表项的换算：引用 0 与越界都非法
    FileEntry* FileFromReference(uint64_t reference) {
        if (reference == 0 || reference > VfsModule::kMaxFiles) return nullptr;
        FileEntry& entry = g_files[reference - 1];
        return entry.used ? &entry : nullptr;
    }

    // 判断挂载点是否为路径的前缀（分量边界匹配，不是纯字符串前缀）
    bool MountPointMatches(const MountEntry& mount, const char* path, uint32_t pathBytes) {
        if (mount.mountPointBytes > pathBytes) return false;
        if (mount.mountPointBytes == 1 && mount.mountPoint[0] == '/') return pathBytes > 0 && path[0] == '/';
        for (uint32_t byte = 0; byte < mount.mountPointBytes; ++byte) {
            if (mount.mountPoint[byte] != path[byte]) return false;
        }
        if (mount.mountPointBytes == pathBytes) return true;
        return path[mount.mountPointBytes] == '/';
    }
}

namespace VfsModule {
    void ResetVfsTables() {
        for (uint32_t index = 0; index < kMaxFileSystems; ++index) g_fileSystems[index] = FileSystemEntry{};
        for (uint32_t index = 0; index < kMaxMounts; ++index) g_mounts[index] = MountEntry{};
        for (uint32_t index = 0; index < kMaxFiles; ++index) g_files[index] = FileEntry{};
        g_nextFileSystemId = 1;
    }

    Storage::Error RegisterFileSystem(const FileSystemOps* ops, void* context, FileSystemId* id) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (ops == nullptr || id == nullptr) return Storage::Error::InvalidArgument;
        // 必需槽缺项即拒绝登记：默认行为实现由模块负责，VFS 不提供缺省回调
        if (ops->Format == nullptr || ops->Mount == nullptr || ops->Unmount == nullptr || ops->Caps == nullptr
            || ops->VolumeIdentity == nullptr || ops->Stat == nullptr || ops->Open == nullptr
            || ops->Read == nullptr || ops->ReadDirectory == nullptr) {
            return Storage::Error::InvalidArgument;
        }
        for (uint32_t index = 0; index < kMaxFileSystems; ++index) {
            if (g_fileSystems[index].used) continue;
            FileSystemId newId = g_nextFileSystemId++;
            if (newId == 0) newId = g_nextFileSystemId++;
            g_fileSystems[index].used = true;
            g_fileSystems[index].id = newId;
            g_fileSystems[index].ops = ops;
            g_fileSystems[index].context = context;
            *id = newId;
            return Storage::Error::Ok;
        }
        return Storage::Error::NoSpace;
    }

    bool FindFileSystemByFormat(const char* format, FileSystemId* id) {
        if (format == nullptr || id == nullptr) return false;
        for (uint32_t index = 0; index < kMaxFileSystems; ++index) {
            const FileSystemEntry& entry = g_fileSystems[index];
            if (!entry.used || entry.ops == nullptr || entry.ops->Format == nullptr) continue;
            const char* const name = entry.ops->Format(entry.context);
            if (name != nullptr && TextEquals(name, format)) {
                *id = entry.id;
                return true;
            }
        }
        return false;
    }

    Storage::Error Mount(FileSystemId id, DeviceHandle device, const char* mountPoint, uint32_t mode) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (mountPoint == nullptr) return Storage::Error::InvalidArgument;
        const Storage::Error pathError = Storage::CheckPath(mountPoint);
        if (pathError != Storage::Error::Ok) return pathError;
        // v1 公共入口只接受根挂载点与只读模式；多挂载点与跨卷解析属后续版本
        if (!TextEquals(mountPoint, "/")) return Storage::Error::NotSupported;
        return MountAt(id, device, mountPoint, mode);
    }

    Storage::Error MountAt(FileSystemId id, DeviceHandle device, const char* mountPoint, uint32_t mode) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (mountPoint == nullptr) return Storage::Error::InvalidArgument;
        const Storage::Error pathError = Storage::CheckPath(mountPoint);
        if (pathError != Storage::Error::Ok) return pathError;
        if (mode != static_cast<uint32_t>(Storage::MountMode::ReadOnly)) return Storage::Error::NotSupported;
        FileSystemEntry* const fileSystem = FindFileSystem(id);
        if (fileSystem == nullptr) return Storage::Error::NotFound;
        if (!DeviceExists(device)) return Storage::Error::NotFound;
        const uint32_t mountPointBytes = TextLength(mountPoint);
        // 同一挂载点只能挂载一次；同一设备当前最多挂载一次
        if (FindMountIndex(mountPoint, mountPointBytes) < kMaxMounts) return Storage::Error::InvalidArgument;
        for (uint32_t index = 0; index < kMaxMounts; ++index) {
            if (g_mounts[index].used && g_mounts[index].device == device) return Storage::Error::InvalidArgument;
        }
        MountEntry* const mount = AllocateMount();
        if (mount == nullptr) return Storage::Error::NoSpace;
        // 先取设备引用，再把设备交给文件系统；失败时回退引用，不留下半挂载状态
        const BlockDeviceOps* deviceOps = nullptr;
        void* deviceContext = nullptr;
        Storage::Error error = Acquire(device, &deviceOps, &deviceContext);
        if (error != Storage::Error::Ok) return error;
        error = fileSystem->ops->Mount(fileSystem->context, deviceOps, deviceContext, mode);
        if (error != Storage::Error::Ok) {
            Release(device);
            return error;
        }
        mount->used = true;
        mount->fsId = fileSystem->id;
        mount->device = device;
        mount->mode = mode;
        mount->mountPointBytes = mountPointBytes;
        return CopyText(mount->mountPoint, static_cast<uint32_t>(sizeof(mount->mountPoint)), mountPoint, mountPointBytes);
    }

    Storage::Error Unmount(const char* mountPoint) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (mountPoint == nullptr) return Storage::Error::InvalidArgument;
        const Storage::Error pathError = Storage::CheckPath(mountPoint);
        if (pathError != Storage::Error::Ok) return pathError;
        const uint32_t mountPointBytes = TextLength(mountPoint);
        const uint32_t mountIndex = FindMountIndex(mountPoint, mountPointBytes);
        if (mountIndex >= kMaxMounts) return Storage::Error::NotFound;
        // 还有未关闭的文件引用时拒绝卸载并保持挂载
        for (uint32_t index = 0; index < kMaxFiles; ++index) {
            if (g_files[index].used && g_files[index].mountIndex == mountIndex) return Storage::Error::InvalidArgument;
        }
        MountEntry& mount = g_mounts[mountIndex];
        FileSystemEntry* const fileSystem = FindFileSystem(mount.fsId);
        if (fileSystem != nullptr) fileSystem->ops->Unmount(fileSystem->context);
        Release(mount.device);
        mount = MountEntry{};
        return Storage::Error::Ok;
    }

    Storage::Error ResolveMount(const char* path, uint32_t& mountIndex, char* volumePath, uint32_t volumePathBytes) {
        if (path == nullptr || volumePath == nullptr) return Storage::Error::InvalidArgument;
        const Storage::Error pathError = Storage::CheckPath(path);
        if (pathError != Storage::Error::Ok) return pathError;
        const uint32_t pathBytes = TextLength(path);
        // 最长挂载点匹配：分量边界相等才算命中，不按纯字符串前缀命中
        uint32_t bestIndex = kMaxMounts;
        uint32_t bestBytes = 0;
        for (uint32_t index = 0; index < kMaxMounts; ++index) {
            const MountEntry& mount = g_mounts[index];
            if (!mount.used) continue;
            if (!MountPointMatches(mount, path, pathBytes)) continue;
            if (mount.mountPointBytes < bestBytes) continue;
            bestIndex = index;
            bestBytes = mount.mountPointBytes;
        }
        if (bestIndex >= kMaxMounts) return Storage::Error::InvalidArgument;
        // 裁掉挂载点前缀后补 "/"，得到卷内绝对路径
        const MountEntry& mount = g_mounts[bestIndex];
        if (mount.mountPointBytes == 1 && mount.mountPoint[0] == '/') {
            mountIndex = bestIndex;
            return CopyText(volumePath, volumePathBytes, path, pathBytes);
        }
        const char* const remainder = path + mount.mountPointBytes;
        if (*remainder == '\0') {
            mountIndex = bestIndex;
            return CopyText(volumePath, volumePathBytes, "/", 1);
        }
        // remainder 以 '/' 起头：卷内路径就是 remainder
        mountIndex = bestIndex;
        return CopyText(volumePath, volumePathBytes, remainder, TextLength(remainder));
    }

    Storage::Error Stat(const char* path, Storage::FileInfo* info) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (info == nullptr) return Storage::Error::InvalidArgument;
        uint32_t mountIndex = kMaxMounts;
        char volumePath[kMaxPathBytes + 1];
        const Storage::Error error = ResolveMount(path, mountIndex, volumePath, static_cast<uint32_t>(sizeof(volumePath)));
        if (error != Storage::Error::Ok) return error;
        const MountEntry& mount = g_mounts[mountIndex];
        FileSystemEntry* const fileSystem = FindFileSystem(mount.fsId);
        if (fileSystem == nullptr) return Storage::Error::InvalidArgument;
        return fileSystem->ops->Stat(fileSystem->context, volumePath, info);
    }

    Storage::Error Open(const char* path, uint64_t* reference) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (reference == nullptr) return Storage::Error::InvalidArgument;
        uint32_t mountIndex = kMaxMounts;
        char volumePath[kMaxPathBytes + 1];
        const Storage::Error error = ResolveMount(path, mountIndex, volumePath, static_cast<uint32_t>(sizeof(volumePath)));
        if (error != Storage::Error::Ok) return error;
        const MountEntry& mount = g_mounts[mountIndex];
        FileSystemEntry* const fileSystem = FindFileSystem(mount.fsId);
        if (fileSystem == nullptr) return Storage::Error::InvalidArgument;
        uint32_t slot = kMaxFiles;
        for (uint32_t index = 0; index < kMaxFiles; ++index) {
            if (g_files[index].used) continue;
            slot = index;
            break;
        }
        if (slot >= kMaxFiles) return Storage::Error::NoSpace;
        uint64_t object = 0;
        const Storage::Error openError = fileSystem->ops->Open(fileSystem->context, volumePath, &object);
        if (openError != Storage::Error::Ok) return openError;
        g_files[slot].used = true;
        g_files[slot].mountIndex = mountIndex;
        g_files[slot].object = object;
        *reference = static_cast<uint64_t>(slot) + 1;
        return Storage::Error::Ok;
    }

    Storage::Error Read(uint64_t reference, uint64_t offset, void* buffer, uint32_t bytes, uint32_t* read) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (read == nullptr) return Storage::Error::InvalidArgument;
        *read = 0;
        if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
        FileEntry* const entry = FileFromReference(reference);
        if (entry == nullptr) return Storage::Error::InvalidArgument;
        const MountEntry& mount = g_mounts[entry->mountIndex];
        FileSystemEntry* const fileSystem = FindFileSystem(mount.fsId);
        if (fileSystem == nullptr) return Storage::Error::InvalidArgument;
        return fileSystem->ops->Read(fileSystem->context, entry->object, offset, buffer, bytes, read);
    }

    Storage::Error Close(uint64_t reference) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        FileEntry* const entry = FileFromReference(reference);
        if (entry == nullptr) return Storage::Error::InvalidArgument;
        *entry = FileEntry{};
        return Storage::Error::Ok;
    }

    Storage::Error ReadDirectory(uint64_t reference, uint64_t* cursor, char* name, uint32_t nameBytes,
        uint32_t* nameLength, Storage::FileInfo* info) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (cursor == nullptr || name == nullptr || nameLength == nullptr || info == nullptr) {
            return Storage::Error::InvalidArgument;
        }
        *nameLength = 0;
        FileEntry* const entry = FileFromReference(reference);
        if (entry == nullptr) return Storage::Error::InvalidArgument;
        const MountEntry& mount = g_mounts[entry->mountIndex];
        FileSystemEntry* const fileSystem = FindFileSystem(mount.fsId);
        if (fileSystem == nullptr) return Storage::Error::InvalidArgument;
        return fileSystem->ops->ReadDirectory(fileSystem->context, entry->object, cursor, name, nameBytes, nameLength, info);
    }

    Storage::Error VolumeIdentity(const char* mountPoint, uint8_t* volumeId) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (mountPoint == nullptr || volumeId == nullptr) return Storage::Error::InvalidArgument;
        const Storage::Error pathError = Storage::CheckPath(mountPoint);
        if (pathError != Storage::Error::Ok) return pathError;
        const uint32_t mountIndex = FindMountIndex(mountPoint, TextLength(mountPoint));
        if (mountIndex >= kMaxMounts) return Storage::Error::InvalidArgument;
        const MountEntry& mount = g_mounts[mountIndex];
        FileSystemEntry* const fileSystem = FindFileSystem(mount.fsId);
        if (fileSystem == nullptr) return Storage::Error::InvalidArgument;
        return fileSystem->ops->VolumeIdentity(fileSystem->context, volumeId);
    }

    uint32_t MountCount() {
        uint32_t count = 0;
        for (uint32_t index = 0; index < kMaxMounts; ++index) {
            if (g_mounts[index].used) ++count;
        }
        return count;
    }
}
