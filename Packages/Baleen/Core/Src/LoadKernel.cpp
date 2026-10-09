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
            constexpr Diagnostic kFailOpen{"E-CORE-33", "Open the kernel image"};
            constexpr Diagnostic kFailFacts{"E-CORE-34", "Read the kernel image facts"};
            constexpr Diagnostic kFailDirectory{"E-CORE-35", "The kernel image path points to a directory"};
            constexpr Diagnostic kFailLength{"E-CORE-36", "The kernel image length is out of range"};
            constexpr Diagnostic kFailRead{"E-CORE-37", "Read the kernel image"};
            constexpr Diagnostic kFailShort{"E-CORE-38", "The kernel image is shorter than its declared size"};
            constexpr Diagnostic kFailBootInfo{"E-CORE-39", "The kernel asks for a BootInfo version that isn't supported"};
            constexpr Diagnostic kFailLoadSpan{"E-CORE-52", "The kernel load span isn't inside a usable memory area"};
            constexpr Diagnostic kFailDigest{"E-CORE-53", "The kernel image digest doesn't match"};
            // 内核镜像数据的字段校验在 Packages/Common 的实现里，原因文本由它给出，编号在本阶段统一
            constexpr Diagnostic kFailKernelData{"E-CORE-54", "The kernel image data is invalid"};
            constexpr Diagnostic kFailLoad{"E-CORE-57", "Load the kernel image into memory"};

            // 从 offset 起读 want 字节到块缓冲；长度不足或读失败返回带编号的原因
            Diagnostic ReadChunk(Storage::FileSystem& fs, const Storage::FileId& id, uint32_t offset, uint32_t want) {
                uint32_t read = 0;
                if (const Storage::Error error = fs.Read(id, offset, s_chunk, want, read); error != Storage::Error::Ok) return kFailRead;
                if (read != want) return kFailShort;
                return Diagnostic{};
            }
        }

        Diagnostic CheckKernelImage(Storage::FileSystem& fs, const char* path, const Boot::MemoryMapEntry* memoryMap, uint32_t memoryMapCount, KernelFacts& facts, bool& missing) {
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
            if (kernelFacts.bootInfoVersion != Boot::InfoLayout::kVersion) return kFailBootInfo;
            uint8_t expected[Kernel::ImageDataLayout::kDigestBytes] = {};
            memcpy(expected, s_chunk + Kernel::ImageDataLayout::kFieldDigest, sizeof(expected));
            memset(s_chunk + Kernel::ImageDataLayout::kFieldDigest, 0, sizeof(expected));

            // 装载区：映像与静态内存的较大者必须整段落在可用区间；判定与 Stub、BaleenCore 的
            // 弹跳窗口共用 SpanUsable，相邻可用条目合并，三处不会对同一张内存图给出不同结论
            const uint64_t span = kernelFacts.memoryBytes > fileBytes ? kernelFacts.memoryBytes : fileBytes;
            if (!SpanUsable(memoryMap, memoryMapCount, kernelFacts.loadAddress, span)) return kFailLoadSpan;

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
            Storage::FileId id;
            if (fs.Open(path, id) != Storage::Error::Ok) return kFailOpen;
            // 目标地址在高位时按文件偏移逐块写入：块设备内部按窗口中转，这里不再分缓冲
            uint8_t* const dest = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(loadAddress));
            for (uint32_t offset = 0; offset < imageBytes; offset += kChunkBytes) {
                const uint32_t want = (imageBytes - offset) < kChunkBytes ? (imageBytes - offset) : kChunkBytes;
                uint32_t read = 0;
                if (fs.Read(id, offset, dest + offset, want, read) != Storage::Error::Ok) return kFailLoad;
                if (read != want) return kFailShort;
            }
            return Diagnostic{};
        }
    }
}
