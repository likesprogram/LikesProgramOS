/* KernelImage.hpp
    内核镜像数据的布局、字段视图与校验：随内核镜像一起被引导器载入、用于核对内核事实与完整性

    这份数据排在镜像文件起点，随文件写入系统卷，不参与执行：BaleenCore 读到内核文件后先按它
    核对内核事实与文件长度，再按整幅文件计算 SHA-256 与数据里的摘要比对；内核入口按链接布局
    核对同一份数据，重复取摘要没有必要
    数据里的 ModuleMajor 是这颗内核所属的发布线：.osm 模块的主版本必须与它相等，
    装载器不承诺跨主版本兼容
    字节表与摘要规则由本文件单点定义；本文件只做机制，
    校验失败只回报原因文本，打印与停机由调用方决定
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Kernel {
    // 内核镜像数据的固定布局与取值：偏移均以文件起点为 0，数据排在文件起点
    struct ImageDataLayout {
        static constexpr uint32_t kOffset = 0;       // 数据相对文件起点的偏移
        static constexpr uint32_t kBytes = 0x80;     // 数据长度
        // 字段偏移，相对文件起点（数据在文件起点，等同于相对数据起点）
        static constexpr uint32_t kFieldMagic = 0x00;           // 格式标记，8 字节
        static constexpr uint32_t kFieldVersion = 0x08;         // 格式版本，16 位
        static constexpr uint32_t kFieldDataBytes = 0x0A;       // 数据长度，16 位
        static constexpr uint32_t kFieldFlags = 0x0C;           // 可选标志，32 位，当前为 0
        static constexpr uint32_t kFieldImageBytes = 0x10;      // 文件字节数，32 位
        static constexpr uint32_t kFieldMemoryBytes = 0x14;     // 静态内存跨度，32 位
        static constexpr uint32_t kFieldEntryOffset = 0x18;     // 入口文件偏移，32 位
        static constexpr uint32_t kFieldLoadAddress = 0x1C;     // 装载物理地址，32 位
        static constexpr uint32_t kFieldDigestAlgorithm = 0x20; // 摘要算法，16 位
        static constexpr uint32_t kFieldBuildIdBytes = 0x22;    // BuildId 长度，16 位
        static constexpr uint32_t kFieldBootInfoVersion = 0x24; // 内核要求的 BootInfo 版本，32 位
        static constexpr uint32_t kFieldBuildId = 0x28;         // BuildId，32 字节
        static constexpr uint32_t kFieldDigest = 0x48;          // 摘要，32 字节
        static constexpr uint32_t kFieldModuleMajor = 0x68;     // 模块主版本（发布线），16 位
        static constexpr uint32_t kFieldReserved = 0x6A;        // 保留，22 字节，全 0

        // 固定取值
        static constexpr uint16_t kVersion = 1;          // 未发布的开发格式标记
        static constexpr uint32_t kFlags = 0;            // 当前不定义可选标志
        static constexpr uint16_t kDigestSha256 = 1;     // 摘要算法编号：SHA-256，没有“禁用摘要”值
        static constexpr uint16_t kBuildIdBytes = 32;    // BuildId 长度
        static constexpr uint32_t kDigestBytes = 32;     // 摘要长度
        static constexpr uint32_t kModuleMajorBytes = 2; // 模块主版本长度
        static constexpr uint32_t kReservedBytes = 22;   // 数据内保留字段长度

        // 装载地址的固定约束：2MiB 对齐，不低于 1MiB；内核映像与静态内存从它起算
        static constexpr uint32_t kLoadAlign = 0x200000;
        static constexpr uint32_t kLoadMin = 0x100000;

        // 摘要计算：Digest 字段在覆盖范围内按 32 个零代入，相对文件起点
        static constexpr uint32_t kDigestFileOffset = kFieldDigest;        // 0x48
        static constexpr uint32_t kDigestFileEnd = kDigestFileOffset + kDigestBytes;   // 0x68
    };

    // 数据的格式标记，含终止零
    constexpr uint8_t kImageMagic[8] = { 'L', 'P', 'O', 'S', 'K', 'R', 'N', 0 };

    // 数据块视图：与字节表一一对应，无隐式填充，运行期按物理地址直接读取
    struct ImageData {
        uint8_t magic[8];              // 格式标记，字节串加终止零
        uint16_t version;              // 格式版本
        uint16_t dataBytes;            // 本结构长度
        uint32_t flags;                // 可选标志，当前为 0
        uint32_t imageBytes;           // 文件字节数，含本数据，不含扇区填充
        uint32_t memoryBytes;          // 从装载地址起的静态内存跨度，含文件与未落盘部分
        uint32_t entryOffset;          // 入口的文件偏移，须落在数据之后与文件之内
        uint32_t loadAddress;          // 内核映像装载的物理地址，按 kLoadAlign 对齐
        uint16_t digestAlgorithm;      // 摘要算法，1 为 SHA-256
        uint16_t buildIdBytes;         // BuildId 长度
        uint32_t bootInfoVersion;      // 内核要求的 BootInfo 版本
        uint8_t buildId[32];           // .os 发布线身份，不透明字节串
        uint8_t digest[32];            // 按摘要覆盖区间计算的 SHA-256 原始摘要
        uint16_t moduleMajor;          // 所属发布线的模块主版本，非零
        uint8_t reserved[22];          // 保留，全 0
    };

    // 数据字段偏移是内核构建与引导器之间的 ABI，改动必须两边同时改
    static_assert(sizeof(ImageData) == ImageDataLayout::kBytes, "内核镜像数据长度必须是 128 字节");
    static_assert(offsetof(ImageData, imageBytes) == ImageDataLayout::kFieldImageBytes, "内核镜像数据的 ImageBytes 字段偏移已变");
    static_assert(offsetof(ImageData, memoryBytes) == ImageDataLayout::kFieldMemoryBytes, "内核镜像数据的 MemoryBytes 字段偏移已变");
    static_assert(offsetof(ImageData, entryOffset) == ImageDataLayout::kFieldEntryOffset, "内核镜像数据的 EntryOffset 字段偏移已变");
    static_assert(offsetof(ImageData, loadAddress) == ImageDataLayout::kFieldLoadAddress, "内核镜像数据的 LoadAddress 字段偏移已变");
    static_assert(offsetof(ImageData, bootInfoVersion) == ImageDataLayout::kFieldBootInfoVersion, "内核镜像数据的 BootInfoVersion 字段偏移已变");
    static_assert(offsetof(ImageData, buildId) == ImageDataLayout::kFieldBuildId, "内核镜像数据的 BuildId 字段偏移已变");
    static_assert(offsetof(ImageData, digest) == ImageDataLayout::kFieldDigest, "内核镜像数据的 Digest 字段偏移已变");
    static_assert(offsetof(ImageData, moduleMajor) == ImageDataLayout::kFieldModuleMajor, "内核镜像数据的 ModuleMajor 字段偏移已变");
    static_assert(offsetof(ImageData, reserved) == ImageDataLayout::kFieldReserved, "内核镜像数据的保留字段偏移已变");

    // 数据校验通过后带出的内核事实
    struct ImageFacts {
        uint32_t imageBytes;       // 数据声明的文件字节数
        uint32_t memoryBytes;      // 数据声明的静态内存跨度，含未落盘尾部
        uint32_t entryOffset;      // 入口的文件偏移
        uint32_t loadAddress;      // 装载物理地址
        uint32_t bootInfoVersion;  // 内核要求的 BootInfo 版本
        uint32_t buildId;          // BuildId 前 4 字节按字节顺序装入的值，仅用于诊断
        uint16_t moduleMajor;      // 所属发布线的模块主版本
    };

    // 按小端读取 16 位
    uint16_t ReadU16(const uint8_t* field);

    // 按小端读取 32 位
    uint32_t ReadU32(const uint8_t* field);

    // 校验数据的固定字段、内核事实与文件长度：格式标记、版本、数据长度、标志、摘要算法、
    // BuildId 长度、ModuleMajor 非零、保留区、ImageBytes 与实际文件长度一致、MemoryBytes
    // 覆盖文件、入口落在数据之后与文件之内、装载地址对齐且映像跨度不越出 32 位
    // 成功返回空指针并把数据声明的事实写进 facts；不检查摘要，也不检查 BootInfo 版本是否
    // 为调用方所能支持：那是调用方的下一步
    const char* CheckImageData(const uint8_t* image, uint32_t fileBytes, ImageFacts& facts);
}
