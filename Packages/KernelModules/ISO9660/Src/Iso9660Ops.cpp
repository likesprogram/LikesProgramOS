/* Iso9660Ops.cpp
    ISO9660 模块的 FileSystemOps 九槽表：把生成的契约函数指针表接到只读卷实现上

    context 是模块内唯一的 Iso9660Volume 实例；Mount 把契约层的 BlockDeviceOps + deviceContext
    归一成 Iso9660Volume 的 IsoDevice（Read 签名两者一致，不需要包装函数）
    表的初始化是常量初始化（函数地址由链接器填 R_X86_64_RELATIVE），不产生全局动态初始化
*/

#include <Iso9660Ops.hpp>

#include <Iso9660Volume.hpp>

#include <Storage/FileSystem.hpp>

namespace KernelModules {
    namespace Iso9660 {
        namespace {
            // 模块内唯一的卷实例：零初始化落 .bss，不产生全局动态初始化
            Iso9660Volume g_volume;

            // 槽 0：格式名，诊断用
            const char* OpFormat(void* context) {
                return static_cast<Iso9660Volume*>(context)->Format();
            }

            // 槽 1：识别并挂载设备；只读实现只接受只读模式
            Storage::Error OpMount(void* context, const BlockDeviceOps* device, void* deviceContext, uint32_t mode) {
                if (context == nullptr || device == nullptr) return Storage::Error::InvalidArgument;
                if (mode != static_cast<uint32_t>(Storage::MountMode::ReadOnly)) return Storage::Error::NotSupported;
                if (device->BlockBytes == nullptr || device->CapacityBytes == nullptr || device->Read == nullptr) return Storage::Error::InvalidArgument;
                IsoDevice access = {};
                access.context = deviceContext;
                access.blockBytes = device->BlockBytes(deviceContext);
                access.capacityBytes = device->CapacityBytes(deviceContext);
                access.Read = device->Read;
                if (access.blockBytes == 0 || access.capacityBytes == 0) return Storage::Error::InvalidArgument;
                return static_cast<Iso9660Volume*>(context)->Mount(access);
            }

            // 槽 2：卸载
            void OpUnmount(void* context) {
                if (context == nullptr) return;
                static_cast<Iso9660Volume*>(context)->Unmount();
            }

            // 槽 3：能力位
            void OpCaps(void* context, Storage::FileSystemCaps* caps) {
                if (context == nullptr || caps == nullptr) return;
                *caps = static_cast<Iso9660Volume*>(context)->Caps();
            }

            // 槽 4：卷标识（主卷描述符的 Volume Identifier 取前 16 字节）
            Storage::Error OpVolumeIdentity(void* context, uint8_t* volumeId) {
                if (context == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Iso9660Volume*>(context)->VolumeIdentity(volumeId);
            }

            // 槽 5：按卷内绝对路径查询事实
            Storage::Error OpStat(void* context, const char* path, Storage::FileInfo* info) {
                if (context == nullptr || info == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Iso9660Volume*>(context)->Stat(path, *info);
            }

            // 槽 6：按路径解析对象标识
            Storage::Error OpOpen(void* context, const char* path, uint64_t* object) {
                if (context == nullptr || object == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Iso9660Volume*>(context)->Open(path, *object);
            }

            // 槽 7：从偏移读至多 bytes 字节
            Storage::Error OpRead(void* context, uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t* read) {
                if (context == nullptr || read == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Iso9660Volume*>(context)->Read(object, offset, buffer, bytes, *read);
            }

            // 槽 8：游标式目录遍历
            Storage::Error OpReadDirectory(void* context, uint64_t object, uint64_t* cursor, char* name, uint32_t nameBytes, uint32_t* nameLength, Storage::FileInfo* info) {
                if (context == nullptr || cursor == nullptr || nameLength == nullptr || info == nullptr) return Storage::Error::InvalidArgument;
                return static_cast<Iso9660Volume*>(context)->ReadDirectory(object, *cursor, name, nameBytes, *nameLength, *info);
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
