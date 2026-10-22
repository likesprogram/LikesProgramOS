/* BootVolume.cpp
    Boot.Volume v1 的实现：按 BootInfo 的卷区段与卷标识定位并挂载系统卷

    线索只缩小范围，身份由卷内标识与 BootInfo.VolumeUuid 逐字节核对；候选设备取块设备
    注册表里的整介质根设备，系统卷用偏移视图切出，按固定格式顺序（当前只有 ext4）只读
    挂载到 "/"，挂载成功后经 FileSystemOps.VolumeIdentity 取标识复核；无法唯一定位返回
    NotFound，不猜卷、不降级
    当前候选集合是全部已登记根设备（Dev.Model 候选收窄属后续接线）；成功时输出
    唯一标记行 `[ OK ] system volume mounted: uuid=<32 位小写十六进制>`，失败静默返回错误
    由内核编排按 E-KRN-70–79 汇总报 FATAL
*/

#include <ModuleAbi.hpp>

#include <VfsCore.hpp>

namespace {
    // 候选设备与系统卷区段核对：成功返回块大小与容量
    bool CandidateFacts(DeviceHandle handle, uint64_t& blockBytes, uint64_t& capacity) {
        const BlockDeviceOps* ops = nullptr;
        void* context = nullptr;
        if (VfsModule::Acquire(handle, &ops, &context) != Storage::Error::Ok) return false;
        if (ops == nullptr || ops->BlockBytes == nullptr || ops->CapacityBytes == nullptr) {
            VfsModule::Release(handle);
            return false;
        }
        const uint32_t deviceBlockBytes = ops->BlockBytes(context);
        const uint64_t deviceCapacity = ops->CapacityBytes(context);
        VfsModule::Release(handle);
        if (deviceBlockBytes < 512 || (deviceBlockBytes & (deviceBlockBytes - 1)) != 0) return false;
        blockBytes = deviceBlockBytes;
        capacity = deviceCapacity;
        return true;
    }

    // 系统卷区段是否与 BootInfo 事实自洽：非零、不回绕、不越介质容量（mediumBytes 为 0 表示未知）
    Storage::Error CheckVolumeRange(const Boot::Info* info) {
        if (info->volumeOffset == 0 || info->volumeBytes == 0) return Storage::Error::NotFound;
        if (info->volumeOffset > 0xFFFFFFFFFFFFFFFFull - info->volumeBytes) return Storage::Error::NotFound;
        if (info->mediumBytes != 0) {
            const uint64_t end = info->volumeOffset + info->volumeBytes;
            if (end > info->mediumBytes) return Storage::Error::NotFound;
        }
        return Storage::Error::Ok;
    }

    // 输出挂载成功标记：`[ OK ] system volume mounted: uuid=<32 位小写十六进制>`
    void WriteMountedMarker(const uint8_t* volumeUuid) {
        static constexpr char kPrefix[] = "system volume mounted: uuid=";
        static constexpr char kDigits[] = "0123456789abcdef";
        char text[64];
        uint32_t cursor = 0;
        for (uint32_t index = 0; kPrefix[index] != '\0'; ++index) {
            text[cursor++] = kPrefix[index];
        }
        for (uint32_t index = 0; index < 16; ++index) {
            text[cursor++] = kDigits[volumeUuid[index] >> 4];
            text[cursor++] = kDigits[volumeUuid[index] & 0x0F];
        }
        text[cursor] = '\0';
        _Contract_Import_CoreLog_v1.Line("OK", text);
    }
}

