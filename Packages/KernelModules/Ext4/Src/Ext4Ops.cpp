/* Ext4Ops.cpp
    Ext4 模块的 FileSystemOps 九槽表：把生成的契约函数指针表接到只读卷实现上

    context 是模块内唯一的 Ext4Volume 实例；Mount 把契约层的 BlockDeviceOps + deviceContext
    归一成 Ext4Volume 的 Ext4Device（Read 签名两者一致，不需要包装函数）
    表的初始化是常量初始化（函数地址由链接器填 R_X86_64_RELATIVE），落可写数据段，
    不产生全局动态初始化；模块内不做任何写卷操作
*/

#include <Ext4Ops.hpp>

#include <Ext4Volume.hpp>

#include <Storage/FileSystem.hpp>

namespace KernelModules {
    namespace Ext4 {
        namespace {
            // 模块内唯一的卷实例：零初始化落 .bss，不产生全局动态初始化
            // 单卷是当前实现的事实（一次启动只挂系统卷一个 Ext4 卷），Vfs 通过 context 使用它
            Ext4Volume g_volume;

            // 槽 0：格式名，诊断用
            const char* OpFormat(void* context) {
                return static_cast<Ext4Volume*>(context)->Format();
            }

            // 槽 1：识别并挂载设备；只读实现只接受只读模式，其余模式返回 NotSupported
            Storage::Error OpMount(void* context, const BlockDeviceOps* device, void* deviceContext, uint32_t mode) {
                if (context == nullptr || device == nullptr) return Storage::Error::InvalidArgument;
                // 挂载模式取值与 Storage::MountMode 一致：只读卷不接受读写挂载
                if (mode != static_cast<uint32_t>(Storage::MountMode::ReadOnly)) return Storage::Error::NotSupported;
                if (device->BlockBytes == nullptr || device->CapacityBytes == nullptr || device->Read == nullptr) return Storage::Error::InvalidArgument;
                Ext4Device access = {};
                access.context = deviceContext;
                access.blockBytes = device->BlockBytes(deviceContext);
                access.capacityBytes = device->CapacityBytes(deviceContext);
                access.Read = device->Read;
                if (access.blockBytes == 0 || access.capacityBytes == 0) return Storage::Error::InvalidArgument;
                return static_cast<Ext4Volume*>(context)->Mount(access);
            }

            // 槽 2：卸载并释放对设备的引用（只清实例状态，设备归块设备层）
            void OpUnmount(void* context) {
                if (context == nullptr) return;
                static_cast<Ext4Volume*>(context)->Unmount();
            }

            // 槽 3：回报实现能力位
            void OpCaps(void* context, Storage::FileSystemCaps* caps) {
                if (context == nullptr || caps == nullptr) return;
                *caps = static_cast<Ext4Volume*>(context)->Caps();
            }

            // 槽 4：写 16 字节卷标识（superblock 的 s_uuid），挂载成功后有效
            Storage::Error OpVolumeIdentity(void* context, uint8_t* volumeId) {
                if (context == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Ext4Volume*>(context)->VolumeIdentity(volumeId);
            }

            // 槽 5：按卷内绝对路径查询事实
            Storage::Error OpStat(void* context, const char* path, Storage::FileInfo* info) {
                if (context == nullptr || info == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Ext4Volume*>(context)->Stat(path, *info);
            }

            // 槽 6：按路径解析对象标识（inode 号）
            Storage::Error OpOpen(void* context, const char* path, uint64_t* object) {
                if (context == nullptr || object == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Ext4Volume*>(context)->Open(path, *object);
            }

            // 槽 7：从偏移读至多 bytes 字节
            Storage::Error OpRead(void* context, uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t* read) {
                if (context == nullptr || read == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Ext4Volume*>(context)->Read(object, offset, buffer, bytes, *read);
            }

            // 槽 8：游标式目录遍历
            Storage::Error OpReadDirectory(void* context, uint64_t object, uint64_t* cursor, char* name, uint32_t nameBytes, uint32_t* nameLength, Storage::FileInfo* info) {
                if (context == nullptr || cursor == nullptr || nameLength == nullptr || info == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Ext4Volume*>(context)->ReadDirectory(object, *cursor, name, nameBytes, *nameLength, *info);
            }

            // FileSystemOps 表：槽顺序与生成头 Types.hpp 一致（9 字段）
            const FileSystemOps kOps = {
                OpFormat,
                OpMount,
                OpUnmount,
                OpCaps,
                OpVolumeIdentity,
                OpStat,
                OpOpen,
                OpRead,
                OpReadDirectory,
            };
        }

        const FileSystemOps* FileSystemOpsTable() {
            return &kOps;
        }

        void* FileSystemContext() {
            return &g_volume;
        }
    }
}
