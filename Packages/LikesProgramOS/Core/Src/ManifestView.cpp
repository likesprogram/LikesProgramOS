/* ManifestView.cpp
    ManifestView 的实现：按字节表有界解析清单副本，失败只回报原因文本

    本文件不做签名验证与摘要复算：交接来的清单副本是 Baleen 验签通过、签名值已归零的那一份，
    内核只核对结构与条目事实；失败原因文本给调用方当 detail，编号由调用方按失败点选
*/

#include <ManifestView.hpp>

#include <KernelImage.hpp>
#include <Libc.hpp>

namespace {
    // 字节表常量的短别名：本文件多处引用，别名让判据行保持单行
    namespace ML = KernelSet::ManifestLayout;

    // 按小端读取 64 位
    uint64_t ReadU64(const uint8_t* field) {
        return static_cast<uint64_t>(Kernel::ReadU32(field)) | (static_cast<uint64_t>(Kernel::ReadU32(field + 4)) << 32);
    }

    // Modules.list 的固定相对路径与长度
    constexpr uint32_t kBootListPathBytes = 12;
    constexpr char kBootListPath[] = "Modules.list";
    // 模块相对路径的固定前缀
    constexpr uint32_t kModulesPrefixBytes = 8;
    constexpr char kModulesPrefix[] = "Modules/";
}

namespace Kernel {
    bool PathValid(const uint8_t* path, uint32_t pathBytes) {
        if (path == nullptr || pathBytes == 0 || pathBytes > ML::kEntryPathMaxBytes) return false;
        if (path[0] == '/') return false;
        // 逐分量检查：分量非空、不是 "." 或 ".."、不含 NUL；结尾的分隔符会留下空分量而被拒
        uint32_t begin = 0;
        for (uint32_t i = 0; i <= pathBytes; ++i) {
            if (i < pathBytes && path[i] != '/') {
                if (path[i] == 0) return false;
                continue;
            }
            const uint32_t componentBytes = i - begin;
            if (componentBytes == 0) return false;
            if (componentBytes == 1 && path[begin] == '.') return false;
            if (componentBytes == 2 && path[begin] == '.' && path[begin + 1] == '.') return false;
            begin = i + 1;
        }
        return true;
    }

    bool ModulePathValid(const uint8_t* path, uint32_t pathBytes) {
        if (!PathValid(path, pathBytes)) return false;
        if (pathBytes <= kModulesPrefixBytes) return false;
        if (memcmp(path, kModulesPrefix, kModulesPrefixBytes) != 0) return false;
        // 最后一段（文件名）必须带版本：至少一个 '.'，且前后都有字符
        uint32_t begin = pathBytes;
        while (begin > 0 && path[begin - 1] != '/') --begin;
        bool versioned = false;
        for (uint32_t i = begin; i < pathBytes; ++i) {
            if (path[i] != '.') continue;
            if (i == begin || i + 1 == pathBytes) return false;
            versioned = true;
        }
        return versioned;
    }