namespace VfsModule {
    Storage::Error MountSystemVolume(const Boot::Info* info, SystemVolumeFacts* facts) {
        if (!Initialized()) return Storage::Error::InvalidArgument;
        if (facts == nullptr) return Storage::Error::InvalidArgument;
        _Contract_Import_CoreMem_v1.Zero(facts, static_cast<uint64_t>(sizeof(SystemVolumeFacts)));
        // 调用方不传 BootInfo 时退回内核核心导出的 BootInfo 视图；导入槽可能为空，先查空再调用
        const Boot::Info* bootInfo = info;
        if (bootInfo == nullptr && _Contract_Import_CoreBoot_v1.Info != nullptr) {
            bootInfo = _Contract_Import_CoreBoot_v1.Info();
        }
        if (bootInfo == nullptr) return Storage::Error::InvalidArgument;
        // 卷标识全零表示引导器未提供，无法核对身份，不猜卷
        const uint8_t* const wantedUuid = bootInfo->volumeUuid;
        if (IsZeroBytes(wantedUuid, 16)) return Storage::Error::NotFound;
        const Storage::Error rangeError = CheckVolumeRange(bootInfo);
        if (rangeError != Storage::Error::Ok) return rangeError;
        // 当前只按 ext4 挂载系统卷；文件系统由 Ext4 模块登记，VFS 不解析卷格式
        FileSystemId ext4Id = 0;
        if (!FindFileSystemByFormat("ext4", &ext4Id)) return Storage::Error::NotFound;

        DeviceHandle roots[kMaxDevices];
        const uint32_t rootCount = RootDevices(roots, kMaxDevices);
        if (rootCount == 0) return Storage::Error::NotFound;

        // 逐个候选核对：区段事实、偏移视图、只读挂载、卷标识；全部通过才算命中
        uint32_t matches = 0;
        DeviceHandle matchedChild = 0;
        for (uint32_t index = 0; index < rootCount; ++index) {
            uint64_t blockBytes = 0;
            uint64_t capacity = 0;
            if (!CandidateFacts(roots[index], blockBytes, capacity)) continue;
            if ((bootInfo->volumeOffset % blockBytes) != 0) continue;
            if (bootInfo->volumeOffset >= capacity) continue;
            if (bootInfo->volumeBytes > capacity - bootInfo->volumeOffset) continue;
            DeviceHandle child = 0;
            if (CreateSubDevice(roots[index], bootInfo->volumeOffset, bootInfo->volumeBytes, 0, &child) != Storage::Error::Ok) {
                continue;
            }
            if (MountAt(ext4Id, child, "/", static_cast<uint32_t>(Storage::MountMode::ReadOnly)) != Storage::Error::Ok) {
                continue;
            }
            uint8_t uuid[16] = {};
            const Storage::Error identityError = VolumeIdentity("/", uuid);
            if (identityError != Storage::Error::Ok || IsZeroBytes(uuid, 16) || !SameBytes(uuid, wantedUuid, 16)) {
                Unmount("/");
                continue;
            }
            ++matches;
            matchedChild = child;
            Unmount("/");
        }
        // 没有候选命中与命中多个候选都按无法唯一定位处理，不按顺序猜
        if (matches != 1) return Storage::Error::NotFound;

        // 唯一候选：重新挂载并复核身份，随后填事实与成功标记
        if (MountAt(ext4Id, matchedChild, "/", static_cast<uint32_t>(Storage::MountMode::ReadOnly)) != Storage::Error::Ok) {
            return Storage::Error::Io;
        }
        uint8_t uuid[16] = {};
        const Storage::Error identityError = VolumeIdentity("/", uuid);
        if (identityError != Storage::Error::Ok || !SameBytes(uuid, wantedUuid, 16)) {
            Unmount("/");
            return identityError != Storage::Error::Ok ? identityError : Storage::Error::NotFound;
        }
        facts->bytes = static_cast<uint32_t>(sizeof(SystemVolumeFacts));
        CopyBytes(facts->volumeUuid, uuid, 16);
        CopyBytes(facts->buildId, bootInfo->kernelSetBuildId, Boot::InfoLayout::kKernelSetBuildIdBytes);
        WriteMountedMarker(uuid);
        return Storage::Error::Ok;
    }
}
