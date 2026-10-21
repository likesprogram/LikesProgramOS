/* LoadKernel.cpp
    BaleenCore 内核镜像检查与装载的实现：读取、数据核对、摘要比对、装载区检查与装入内存

    顺序固定：先按路径打开并取事实，再读首块核对内核镜像数据与文件长度，
    然后从文件起点流式重读整幅并比对摘要，最后核对装载区落在内存图的可用区间里
    摘要字段在首块内按 32 个零参与计算，读到整幅后再与该字段比对
    装载在检查通过之后进行：按文件偏移读进装载地址，块设备按需中转高位目标
*/

#include <LoadKernel.hpp>

#include <BootMemory.hpp>
#include <Diagnostics.hpp>
#include <KernelImage.hpp>
#include <Libc.hpp>
#include <Sha256.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            // 读取缓冲：一次一块，边读边算摘要
            // 块大小取低地址弹跳窗口（64 KiB）：读盘服务一次往返搬完一个窗口，
            // Ext4 侧对连续 extent 合并读，整幅摘要的块设备往返次数因此按段而不是按块计
            constexpr uint32_t kChunkBytes = kBounceBytes;
            uint8_t s_chunk[kChunkBytes];

            // —— 本阶段失败原因的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailOpen{BALEEN_STAGE_CODE("33"), "Open the kernel image"};
            constexpr Diagnostic kFailFacts{BALEEN_STAGE_CODE("34"), "Read the kernel image facts"};
            constexpr Diagnostic kFailDirectory{BALEEN_STAGE_CODE("35"), "The kernel image path points to a directory"};
            constexpr Diagnostic kFailLength{BALEEN_STAGE_CODE("36"), "The kernel image length is out of range"};
            constexpr Diagnostic kFailRead{BALEEN_STAGE_CODE("37"), "Read the kernel image"};
            constexpr Diagnostic kFailShort{BALEEN_STAGE_CODE("38"), "The kernel image is shorter than its declared size"};
            constexpr Diagnostic kFailBootInfo{BALEEN_STAGE_CODE("39"), "The kernel asks for a BootInfo version that isn't supported"};
            constexpr Diagnostic kFailLoadSpan{BALEEN_STAGE_CODE("52"), "The kernel load span isn't inside a usable memory area"};
            // 引导器自己还在这段内存里执行：镜像数据的固定装载地址不得与它重叠
            constexpr Diagnostic kFailLoadOverlap{BALEEN_STAGE_CODE("91"), "The kernel load span overlaps the bootloader"};
            constexpr Diagnostic kFailDigest{BALEEN_STAGE_CODE("53"), "The kernel image digest doesn't match"};
            // 内核镜像数据的字段校验在 Packages/Common 的实现里，原因文本由它给出，编号在本阶段统一
            constexpr Diagnostic kFailKernelData{BALEEN_STAGE_CODE("54"), "The kernel image data is invalid"};
            // 与清单验证的 E-CORE-65 同一编号与文本：装载路径接收所选集的期望标识，
            // 镜像 BuildId 不符时在装载之前拒绝，挡住“验证 A、装载 B”
            constexpr Diagnostic kFailBuildId{BALEEN_STAGE_CODE("65"), "The manifest BuildId doesn't match the selected set"};
            constexpr Diagnostic kFailLoad{BALEEN_STAGE_CODE("57"), "Load the kernel image into memory"};

            // 从 offset 起读 want 字节到块缓冲；长度不足或读失败返回带编号的原因
            Diagnostic ReadChunk(Storage::FileSystem& fs, const Storage::FileId& id, uint32_t offset, uint32_t want) {
                uint32_t read = 0;
                if (const Storage::Error error = fs.Read(id, offset, s_chunk, want, read); error != Storage::Error::Ok) return kFailRead;
                if (read != want) return kFailShort;
                return Diagnostic{};
            }
        }

        Diagnostic CheckKernelImage(Storage::FileSystem& fs, const char* path, const uint8_t expectedBuildId[32], const Boot::MemoryMapEntry* memoryMap, uint32_t memoryMapCount, uint64_t loaderBegin, uint64_t loaderBytes, KernelFacts& facts, bool& missing) {
            missing = false;
            Storage::FileId id;
            const Storage::Error open = fs.Open(path, id);
            if (open == Storage::Error::NotFound) {
                missing = true;
                return Diagnostic{};
            }
            if (open != Storage::Error::Ok) return kFailOpen;
            Storage::FileInfo info;
            if (const Storage::Error error = fs.Stat(path, info); error != Storage::Error::Ok) return kFailFacts;
            if (info.directory) return kFailDirectory;
            if (info.bytes == 0 || info.bytes > 0xFFFFFFFFull) return kFailLength;
            const uint32_t fileBytes = static_cast<uint32_t>(info.bytes);

            // 首块：核对内核镜像数据与文件长度；摘要字段在参与计算前先取出、抹零
            const uint32_t headBytes = fileBytes < kChunkBytes ? fileBytes : kChunkBytes;
            if (const Diagnostic reason = ReadChunk(fs, id, 0, headBytes); reason.code != nullptr) return reason;
            Kernel::ImageFacts kernelFacts;
            if (const char* reason = Kernel::CheckImageData(s_chunk, fileBytes, kernelFacts)) return Diagnostic{kFailKernelData.code, reason};
            // 装载路径必须装载所选集：镜像数据里的 BuildId 与选版定下的标识逐字节相同
            if (memcmp(s_chunk + Kernel::ImageDataLayout::kFieldBuildId, expectedBuildId, Kernel::ImageDataLayout::kBuildIdBytes) != 0) return kFailBuildId;
            if (kernelFacts.bootInfoVersion != Boot::InfoLayout::kVersion) return kFailBootInfo;
            uint8_t expected[Kernel::ImageDataLayout::kDigestBytes] = {};
            memcpy(expected, s_chunk + Kernel::ImageDataLayout::kFieldDigest, sizeof(expected));
            memset(s_chunk + Kernel::ImageDataLayout::kFieldDigest, 0, sizeof(expected));

            // 装载区：映像与静态内存的较大者必须整段落在可用区间；判定与 Stub、BaleenCore 的
            // 弹跳窗口共用 SpanUsable，相邻可用条目合并，三处不会对同一张内存图给出不同结论
            // 内存图为空表示调用方在装载之后自行核对（UEFI 在退出启动服务后按最终内存图核对）
            const uint64_t span = kernelFacts.memoryBytes > fileBytes ? kernelFacts.memoryBytes : fileBytes;
            // 装载跨度不得与仍在执行的引导器映像重叠：BIOS 侧 kLoadMin 正好是 BaleenCore 的装入地址，
            // 不核对时声明 LoadAddress=1MiB 的镜像能通过 SpanUsable，装载即覆盖 BaleenCore 自身
            if (loaderBytes != 0 &&
                static_cast<uint64_t>(kernelFacts.loadAddress) < loaderBegin + loaderBytes &&
                loaderBegin < static_cast<uint64_t>(kernelFacts.loadAddress) + span)
                return kFailLoadOverlap;
            if (memoryMap != nullptr && !SpanUsable(memoryMap, memoryMapCount, kernelFacts.loadAddress, span)) return kFailLoadSpan;

            // 摘要：从文件起点读到末尾，首块的摘要字段已按零参与
            Digest::Sha256 sha;
            sha.Begin();
            sha.Update(s_chunk, headBytes);
            for (uint32_t offset = kChunkBytes; offset < fileBytes; offset += kChunkBytes) {
                const uint32_t want = (fileBytes - offset) < kChunkBytes ? (fileBytes - offset) : kChunkBytes;
                if (const Diagnostic reason = ReadChunk(fs, id, offset, want); reason.code != nullptr) return reason;
                sha.Update(s_chunk, want);
            }
            uint8_t digest[Digest::kSha256Bytes];
            sha.Finish(digest);
            if (memcmp(digest, expected, sizeof(digest)) != 0) return kFailDigest;

            facts.imageBytes = kernelFacts.imageBytes;
            facts.memoryBytes = kernelFacts.memoryBytes;
            facts.entryOffset = kernelFacts.entryOffset;
            facts.loadAddress = kernelFacts.loadAddress;
            facts.bootInfoVersion = kernelFacts.bootInfoVersion;
            facts.buildId = kernelFacts.buildId;
            return Diagnostic{};
        }

        Diagnostic LoadKernelImage(Storage::FileSystem& fs, const char* path, uint32_t loadAddress, uint32_t imageBytes) {
            return LoadKernelImageTo(fs, path, reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(loadAddress)), imageBytes);
        }

        Diagnostic LoadKernelImageTo(Storage::FileSystem& fs, const char* path, uint8_t* destination, uint32_t imageBytes) {
            if (destination == nullptr) return kFailLoad;
            Storage::FileId id;
            if (fs.Open(path, id) != Storage::Error::Ok) return kFailOpen;
            // 按文件偏移逐块读进目标缓冲：块设备内部按窗口中转，这里不再分缓冲
            for (uint32_t offset = 0; offset < imageBytes; offset += kChunkBytes) {
                const uint32_t want = (imageBytes - offset) < kChunkBytes ? (imageBytes - offset) : kChunkBytes;
                uint32_t read = 0;
                if (fs.Read(id, offset, destination + offset, want, read) != Storage::Error::Ok) return kFailLoad;
                if (read != want) return kFailShort;
            }
            return Diagnostic{};
        }
    }
}
