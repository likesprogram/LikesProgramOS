/* ManifestVerify.cpp
    内核集清单与产物验证的实现：读清单、核对结构与签名、防回滚比较、逐产物摘要与列表产物视图

    清单最大 0x10000 字节，产物的摘要按 64 KiB 块流式重算；两者各用一块静态缓冲
    签名覆盖是整份清单（签名值 64 字节视作零），ManifestDigest 与签名使用同一段字节
    集内一致的内核镜像数据检查只读 .os 文件的首块，摘要在逐产物的流式比对里覆盖
    验证通过的清单副本（签名值已归零）、Modules.list 副本与列表产物视图经 ManifestFacts 带回：
    清单副本是交给内核的字节，列表副本与计划供预装使用；三者都在本文件的静态存储里，
    交权前一直有效
    旧修订快照（Manifest.<rev>.mft / Modules.<rev>.list）走同一套验证，只换文件名来源
*/

#include <ManifestVerify.hpp>

#include <BootMemory.hpp>
#include <Ed25519.hpp>
#include <KernelImage.hpp>
#include <LayoutCheck.hpp>
#include <Libc.hpp>
#include <SecurityFloor.hpp>
#include <Sha256.hpp>
#include <TrustAnchor.hpp>

namespace Baleen {
    namespace Core {
        namespace {
            namespace ML = KernelSet::ManifestLayout;

            // 清单缓冲：按清单长度上限分配
            uint8_t s_manifest[ML::kManifestMaxBytes];
            // 产物摘要的读取缓冲：块大小取低地址弹跳窗口，与装载路径一致
            uint8_t s_chunk[kBounceBytes];
            // 清单固定路径名：当前修订的清单与列表都放在集目录根上
            constexpr char kManifestPath[] = "Manifest.mft";
            // 模块加载列表在集内的固定相对路径：与 Manifest.mft 同级，内容受清单摘要覆盖
            constexpr char kBootListPath[] = "Modules.list";
            // 列表容量上限：30 条最长路径（255 字节）加固定头，按 16 KiB 给解析缓冲
            constexpr uint32_t kBootListMaxBytes = 16384;
            // 列表固定头的读取上限：三行 ASCII 远小于这个长度，取不变式上界即可
            constexpr uint32_t kBootListHeadBytes = 96;
            // 列表副本：交给内核与预装实现的字节，交权前一直有效
            uint8_t s_bootList[kBootListMaxBytes];
            // 列表固定头的读缓冲：修订选择只读这几行，不做签名校验
            uint8_t s_bootListHead[kBootListHeadBytes];
            // 列表产物视图：静态存储，随 facts 带回；一次验证只保留最后一份视图
            ListedArtifact s_listedArtifacts[kBootListMaxEntries];
            // 集内路径缓冲：集目录（由布局描述的内核路径推出，至多 254 字节）加 '/'
            // 加相对路径（上限 255）组合后不超过 510 字节，加结尾空字符仍在 512 内
            // 集内路径组合上限写死为 510 字节，超限按清单格式错误拒绝
            constexpr uint32_t kPathBytes = 512;
            char s_path[kPathBytes];

            // 引导器内嵌的最低允许 SecurityVersion 由构建期生成头给出：
            // 默认 0 表示不吊销任何发行世代；make SECURITY_FLOOR=<n> 抬高它即吊销更低世代
            // 撤销的生效点是一次可信的引导器更新