    const char* ParseManifest(const uint8_t* bytes, uint32_t length, ManifestView& view) {
        view = ManifestView{};
        if (bytes == nullptr) return "the manifest copy is null";
        if (length < ML::kHeaderBytes + ML::kSignatureAreaBytes) return "the manifest length is below the fixed header and signature area";
        if (length > ML::kManifestMaxBytes) return "the manifest length is beyond the limit";
        if (Kernel::ReadU32(bytes + ML::kFieldMagic) != ML::kMagic) return "the manifest magic doesn't match";
        if (Kernel::ReadU16(bytes + ML::kFieldVersion) != ML::kVersion) return "the manifest version isn't supported";
        if (Kernel::ReadU16(bytes + ML::kFieldHeaderBytes) != ML::kHeaderBytes) return "the manifest header length isn't supported";
        if (Kernel::ReadU32(bytes + ML::kFieldBytes) != length) return "the manifest length field doesn't match the handoff length";
        if (Kernel::ReadU32(bytes + ML::kFieldFlags) != 0) return "the manifest flags aren't zero";
        if (Kernel::ReadU16(bytes + ML::kFieldReserved16) != 0 || Kernel::ReadU32(bytes + ML::kFieldReserved32) != 0)
            return "the manifest reserved fields aren't zero";
        bool buildIdZero = true;
        for (uint32_t i = 0; i < ML::kBuildIdBytes; ++i) {
            if (bytes[ML::kFieldBuildId + i] == 0) continue;
            buildIdZero = false;
            break;
        }
        if (buildIdZero) return "the manifest build id is zero";
        const uint32_t count = Kernel::ReadU16(bytes + ML::kFieldArtifactCount);
        if (count < 2 || count > ML::kEntryMaxCount) return "the manifest artifact count is out of range";
        // 签名区只核对固定字段：签名值 64 字节由 Baleen 归零，内核不验签也不复算摘要
        const uint32_t signatureBegin = length - ML::kSignatureAreaBytes;
        if (bytes[signatureBegin + ML::kSignatureAlgorithm] != ML::kAlgorithmEd25519) return "the manifest signature algorithm isn't supported";
        if (bytes[signatureBegin + ML::kSignatureKeyId] == 0) return "the manifest signature key id is reserved";
        if (bytes[signatureBegin + ML::kSignatureReserved] != 0 || bytes[signatureBegin + ML::kSignatureReserved + 1] != 0)
            return "the manifest signature reserved bytes aren't zero";

        uint32_t offset = ML::kFieldArtifacts;
        uint32_t osEntries = 0;
        uint32_t listEntries = 0;
        for (uint32_t index = 0; index < count; ++index) {
            if (offset > signatureBegin || ML::kEntryFixedBytes > signatureBegin - offset) return "the manifest entry runs past the entry area";
            const uint8_t* const entry = bytes + offset;
            const uint32_t pathBytes = Kernel::ReadU16(entry + ML::kEntryPathBytes);
            if (pathBytes == 0 || pathBytes > ML::kEntryPathMaxBytes) return "the manifest entry path length is out of range";
            if (ML::kEntryFixedBytes + pathBytes > signatureBegin - offset) return "the manifest entry runs past the entry area";
            const uint8_t type = entry[ML::kEntryType];
            if (type != ML::kTypeOs && type != ML::kTypeOsm && type != ML::kTypeBootList) return "the manifest entry type isn't supported";
            if (entry[ML::kEntryReserved] != 0) return "the manifest entry reserved byte isn't zero";
            const uint64_t dataBytes = ReadU64(entry + ML::kEntryDataBytes);
            if (dataBytes == 0) return "the manifest entry data length is zero";
            const uint8_t* const path = entry + ML::kEntryPath;
            if (!PathValid(path, pathBytes)) return "the manifest entry path is invalid";
            if (type == ML::kTypeOs) {
                ++osEntries;
            } else if (type == ML::kTypeBootList) {
                ++listEntries;
                if (pathBytes != kBootListPathBytes || memcmp(path, kBootListPath, kBootListPathBytes) != 0) return "the manifest Modules.list path is wrong";
            } else if (!ModulePathValid(path, pathBytes)) {
                return "the manifest module path is wrong";
            }
            // 路径按字节唯一：同一路径出现两次即格式不符
            for (uint32_t i = 0; i < view.entryCount; ++i) {
                if (view.entries[i].pathBytes != pathBytes) continue;
                if (memcmp(view.entries[i].path, path, pathBytes) == 0) return "the manifest entry path is duplicated";
            }
            ManifestEntry& out = view.entries[view.entryCount];
            out.type = type;
            out.pathBytes = pathBytes;
            out.dataBytes = dataBytes;
            out.path = path;
            out.digest = entry + ML::kEntryDigest;
            ++view.entryCount;
            offset += ML::kEntryFixedBytes + pathBytes;
        }
        if (offset != signatureBegin) return "the manifest entries don't fill the entry area";
        if (osEntries != 1) return "the manifest must carry exactly one .os entry";
        if (listEntries != 1) return "the manifest must carry exactly one Modules.list entry";

        memcpy(view.buildId, bytes + ML::kFieldBuildId, ML::kBuildIdBytes);
        view.bytes = bytes;
        view.length = length;
        view.securityVersion = ReadU64(bytes + ML::kFieldSecurityVersion);
        return nullptr;
    }

    const ManifestEntry* FindManifestEntry(const ManifestView& view, const uint8_t* path, uint32_t pathBytes) {
        if (path == nullptr || pathBytes == 0) return nullptr;
        for (uint32_t i = 0; i < view.entryCount; ++i) {
            const ManifestEntry& entry = view.entries[i];
            if (entry.pathBytes != pathBytes) continue;
            if (memcmp(entry.path, path, pathBytes) == 0) return &entry;
        }
        return nullptr;
    }
}
