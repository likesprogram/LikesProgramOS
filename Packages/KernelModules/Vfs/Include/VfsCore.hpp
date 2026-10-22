/* VfsCore.hpp
    Vfs 模块内部实现声明：块设备注册表、VFS 挂载与文件引用、系统卷挂载

    仅供本模块源文件与宿主夹具 CheckVfs 使用，不是跨模块接口；跨模块面是生成头
    Slots.hpp 的三个槽位表（Block.Registry / Fs.Vfs / Boot.Volume）。返回码全部是
    Storage::Error（存储面），模块内部不另造错误码、不泄漏 KernelError
    容量全部固定上界：设备 32（含子设备）、文件系统 8、挂载 4、文件引用 32；
    不动态分配、不缓存、不睡眠
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/Path.hpp>
#include <Types.hpp>

namespace VfsModule {
    // 固定容量上限：耗尽分别按 NoSpace / InvalidArgument 报错
    constexpr uint32_t kMaxDevices = 32;       // 块设备登记上限（根设备与子设备合计）
    constexpr uint32_t kMaxFileSystems = 8;    // 文件系统登记上限
    constexpr uint32_t kMaxMounts = 4;         // 挂载表上限
    constexpr uint32_t kMaxFiles = 32;         // 文件引用上限
    // 子设备句柄分配基址：子设备与块设备共用 DeviceHandle 命名空间，子视图取高位段
    constexpr DeviceHandle kChildHandleBase = 0x80000000u;
    // BindingFacts.state 的约定：0 表示清除，1 为只绑定未启动，2 为已启动（ops/context 有效）
    // 值由 DriverLoader 写入、Dev.Model 持久化（与 DriverRegistry 的同名常量一致）
    constexpr uint32_t kBindingStateBound = 1;
    constexpr uint32_t kBindingStateStarted = 2;
    // 挂载点与卷内路径缓冲上限：与共用统一路径上限一致
    constexpr uint32_t kMaxPathBytes = Storage::PathRules::kMaxBytes;

    // 存储层错误是否成功
    constexpr bool Succeeded(Storage::Error error) { return error == Storage::Error::Ok; }

    // 判断一段字节是否全零
    inline bool IsZeroBytes(const uint8_t* bytes, uint32_t count) {
        for (uint32_t index = 0; index < count; ++index) {
            if (bytes[index] != 0) return false;
        }
        return true;
    }

    // 比较两段字节是否相等
    inline bool SameBytes(const uint8_t* left, const uint8_t* right, uint32_t count) {
        for (uint32_t index = 0; index < count; ++index) {
            if (left[index] != right[index]) return false;
        }
        return true;
    }

    // 复制一段字节
    inline void CopyBytes(uint8_t* target, const uint8_t* source, uint32_t count) {
        for (uint32_t index = 0; index < count; ++index) target[index] = source[index];
    }

    // 复位全部注册表与句柄表：Init 与 Abort 调用
    void Reset();
    // 子层复位：块设备注册表、VFS 表（Reset 聚合调用）
    void ResetBlockRegistry();
    void ResetVfsTables();

    // 模块是否已成功 Init（处理器槽可据此拒绝未初始化调用）
    bool Initialized();

    // 从 Dev.Model 的绑定事实登记块设备：遍历设备句柄，已绑定的经 Publish 登记，未绑定跳过
    // Init 在复位注册表之后调用；失败返回 KernelError 供 Init 上报；Abort 由 Reset 撤销
    KernelError RegisterDevicesFromBindings();

    // 按格式名查找已登记的文件系统：找不到返回 false
    bool FindFileSystemByFormat(const char* format, FileSystemId* id);

    // —— 块设备层（Block.Registry 槽的实现）——
    Storage::Error Publish(DeviceHandle handle, const BlockDeviceOps* ops, void* context, uint32_t flags);
    Storage::Error CreateSubDevice(DeviceHandle parent, uint64_t begin, uint64_t bytes, uint32_t flags, DeviceHandle* child);
    Storage::Error Acquire(DeviceHandle handle, const BlockDeviceOps** ops, void** context);
    Storage::Error Release(DeviceHandle handle);
    // 设备是否已登记
    bool DeviceExists(DeviceHandle handle);
    // 设备当前引用计数：未登记返回 0
    uint32_t ReferenceCount(DeviceHandle handle);
    // 已登记的根设备（非子设备）句柄快照，返回写入的条数
    uint32_t RootDevices(DeviceHandle* handles, uint32_t maxHandles);

    // —— VFS（Fs.Vfs 槽的实现）——
    Storage::Error RegisterFileSystem(const FileSystemOps* ops, void* context, FileSystemId* id);
    // 公共挂载入口：v1 只接受挂载点 "/" 与只读模式，其它返回 NotSupported
    Storage::Error Mount(FileSystemId id, DeviceHandle device, const char* mountPoint, uint32_t mode);
    // 通用挂载：不做 v1 的挂载点限制，供系统卷挂载与最长匹配测试使用
    Storage::Error MountAt(FileSystemId id, DeviceHandle device, const char* mountPoint, uint32_t mode);
    Storage::Error Unmount(const char* mountPoint);
    Storage::Error Stat(const char* path, Storage::FileInfo* info);
    Storage::Error Open(const char* path, uint64_t* reference);
    Storage::Error Read(uint64_t reference, uint64_t offset, void* buffer, uint32_t bytes, uint32_t* read);
    Storage::Error Close(uint64_t reference);
    Storage::Error ReadDirectory(uint64_t reference, uint64_t* cursor, char* name, uint32_t nameBytes,
        uint32_t* nameLength, Storage::FileInfo* info);
    // 已挂载文件系统的 16 字节卷标识；挂载点不存在或实现不给出时返回错误
    Storage::Error VolumeIdentity(const char* mountPoint, uint8_t* volumeId);

    // 按最长挂载点匹配解析路径：返回挂载表下标，并把卷内绝对路径写入缓冲
    Storage::Error ResolveMount(const char* path, uint32_t& mountIndex, char* volumePath, uint32_t volumePathBytes);
    // 当前挂载条数：宿主夹具检查挂载清理用
    uint32_t MountCount();

    // —— 系统卷（Boot.Volume 槽的实现）——
    Storage::Error MountSystemVolume(const Boot::Info* info, SystemVolumeFacts* facts);
}