            // —— 本阶段失败原因的编号：编号是稳定标识，文本可以润色 ——
            constexpr Diagnostic kFailKernelData{BALEEN_STAGE_CODE("54"), "The kernel image data is invalid"};
            constexpr Diagnostic kFailManifestRead{BALEEN_STAGE_CODE("61"), "Read the kernel set manifest"};
            constexpr Diagnostic kFailManifestFormat{BALEEN_STAGE_CODE("62"), "The kernel set manifest is invalid"};
            constexpr Diagnostic kFailManifestKey{BALEEN_STAGE_CODE("63"), "The manifest signature key isn't trusted"};
            constexpr Diagnostic kFailManifestSignature{BALEEN_STAGE_CODE("64"), "The manifest signature doesn't verify"};
            constexpr Diagnostic kFailManifestBuildId{BALEEN_STAGE_CODE("65"), "The manifest BuildId doesn't match the selected set"};
            constexpr Diagnostic kFailRollback{BALEEN_STAGE_CODE("66"), "The manifest SecurityVersion is below what the device or the bootloader allows"};
            constexpr Diagnostic kFailArtifactRead{BALEEN_STAGE_CODE("67"), "Read a kernel set artifact"};
            constexpr Diagnostic kFailArtifactDigest{BALEEN_STAGE_CODE("68"), "A kernel set artifact digest doesn't match"};
            constexpr Diagnostic kFailKeyRevoked{BALEEN_STAGE_CODE("74"), "The manifest signature key is revoked"};
            constexpr Diagnostic kFailModuleMajor{BALEEN_STAGE_CODE("95"), "The module major version doesn't match the kernel"};
            constexpr Diagnostic kFailBootList{BALEEN_STAGE_CODE("96"), "The module load list is invalid"};

            // 清单里登记的产物条目：结构检查时解析一次，逐产物核对与列表产物视图都按它走
            struct ArtifactRef {
                uint32_t index;             // 清单产物下标（0 起）
                uint8_t type;               // 1 = .os，2 = .osm，3 = Modules.list
                const uint8_t* path;        // 指向清单副本里的相对路径字节
                uint32_t pathBytes;         // 路径字节数
                uint64_t dataBytes;         // 清单登记的字节数
                const uint8_t* digest;      // 指向清单副本里的 SHA-256
            };

            // 列表里的一条预装条目：路径指向列表副本
            struct BootListEntry {
                const uint8_t* path;        // 相对路径起点（列表副本内）
                uint32_t pathBytes;         // 路径字节数
            };

            // 列表条目序列：解析一次，列表产物视图按它逐条命中清单的 .osm 条目
            BootListEntry s_bootListEntries[kBootListMaxEntries];

            // 按小端读取 16 位
            uint16_t Get16(const uint8_t* field) {
                return static_cast<uint16_t>(field[0] | (field[1] << 8));
            }

            // 按小端读取 32 位
            uint32_t Get32(const uint8_t* field) {
                uint32_t value = 0;
                for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(field[i]) << (8 * i);
                return value;
            }

            // 按小端读取 64 位
            uint64_t Get64(const uint8_t* field) {
                return static_cast<uint64_t>(Get32(field)) | (static_cast<uint64_t>(Get32(field + 4)) << 32);
            }

            // 拼接 path = directory + "/" + relative；相接处只保留一个 '/'
            bool JoinPath(const char* directory, const char* relative, uint32_t relativeBytes) {
                uint32_t length = 0;
                for (; directory[length] != '\0'; ++length) {
                    if (length + 1 >= kPathBytes) return false;
                    s_path[length] = directory[length];
                }
                if (length > 0 && s_path[length - 1] != '/') s_path[length++] = '/';
                for (uint32_t i = 0; i < relativeBytes; ++i) {
                    if (length + 1 >= kPathBytes) return false;
                    s_path[length++] = relative[i];
                }
                s_path[length] = '\0';
                return true;
            }

            // 拼接 path = directory + "/" + name（以空字符结尾的文件名）
            bool JoinName(const char* directory, const char* name) {
                return JoinPath(directory, name, static_cast<uint32_t>(strlen(name)));
            }

            // 从 offset 起读 want 字节到块缓冲；长度不足或读失败返回带编号的原因
            Diagnostic ReadChunk(Storage::FileSystem& fs, const Storage::FileId& id, uint32_t offset, uint32_t want) {
                uint32_t read = 0;
                if (const Storage::Error error = fs.Read(id, offset, s_chunk, want, read); error != Storage::Error::Ok) return kFailArtifactRead;
                if (read != want) return kFailArtifactRead;
                return Diagnostic{};
            }

