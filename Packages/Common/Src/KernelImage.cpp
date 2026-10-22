/* KernelImage.cpp
    内核镜像数据的公共校验实现：固定字段、内核事实与文件长度的一致性

    只回报原因文本，不打印也不停机；不检查摘要，也不检查 BootInfo 版本是否为调用方所能支持
    摘要按覆盖区间把 Digest 字段视作 32 个零，比对由调用方在读到整幅文件后进行
*/

#include <KernelImage.hpp>

namespace Kernel {
    uint16_t ReadU16(const uint8_t* field) {
        return static_cast<uint16_t>(field[0] | (field[1] << 8));
    }

    uint32_t ReadU32(const uint8_t* field) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
        return value;
    }

    const char* CheckImageData(const uint8_t* image, uint32_t fileBytes, ImageFacts& facts) {
        using Layout = ImageDataLayout;
        if (image == nullptr) return "The kernel image pointer is null";
        if (fileBytes < Layout::kBytes) return "The kernel image is shorter than its data";
        // 格式标记先行：不是本项目认识的内核镜像时，后面的字段没有解释意义
        for (uint32_t i = 0; i < sizeof(kImageMagic); ++i) {
            if (image[i] != kImageMagic[i]) return "The kernel image data magic doesn't match";
        }
        if (ReadU16(image + Layout::kFieldVersion) != Layout::kVersion) return "The kernel image data version isn't supported";
        if (ReadU16(image + Layout::kFieldDataBytes) != Layout::kBytes) return "The kernel image data length isn't supported";
        if (ReadU32(image + Layout::kFieldFlags) != Layout::kFlags) return "The kernel image data flags aren't supported";
        if (ReadU16(image + Layout::kFieldDigestAlgorithm) != Layout::kDigestSha256) return "The kernel image digest algorithm isn't supported";
        if (ReadU16(image + Layout::kFieldBuildIdBytes) != Layout::kBuildIdBytes) return "The kernel image build id length isn't supported";
        if (ReadU16(image + Layout::kFieldModuleMajor) == 0) return "The kernel image module major is zero";
        for (uint32_t i = 0; i < Layout::kReservedBytes; ++i) {
            if (image[Layout::kFieldReserved + i] != 0) return "The kernel image data reserved bytes aren't zero";
        }

        // 内核事实：文件长度必须与数据声明一致，内存跨度必须覆盖文件，入口必须落在数据之后
        const uint32_t imageBytes = ReadU32(image + Layout::kFieldImageBytes);
        const uint32_t memoryBytes = ReadU32(image + Layout::kFieldMemoryBytes);
        const uint32_t entry = ReadU32(image + Layout::kFieldEntryOffset);
        const uint32_t load = ReadU32(image + Layout::kFieldLoadAddress);
        const uint32_t bootInfoVersion = ReadU32(image + Layout::kFieldBootInfoVersion);
        if (imageBytes != fileBytes) return "The kernel image size doesn't match the file length";
        if (memoryBytes < imageBytes) return "The kernel image memory span doesn't cover the image";
        if (entry < Layout::kBytes || entry >= imageBytes) return "The kernel image entry offset is outside the image";
        if (load < Layout::kLoadMin || (load % Layout::kLoadAlign) != 0) return "The kernel image load address isn't aligned or is too low";
        if (bootInfoVersion == 0) return "The kernel image doesn't declare a BootInfo version";
        // 映像与静态内存按装载对齐上取整后必须落在 32 位物理地址范围内，加法不产生进位
        // 对齐量是 2 的幂，上取整用掩码，不牵入 64 位除法
        const uint64_t span = memoryBytes > imageBytes ? memoryBytes : imageBytes;
        const uint64_t spanAligned = (span + Layout::kLoadAlign - 1) & ~static_cast<uint64_t>(Layout::kLoadAlign - 1);
        if (static_cast<uint64_t>(load) + spanAligned > 0x100000000ull) return "The kernel image span runs past the 32-bit physical limit";

        facts.imageBytes = imageBytes;
        facts.memoryBytes = memoryBytes;
        facts.entryOffset = entry;
        facts.loadAddress = load;
        facts.bootInfoVersion = bootInfoVersion;
        // BuildId 是不透明字节串：前 4 字节按字节顺序装入这个仅用于诊断的值，
        // 使运行期打印与构建日志里的 BuildId 文本写法一致，不引入小端整数解释
        uint32_t buildId = 0;
        for (uint32_t i = 0; i < 4; ++i) buildId = (buildId << 8) | image[Layout::kFieldBuildId + i];
        facts.buildId = buildId;
        facts.moduleMajor = ReadU16(image + Layout::kFieldModuleMajor);
        return nullptr;
    }
}
