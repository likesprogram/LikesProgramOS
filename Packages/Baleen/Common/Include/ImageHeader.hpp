/* ImageHeader.hpp
    Baleen 镜像完整性头：Stub 与 Core 共用的固定布局、字段视图与校验

    两种镜像同构：文件偏移 0 起是 16 位近跳转前缀与保留区，0x10 起是 128 字节头，
    0x90 起是早期入口；只有格式标记不同（Stub 为 BLNSTUB、Core 为 BLNCORE），
    字段偏移、固定取值与摘要覆盖区间完全一致
    字节表与自引用处理见 Packages/Baleen/Stub/README.md 第三、四节
    本文件只做机制：校验失败只回报原因文本，打印与停机由调用方决定
*/

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <Sha256.hpp>

namespace Baleen {
    // 镜像完整性头的固定布局与取值：偏移均以文件起点为 0
    struct ImageHeaderLayout {
        static constexpr uint32_t kOffset = 0x10;        // 头相对文件起点的偏移
        static constexpr uint32_t kBytes = 0x80;         // 头固定长度
        static constexpr uint32_t kEntryOffset = 0x90;   // 早期入口的文件偏移，即入口前缀的目标
        static constexpr uint32_t kPrefixBytes = 3;      // 文件起点的 16 位近跳转前缀
        static constexpr uint32_t kLeadBytes = 0x10;     // 入口前缀与保留区合计，即头起点
        static constexpr uint8_t kNearJumpOpcode = 0xE9; // 近跳转操作码

        // 头内字段偏移，相对头起点
        static constexpr uint32_t kFieldMagic = 0x00;            // 格式标记，8 字节
        static constexpr uint32_t kFieldVersion = 0x08;          // 格式标记版本，16 位
        static constexpr uint32_t kFieldHeaderBytes = 0x0A;      // 头长度，16 位
        static constexpr uint32_t kFieldFlags = 0x0C;            // 可选标志，32 位，当前为 0
        static constexpr uint32_t kFieldImageBytes = 0x10;       // 文件字节数，32 位
        static constexpr uint32_t kFieldMemoryBytes = 0x14;      // 静态内存跨度，32 位
        static constexpr uint32_t kFieldEntryOffset = 0x18;      // 早期入口偏移，32 位
        static constexpr uint32_t kFieldDigestAlgorithm = 0x1C;  // 摘要算法，16 位
        static constexpr uint32_t kFieldBuildIdBytes = 0x1E;     // BuildId 长度，16 位
        static constexpr uint32_t kFieldBuildId = 0x20;          // BuildId，32 字节
        static constexpr uint32_t kFieldDigest = 0x40;           // 摘要，32 字节
        static constexpr uint32_t kFieldReserved = 0x60;         // 保留，32 字节，全 0

        // 固定取值
        static constexpr uint16_t kVersion = 1;          // 未发布的开发格式标记
        static constexpr uint32_t kFlags = 0;            // 当前不定义可选标志
        static constexpr uint16_t kDigestSha256 = 1;     // 摘要算法编号，没有“禁用摘要”值
        static constexpr uint16_t kBuildIdBytes = 32;    // BuildId 长度
        static constexpr uint32_t kDigestBytes = 32;     // 摘要长度
        static constexpr uint32_t kReservedBytes = 32;   // 头内保留字段长度

        // 摘要计算：Digest 字段在覆盖范围内按 32 个零代入，相对文件起点
        static constexpr uint32_t kDigestFileOffset = kOffset + kFieldDigest;        // 0x50
        static constexpr uint32_t kDigestFileEnd = kDigestFileOffset + kDigestBytes; // 0x70
    };

    // 两种镜像的格式标记，含终止零：布局同构，标记与各自的结构文档独立
    constexpr uint8_t kStubImageMagic[8] = { 'B', 'L', 'N', 'S', 'T', 'U', 'B', 0 };
    constexpr uint8_t kCoreImageMagic[8] = { 'B', 'L', 'N', 'C', 'O', 'R', 'E', 0 };

    // 头字段视图：布局与字节表一一对应，无隐式填充，运行期按物理地址直接读取
    struct ImageHeader {
        uint8_t magic[8];              // 格式标记，字节串加终止零
        uint16_t version;              // 格式标记版本
        uint16_t headerBytes;          // 本结构长度
        uint32_t flags;                // 可选标志，当前为 0
        uint32_t imageBytes;           // 文件字节数，不含扇区填充与未落盘部分
        uint32_t memoryBytes;          // 从装载点起的静态内存跨度，含文件与未落盘部分
        uint32_t entryOffset;          // 早期入口的文件偏移
        uint16_t digestAlgorithm;      // 摘要算法，1 为 SHA-256
        uint16_t buildIdBytes;         // BuildId 长度
        uint8_t buildId[32];           // 本次构建的标识，不透明字节串
        uint8_t digest[32];            // 按摘要覆盖区间计算的 SHA-256 原始摘要
        uint8_t reserved[32];          // 保留，全 0
    };

    // 头字段偏移是发布方与校验方之间的 ABI，改动必须两边同时改
    static_assert(sizeof(ImageHeader) == ImageHeaderLayout::kBytes, "镜像头长度必须是 128 字节");
    static_assert(offsetof(ImageHeader, imageBytes) == ImageHeaderLayout::kFieldImageBytes, "镜像头的 ImageBytes 字段偏移已变");
    static_assert(offsetof(ImageHeader, memoryBytes) == ImageHeaderLayout::kFieldMemoryBytes, "镜像头的 MemoryBytes 字段偏移已变");
    static_assert(offsetof(ImageHeader, entryOffset) == ImageHeaderLayout::kFieldEntryOffset, "镜像头的 EntryOffset 字段偏移已变");
    static_assert(offsetof(ImageHeader, buildId) == ImageHeaderLayout::kFieldBuildId, "镜像头的 BuildId 字段偏移已变");
    static_assert(offsetof(ImageHeader, digest) == ImageHeaderLayout::kFieldDigest, "镜像头的 Digest 字段偏移已变");
    static_assert(offsetof(ImageHeader, reserved) == ImageHeaderLayout::kFieldReserved, "镜像头的保留字段偏移已变");

    // 头字段校验通过后带出的镜像事实
    struct ImageFacts {
        uint32_t imageBytes;    // 头声明的文件字节数
        uint32_t memoryBytes;   // 头声明的静态内存跨度，含未落盘尾部
        uint32_t entryOffset;   // 早期入口的文件偏移
        uint32_t buildId;       // BuildId 前 4 字节，仅用于诊断
    };

    // 按小端读取 16 位
    uint16_t ReadU16(const uint8_t* field);

    // 按小端读取 32 位
    uint32_t ReadU32(const uint8_t* field);

    // 校验头的固定字段与入口前缀：格式标记、版本、头长、标志、摘要算法、BuildId 长度、
    // 保留区、长度与入口范围、入口前缀与 EntryOffset 一致
    // 成功返回空指针并把头声明的事实写进 facts；不检查摘要，也不检查长度与实际布局或
    // 介质描述的对应关系：那是调用方的环境事实
    const char* CheckImageHeader(const uint8_t* image, const uint8_t magic[8], ImageFacts& facts);

    // 比对整幅镜像的 SHA-256 摘要：Digest 字段按 32 个零代入，不改写镜像
    // imageBytes 必须已由 CheckImageHeader 核对过，且不小于摘要覆盖区间
    const char* CheckImageDigest(const uint8_t* image, uint32_t imageBytes);
}