            // 清单尾部签名区的结构检查与取用：算法、密钥编号与签名值
            bool CheckSignatureArea(const uint8_t* manifest, uint32_t bytes, uint8_t& keyId, const uint8_t*& signature) {
                const uint32_t begin = bytes - ML::kSignatureAreaBytes;
                if (manifest[begin + ML::kSignatureAlgorithm] != ML::kAlgorithmEd25519) return false;
                keyId = manifest[begin + ML::kSignatureKeyId];
                if (keyId == 0) return false;
                if (manifest[begin + ML::kSignatureReserved] != 0 || manifest[begin + ML::kSignatureReserved + 1] != 0) return false;
                signature = manifest + begin + ML::kSignatureValue;
                return true;
            }

            // 解析列表固定头三行：LPML 1 / major <十进制> / revision <十进制>
            // 固定头顺序与数值范围在解析时核对；
            // begin 返回条目区起点。失败表示列表结构非法（E-CORE-96）
            bool ParseBootListHeader(const uint8_t* data, uint32_t bytes, uint16_t& moduleMajor, uint64_t& revision, uint32_t& begin) {
                uint32_t pos = 0;
                const char kMagic[] = "LPML 1\n";
                for (uint32_t i = 0; kMagic[i] != '\0'; ++i, ++pos) {
                    if (pos >= bytes || data[pos] != static_cast<uint8_t>(kMagic[i])) return false;
                }
                const auto parseNumber = [&](const char* label, uint64_t limit, uint64_t& value) -> bool {
                    uint32_t length = 0;
                    for (; label[length] != '\0'; ++length, ++pos) {
                        if (pos >= bytes || data[pos] != static_cast<uint8_t>(label[length])) return false;
                    }
                    uint64_t number = 0;
                    uint32_t digits = 0;
                    while (pos < bytes && data[pos] >= '0' && data[pos] <= '9') {
                        const uint64_t digit = static_cast<uint64_t>(data[pos] - '0');
                        // 目标侧没有 64 位除法运行库：用乘十上界判溢出，不写除法
                        if (number > 0x1999999999999999ull) return false;
                        number = number * 10 + digit;
                        if (number > limit) return false;
                        ++digits;
                        ++pos;
                    }
                    if (digits == 0 || pos >= bytes || data[pos] != '\n') return false;
                    ++pos;
                    value = number;
                    return true;
                };
                uint64_t major = 0;
                if (!parseNumber("major ", 0xFFFF, major)) return false;
                if (major == 0) return false;
                if (!parseNumber("revision ", 0xFFFFFFFFFFFFFFFFull, revision)) return false;
                if (revision == 0) return false;
                moduleMajor = static_cast<uint16_t>(major);
                begin = pos;
                return true;
            }

            // 解析整份 Modules.list：固定头三行 + 0..30 条 module <相对路径> 条目
            // 格式规则：ASCII、LF、不得含 CR/NUL/空行/
            // 注释/行尾空白或额外字段，必须以 LF 结尾；路径非空、不超过 255 字节、不重复
            // 条目只是配置：路径与清单条目的逐字节匹配在列表产物视图里核对，不在这里猜
            bool ParseBootList(const uint8_t* data, uint32_t bytes, uint16_t& moduleMajor, uint64_t& revision, BootListEntry* entries, uint32_t& count) {
                uint32_t pos = 0;
                if (!ParseBootListHeader(data, bytes, moduleMajor, revision, pos)) return false;
                count = 0;
                while (pos < bytes) {
                    const char kModule[] = "module ";
                    for (uint32_t i = 0; kModule[i] != '\0'; ++i, ++pos) {
                        if (pos >= bytes || data[pos] != static_cast<uint8_t>(kModule[i])) return false;
                    }
                    const uint32_t pathBegin = pos;
                    while (pos < bytes && data[pos] != '\n') {
                        // 列表是 ASCII 配置：CR、NUL、制表符与行尾空白都不是合法路径字节
                        if (data[pos] == '\r' || data[pos] == 0 || data[pos] == '\t') return false;
                        if (data[pos] == ' ') return false;
                        ++pos;
                    }
                    if (pos >= bytes) return false;   // 必须以 LF 结尾
                    const uint32_t pathBytes = pos - pathBegin;
                    if (pathBytes == 0 || pathBytes > ML::kEntryPathMaxBytes) return false;
                    if (count >= kBootListMaxEntries) return false;
                    // 重复路径会让“一条条目对应一条清单产物”的对应关系失去唯一性
                    for (uint32_t earlier = 0; earlier < count; ++earlier) {
                        if (entries[earlier].pathBytes != pathBytes) continue;
                        if (memcmp(entries[earlier].path, data + pathBegin, pathBytes) == 0) return false;
                    }
                    entries[count].path = data + pathBegin;
                    entries[count].pathBytes = pathBytes;
                    ++count;
                    ++pos;
                }
                return count <= kBootListMaxEntries;
            }

            // 只核对 .osm 条目的最小读取面之外，不做别的事：容器内部由内核装载器判定
            // 列表产物视图只带清单登记的字节数与摘要，容器格式不在这里解释
            bool PathSame(const ArtifactRef& artifact, const BootListEntry& entry) {
                return artifact.pathBytes == entry.pathBytes && memcmp(artifact.path, entry.path, entry.pathBytes) == 0;
            }
        }

        Diagnostic VerifyKernelSet(Storage::FileSystem& fs, const char* setDirectory, const uint8_t expectedBuildId[32], uint64_t seenSecurityVersion, const Revocation::Facts& revocation, ManifestFacts& facts, const ManifestSource& source) {
            // 1. 定位清单：集目录 + 清单文件名（当前修订 Manifest.mft / 旧修订 Manifest.<rev>.mft）
            const char* manifestName = (source.manifestName != nullptr && source.manifestName[0] != '\0') ? source.manifestName : kManifestPath;
            if (!JoinName(setDirectory, manifestName)) return kFailManifestRead;
            Storage::FileId id;
            if (fs.Open(s_path, id) != Storage::Error::Ok) return kFailManifestRead;
            Storage::FileInfo info;
            if (fs.Stat(s_path, info) != Storage::Error::Ok) return kFailManifestRead;
            if (info.directory || info.bytes < ML::kHeaderBytes + ML::kSignatureAreaBytes || info.bytes > ML::kManifestMaxBytes) return kFailManifestFormat;
            const uint32_t bytes = static_cast<uint32_t>(info.bytes);
            uint32_t read = 0;
            if (fs.Read(id, 0, s_manifest, bytes, read) != Storage::Error::Ok || read != bytes) return kFailManifestRead;

            // 2. 结构与条目格式：头部固定值、长度一致、保留字段为零、条目字段与路径
            if (Get32(s_manifest + ML::kFieldMagic) != ML::kMagic) return kFailManifestFormat;
            if (Get16(s_manifest + ML::kFieldVersion) != ML::kVersion) return kFailManifestFormat;
            if (Get16(s_manifest + ML::kFieldHeaderBytes) != ML::kHeaderBytes) return kFailManifestFormat;
            if (Get32(s_manifest + ML::kFieldBytes) != bytes) return kFailManifestFormat;
            if (Get32(s_manifest + ML::kFieldFlags) != 0) return kFailManifestFormat;
            if (Get16(s_manifest + ML::kFieldReserved16) != 0 || Get32(s_manifest + ML::kFieldReserved32) != 0) return kFailManifestFormat;
            const uint16_t artifactCount = Get16(s_manifest + ML::kFieldArtifactCount);
            if (artifactCount == 0 || artifactCount > ML::kEntryMaxCount) return kFailManifestFormat;
            const uint32_t signatureBegin = bytes - ML::kSignatureAreaBytes;
            uint32_t offset = ML::kFieldArtifacts;
            uint32_t osCount = 0;
            uint32_t bootListCount = 0;
            uint32_t bootListEntryCount = 0;
            ArtifactRef artifacts[ML::kEntryMaxCount] = {};
            uint32_t artifactRefCount = 0;
            for (uint16_t i = 0; i < artifactCount; ++i) {
                if (offset + ML::kEntryFixedBytes > signatureBegin) return kFailManifestFormat;
                const uint8_t type = s_manifest[offset + ML::kEntryType];
                if (type != ML::kTypeOs && type != ML::kTypeOsm && type != ML::kTypeBootList) return kFailManifestFormat;
                if (type == ML::kTypeOs) ++osCount;
                const uint16_t pathBytes = Get16(s_manifest + offset + ML::kEntryPathBytes);
                if (pathBytes == 0 || pathBytes > ML::kEntryPathMaxBytes) return kFailManifestFormat;
                if (s_manifest[offset + ML::kEntryReserved] != 0) return kFailManifestFormat;
                // 模块加载列表固定放在集目录根、与 Manifest.mft 同级
                if (type == ML::kTypeBootList) {
                    ++bootListCount;
                    if (pathBytes != sizeof(kBootListPath) - 1
                        || memcmp(s_manifest + offset + ML::kEntryPath, kBootListPath, sizeof(kBootListPath) - 1) != 0) {
                        return kFailManifestFormat;
                    }
                }
                // 条目是集内相对路径：与布局路径共用同一份语法判据（禁止 . 与 ..）
                if (!LayoutCheck::PathValid(s_manifest + offset + ML::kEntryPath, pathBytes, false)) return kFailManifestFormat;
                // 路径互不重复：同一路径登记两次会让“签名覆盖全部产物”的语义变弱
                for (uint32_t seen = 0; seen < artifactRefCount; ++seen) {
                    if (artifacts[seen].pathBytes == pathBytes &&
                        memcmp(artifacts[seen].path, s_manifest + offset + ML::kEntryPath, pathBytes) == 0)
                        return kFailManifestFormat;
                }
                if (Get64(s_manifest + offset + ML::kEntryDataBytes) == 0) return kFailManifestFormat;
                artifacts[artifactRefCount].index = i;
                artifacts[artifactRefCount].type = type;
                artifacts[artifactRefCount].path = s_manifest + offset + ML::kEntryPath;
                artifacts[artifactRefCount].pathBytes = pathBytes;
                artifacts[artifactRefCount].dataBytes = Get64(s_manifest + offset + ML::kEntryDataBytes);
                artifacts[artifactRefCount].digest = s_manifest + offset + ML::kEntryDigest;
                ++artifactRefCount;
                offset += ML::kEntryFixedBytes + pathBytes;
            }
            if (offset != signatureBegin) return kFailManifestFormat;
            if (osCount != 1) return kFailManifestFormat;
            if (bootListCount != 1) return kFailManifestFormat;

            // 3. 签名区结构：算法、密钥编号与保留字段；结构不符属于清单格式错误，不是验签失败
            uint8_t keyId = 0;
            const uint8_t* signature = nullptr;
            if (!CheckSignatureArea(s_manifest, bytes, keyId, signature)) return kFailManifestFormat;

            // 4. 签名与吊销：签名值先取出，签名区按零代入后覆盖整份清单
            uint8_t signatureValue[Crypto::kEd25519SignatureBytes];
            memcpy(signatureValue, signature, sizeof(signatureValue));
            // 吊销先于查表：被吊销是显式策略结论，比“不在表里”更能说明现场
            if (Revocation::IsKeyRevoked(revocation.revokedKeyMask, keyId)) return kFailKeyRevoked;
            const uint8_t* publicKey = Crypto::FindTrustAnchor(keyId);
            if (publicKey == nullptr) return kFailManifestKey;
            memset(s_manifest + signatureBegin + ML::kSignatureValue, 0, ML::kSignatureValueBytes);
            Digest::Sha256 digest;
            digest.Begin();
            digest.Update(s_manifest, bytes);
            digest.Finish(facts.manifestDigest);
            if (!Crypto::Ed25519Verify(publicKey, signatureValue, s_manifest, bytes)) return kFailManifestSignature;

            // 5. 集身份一致：验签通过后才比较 BuildId，否则被改过的标识会掩盖真正的验签失败
            uint8_t buildIdAny = 0;
            for (uint32_t i = 0; i < ML::kBuildIdBytes; ++i) buildIdAny |= s_manifest[ML::kFieldBuildId + i];
            if (buildIdAny == 0) return kFailManifestFormat;
            if (memcmp(s_manifest + ML::kFieldBuildId, expectedBuildId, ML::kBuildIdBytes) != 0) return kFailManifestBuildId;

            // 6. 防回滚：SecurityVersion 不低于内嵌下限、设备已见最高与吊销记录给出的下限
            const uint64_t securityVersion = Get64(s_manifest + ML::kFieldSecurityVersion);
            if (securityVersion < Crypto::kMinimumSecurityVersion) return kFailRollback;
            if (securityVersion < seenSecurityVersion) return kFailRollback;
            if (securityVersion < revocation.minimumSecurityVersion) return kFailRollback;

            // 7. 逐产物：路径合法已在结构检查里核对；这里核对存在、长度与摘要
            uint16_t kernelModuleMajor = 0;
            uint16_t bootListMajor = 0;
            uint64_t bootListRevision = 0;
            uint32_t bootListBytes = 0;
            for (uint32_t i = 0; i < artifactRefCount; ++i) {
                const ArtifactRef& artifact = artifacts[i];
                if (!JoinPath(setDirectory, reinterpret_cast<const char*>(artifact.path), artifact.pathBytes)) return kFailManifestFormat;
                // 旧修订快照的列表文件名与清单里登记的名字不同：清单登记的是 Type=3 的逻辑路径，
                // 快照按 source.bootListName 取文件；清单摘要覆盖的仍是列表内容的摘要
                if (artifact.type == ML::kTypeBootList && source.bootListName != nullptr && source.bootListName[0] != '\0') {
                    if (!JoinName(setDirectory, source.bootListName)) return kFailManifestFormat;
                }
                if (fs.Open(s_path, id) != Storage::Error::Ok) return kFailArtifactRead;
                if (fs.Stat(s_path, info) != Storage::Error::Ok) return kFailArtifactRead;
                if (info.directory || info.bytes != artifact.dataBytes) return kFailArtifactRead;
                const uint32_t fileBytes = static_cast<uint32_t>(artifact.dataBytes);
                Digest::Sha256 artifactDigest;
                artifactDigest.Begin();
                for (uint32_t chunk = 0; chunk < fileBytes; chunk += kBounceBytes) {
                    const uint32_t want = (fileBytes - chunk) < kBounceBytes ? (fileBytes - chunk) : kBounceBytes;
                    if (const Diagnostic reason = ReadChunk(fs, id, chunk, want); reason.code != nullptr) return reason;
                    artifactDigest.Update(s_chunk, want);
                }
                uint8_t actual[Digest::kSha256Bytes];
                artifactDigest.Finish(actual);
                if (memcmp(actual, artifact.digest, sizeof(actual)) != 0) return kFailArtifactDigest;
                // 模块加载列表：核对固定格式、条目序列与模块主版本；主版本是硬门，跨主版本不承诺兼容
                if (artifact.type == ML::kTypeBootList) {
                    if (artifact.dataBytes > kBootListMaxBytes) return kFailBootList;
                    const uint32_t listBytes = static_cast<uint32_t>(artifact.dataBytes);
                    if (listBytes > kBootListMaxBytes) return kFailBootList;
                    uint32_t listRead = 0;
                    if (fs.Read(id, 0, s_bootList, listBytes, listRead) != Storage::Error::Ok || listRead != listBytes) return kFailArtifactRead;
                    if (!ParseBootList(s_bootList, listBytes, bootListMajor, bootListRevision, s_bootListEntries, bootListEntryCount)) return kFailBootList;
                    bootListBytes = listBytes;
                }
                // 集内一致：内核镜像的 BuildId 必须等于清单 BuildId（读首块核对数据字段）
                if (artifact.type == ML::kTypeOs) {
                    const uint32_t headBytes = fileBytes < Kernel::ImageDataLayout::kBytes ? fileBytes : Kernel::ImageDataLayout::kBytes;
                    if (const Diagnostic reason = ReadChunk(fs, id, 0, headBytes); reason.code != nullptr) return reason;
                    Kernel::ImageFacts kernelFacts;
                    if (const char* reason = Kernel::CheckImageData(s_chunk, fileBytes, kernelFacts)) return Diagnostic{kFailKernelData.code, reason};
                    if (memcmp(s_chunk + Kernel::ImageDataLayout::kFieldBuildId, s_manifest + ML::kFieldBuildId, ML::kBuildIdBytes) != 0) return kFailManifestBuildId;
                    kernelModuleMajor = kernelFacts.moduleMajor;
                    // 装载路径只认这一条：把清单登记的 .os 相对路径带回，选择器不得另行拼接固定文件名
                    memcpy(facts.kernelPath, artifact.path, artifact.pathBytes);
                    facts.kernelPath[artifact.pathBytes] = 0;
                    facts.kernelPathBytes = static_cast<uint16_t>(artifact.pathBytes);
                }
            }

            // 模块主版本硬门：清单声明的发布线必须同时等于 .os 与 Modules.list 里的主版本
            // 即使 ABI/接口范围碰巧匹配，跨主版本也一律拒绝装载
            if (kernelModuleMajor == 0 || bootListMajor == 0) return kFailManifestFormat;
            if (bootListMajor != kernelModuleMajor) return kFailModuleMajor;

            // 8. 列表产物视图：列表条目按逐字节路径命中清单的 .osm 条目，命中的产物下标、长度与摘要
            // 就是预装实现要核对的清单事实；列表条目不在清单里、或命中的不是 .osm，按列表非法拒绝
            // 列表是完整模块加载列表：非闭包条目只记录清单事实，是否预装由预装实现按期望集筛
            uint32_t listedCount = 0;
            for (uint32_t entry = 0; entry < bootListEntryCount; ++entry) {
                bool found = false;
                for (uint32_t candidate = 0; candidate < artifactRefCount; ++candidate) {
                    if (artifacts[candidate].type != ML::kTypeOsm) continue;
                    if (!PathSame(artifacts[candidate], s_bootListEntries[entry])) continue;
                    ListedArtifact& listed = s_listedArtifacts[listedCount];
                    listed.artifactIndex = artifacts[candidate].index;
                    listed.pathBytes = s_bootListEntries[entry].pathBytes;
                    listed.dataBytes = artifacts[candidate].dataBytes;
                    memcpy(listed.path, s_bootListEntries[entry].path, s_bootListEntries[entry].pathBytes);
                    listed.path[s_bootListEntries[entry].pathBytes] = 0;
                    memcpy(listed.digest, artifacts[candidate].digest, ML::kEntryDigestBytes);
                    ++listedCount;
                    found = true;
                    break;
                }
                if (!found) return kFailBootList;
            }

            // 9. 带回事实：清单副本（签名区已归零）、列表副本与列表产物视图都在本文件的静态存储里，
            // 交权或本次选版期间保持有效
            memcpy(facts.buildId, s_manifest + ML::kFieldBuildId, ML::kBuildIdBytes);
            facts.securityVersion = securityVersion;
            facts.bootListRevision = bootListRevision;
            facts.moduleMajor = kernelModuleMajor;
            facts.artifactCount = artifactCount;
            facts.manifestBytes = bytes;
            facts.manifestCopy = s_manifest;
            facts.bootListCopy = s_bootList;
            facts.bootListBytes = bootListBytes;
            facts.listedArtifacts = s_listedArtifacts;
            facts.listedArtifactCount = listedCount;
            return Diagnostic{};
        }

        bool ReadBootListRevision(Storage::FileSystem& fs, const char* setDirectory, uint64_t& revision) {
            if (!JoinName(setDirectory, kBootListPath)) return false;
            Storage::FileId id;
            if (fs.Open(s_path, id) != Storage::Error::Ok) return false;
            Storage::FileInfo info;
            if (fs.Stat(s_path, info) != Storage::Error::Ok) return false;
            if (info.directory || info.bytes == 0 || info.bytes > kBootListMaxBytes) return false;
            const uint32_t headBytes = info.bytes < kBootListHeadBytes ? static_cast<uint32_t>(info.bytes) : kBootListHeadBytes;
            uint32_t read = 0;
            if (fs.Read(id, 0, s_bootListHead, headBytes, read) != Storage::Error::Ok || read != headBytes) return false;
            uint16_t moduleMajor = 0;
            uint32_t begin = 0;
            return ParseBootListHeader(s_bootListHead, headBytes, moduleMajor, revision, begin);
        }
    }
}
