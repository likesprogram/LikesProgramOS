/* main.cpp
    CheckIso9660：内核 ISO9660 模块只读实现与 FileSystemOps 九槽表的宿主侧检查，手工执行

    检查对象是模块将要登记的那份源码：Packages/KernelModules/ISO9660/Src/Iso9660Volume.cpp、
    Iso9660Ops.cpp 与 Module.cpp，用 vpath 按相对路径直接编进本工具，不做第二份参考实现
    夹具用 xorriso 的 mkisofs 兼容模式生成带 Rock Ridge 的真实 ISO9660 卷，再按契约层
    BlockDeviceOps 的形状构造内存块设备，经 FileSystemOps 表挂载、查询与读取：
    PVD 与卷标识 / 根目录与子目录 / Rock Ridge 长名 / 大小写与 ";1" 版本形式命中 /
    跨块文件读取 / 文件尾 / 坏结构拒绝 / 读盘失败透传 / 模块 Init 登记路径
    缺 xorriso 时按基础设施缺失报告（退出码 2）；用例数固定，失败返回 1，成功打印计数
*/

#include <Iso9660Ops.hpp>
#include <Iso9660Volume.hpp>

#include <ModuleAbi.hpp>

#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
    namespace fs = std::filesystem;
    using Bytes = std::vector<uint8_t>;
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 21;
    // 已跑用例数
    unsigned g_ran = 0;
    // 夹具卷参数
    constexpr const char* kVolumeId = "TESTVOL";
    constexpr uint64_t kSystemVolumeBytes = 16 * 1024 * 1024;   // ISO 内系统卷镜像长度
    constexpr uint64_t kLongNameBytes = 12345;                  // 奇数长度，便于测非整块读取
    constexpr uint64_t kModuleBytes = 5000;                     // 子目录里的模块文件
    // 夹具文件在 ISO 内的路径（Rock Ridge NM 保留真实名字）
    constexpr const char* kSystemVolumePath = "/SystemVolume.img";
    constexpr const char* kHelloPath = "/Hello.txt";
    constexpr const char* kLongNamePath = "/LongNameFile.bin";
    constexpr const char* kSubDir = "/sub";
    constexpr const char* kModulePath = "/sub/Module.osm";
    // PVD 位置与字段偏移
    constexpr uint64_t kPvdOffset = 16 * 2048;
    constexpr uint32_t kPvdLogicalBlock = 0x80;
    constexpr uint32_t kPvdVolumeBlocks = 0x50;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }
    // 环境变量取值
    std::string Env(const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return value && *value ? value : fallback;
    }
    // 读整个文件
    Bytes ReadFile(const fs::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        Require(bool(input), "无法读取 " + path.string());
        const auto size = input.tellg();
        Require(size >= 0, "文件大小异常 " + path.string());
        input.seekg(0);
        Bytes data(static_cast<std::size_t>(size));
        if (!data.empty()) input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        Require(bool(input), "读取失败 " + path.string());
        return data;
    }
    // 写整个文件
    void WriteFile(const fs::path& path, const uint8_t* data, std::size_t bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(bool(output), "无法写入 " + path.string());
        if (bytes != 0) output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        output.close();
        Require(bool(output), "写入失败 " + path.string());
    }
    // 确定性伪随机内容
    Bytes Pattern(std::size_t bytes, uint32_t seed) {
        Bytes data(bytes);
        uint32_t state = seed * 2654435761u + 1u;
        for (std::size_t i = 0; i < bytes; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            data[i] = static_cast<uint8_t>(state & 0xFF);
        }
        return data;
    }
    // 跑一条外部命令，输出重定向到日志
    int Run(const std::vector<std::string>& command, const fs::path& log) {
        const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        Require(fd >= 0, "无法创建日志 " + log.string());
        const pid_t pid = ::fork();
        if (pid < 0) { ::close(fd); Fail("fork 失败"); }
        if (pid == 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
            std::vector<char*> argv;
            argv.reserve(command.size() + 1);
            for (const auto& argument : command) argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }
        ::close(fd);
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid) Fail("等待子进程失败");
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
    }
    // 打印日志尾部
    void ShowLogTail(const fs::path& log, unsigned lines) {
        std::ifstream input(log);
        std::vector<std::string> all;
        std::string line;
        while (std::getline(input, line)) all.push_back(line);
        const std::size_t first = all.size() > lines ? all.size() - lines : 0;
        for (std::size_t i = first; i < all.size(); ++i) std::cerr << "  | " << all[i] << std::endl;
    }

    // 内存块设备：按契约层 BlockDeviceOps 的读侧形状提供读原语
    struct Medium {
        Bytes bytes;                 // 介质内容（ISO 镜像）
        uint32_t blockBytes = 2048;  // 逻辑块大小，2 的幂
        bool failRead = false;       // 注入读失败
    };

    // BlockDeviceOps 槽 0：逻辑块大小
    uint32_t DeviceBlockBytes(void* context) { return static_cast<Medium*>(context)->blockBytes; }
    // BlockDeviceOps 槽 1：容量
    uint64_t DeviceCapacityBytes(void* context) { return static_cast<Medium*>(context)->bytes.size(); }
    // BlockDeviceOps 槽 2：可写性（只读）
    uint32_t DeviceWritable(void* context) { (void)context; return 0; }
    // BlockDeviceOps 槽 3：读
    Storage::Error DeviceRead(void* context, uint64_t offset, void* buffer, uint32_t bytes) {
        Medium* medium = static_cast<Medium*>(context);
        if (medium->failRead) return Storage::Error::Io;
        if (offset > medium->bytes.size() || bytes > medium->bytes.size() - offset) return Storage::Error::InvalidArgument;
        std::memcpy(buffer, medium->bytes.data() + offset, bytes);
        return Storage::Error::Ok;
    }
    // BlockDeviceOps 槽 4：写（只读设备拒绝）
    Storage::Error DeviceWrite(void* context, uint64_t offset, const void* buffer, uint32_t bytes) {
        (void)context; (void)offset; (void)buffer; (void)bytes;
        return Storage::Error::ReadOnly;
    }
    // BlockDeviceOps 槽 5：刷新
    Storage::Error DeviceFlush(void* context) { (void)context; return Storage::Error::Ok; }

    // 设备调用表：契约层 BlockDeviceOps v1
    const BlockDeviceOps kDeviceOps = {0, DeviceBlockBytes, DeviceCapacityBytes, DeviceWritable, DeviceRead, DeviceWrite, DeviceFlush};

    // 检查对象：模块将要登记的 FileSystemOps 表与 context
    const FileSystemOps* Ops() { return KernelModules::Iso9660::FileSystemOpsTable(); }
    void* Context() { return KernelModules::Iso9660::FileSystemContext(); }

    // 按只读模式挂载一份介质
    Storage::Error MountMedium(Medium& medium, uint32_t mode = 0) {
        return Ops()->Mount(Context(), &kDeviceOps, &medium, mode);
    }

    // 夹具目录与基准 ISO 镜像
    fs::path g_work;
    fs::path g_root;
    Bytes g_iso;
    Bytes g_systemVolume;
    Bytes g_hello;
    Bytes g_longName;
    Bytes g_module;

    // 生成夹具：写文件树，用 xorriso 的 mkisofs 兼容模式产出带 Rock Ridge 的 ISO
    bool MakeFixture() {
        const std::string xorriso = Env("XORRISO", "xorriso");
        g_root = g_work / "root";
        fs::create_directories(g_root / "sub");
        g_systemVolume = Pattern(kSystemVolumeBytes, 1);
        g_hello = Pattern(100, 2);
        g_longName = Pattern(kLongNameBytes, 3);
        g_module = Pattern(kModuleBytes, 4);
        WriteFile(g_root / "SystemVolume.img", g_systemVolume.data(), g_systemVolume.size());
        WriteFile(g_root / "Hello.txt", g_hello.data(), g_hello.size());
        WriteFile(g_root / "LongNameFile.bin", g_longName.data(), g_longName.size());
        WriteFile(g_root / "sub" / "Module.osm", g_module.data(), g_module.size());
        const fs::path iso = g_work / "volume.iso";
        // -R 打开 Rock Ridge（NM 保留真实名字）；-V 指定卷标识
        const int code = Run({xorriso, "-as", "mkisofs", "-quiet", "-R", "-V", kVolumeId, "-o", iso.string(), g_root.string()},
                             g_work / "xorriso.log");
        if (code != 0) {
            std::cerr << "CheckIso9660: xorriso 失败（退出码 " << code << "）" << std::endl;
            ShowLogTail(g_work / "xorriso.log", 8);
            return false;
        }
        g_iso = ReadFile(iso);
        return !g_iso.empty();
    }

    // 挂载基准 ISO（可先做副本再改字节）
    Storage::Error MountVolume(const Bytes& iso, Medium& medium, uint32_t mode = 0) {
        medium.bytes = iso;
        return MountMedium(medium, mode);
    }

    // 按路径读整个文件并与期望字节比对
    void ExpectFile(const char* path, const Bytes& expected) {
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), path, &info) == Storage::Error::Ok, std::string("Stat 应成功：") + path);
        Require(info.bytes == expected.size() && !info.directory, std::string("文件事实不符：") + path);
        uint64_t object = 0;
        Require(Ops()->Open(Context(), path, &object) == Storage::Error::Ok, std::string("Open 应成功：") + path);
        Bytes buffer(expected.size() + 16, 0xEE);
        uint32_t read = 0;
        Require(Ops()->Read(Context(), object, 0, buffer.data(), static_cast<uint32_t>(buffer.size()), &read) == Storage::Error::Ok,
            std::string("Read 应成功：") + path);
        Require(read == expected.size(), std::string("读取长度不符：") + path);
        Require(std::memcmp(buffer.data(), expected.data(), expected.size()) == 0, std::string("读取内容不符：") + path);
        uint32_t tail = 0;
        Require(Ops()->Read(Context(), object, expected.size(), buffer.data(), 16, &tail) == Storage::Error::Ok && tail == 0,
            std::string("文件尾之后应读到 0：") + path);
    }

    // 列一个目录的全部目录项
    std::map<std::string, Storage::FileInfo> ListDirectory(const char* path) {
        uint64_t object = 0;
        Require(Ops()->Open(Context(), path, &object) == Storage::Error::Ok, std::string("Open 应成功：") + path);
        std::map<std::string, Storage::FileInfo> entries;
        uint64_t cursor = 0;
        for (;;) {
            char name[256] = {};
            uint32_t nameLength = 0;
            Storage::FileInfo info{};
            const Storage::Error error = Ops()->ReadDirectory(Context(), object, &cursor, name, sizeof(name), &nameLength, &info);
            if (error == Storage::Error::NotFound) break;
            Require(error == Storage::Error::Ok, std::string("ReadDirectory 应成功：") + path);
            Require(nameLength > 0 && name[nameLength] == '\0', "目录项名字应以空字符结尾");
            const std::string entry(name, nameLength);
            Require(entry != "." && entry != "..", "目录遍历不得交出 . 与 ..");
            Require(entries.find(entry) == entries.end(), "目录项不应重复：" + entry);
            entries.emplace(entry, info);
        }
        return entries;
    }

    // 写 ISO 镜像里的 16 位字段（小端）
    void Put16(Bytes& data, std::size_t at, uint16_t value) {
        Require(at + 2 <= data.size(), "夹具字段越界");
        data[at] = static_cast<uint8_t>(value);
        data[at + 1] = static_cast<uint8_t>(value >> 8);
    }
    // 写 ISO 镜像里的 32 位字段（小端）
    void Put32(Bytes& data, std::size_t at, uint32_t value) {
        Require(at + 4 <= data.size(), "夹具字段越界");
        for (uint32_t i = 0; i < 4; ++i) data[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // —— 正例：挂载、事实与读取 ——

    // 格式名与能力位：只读、可列目录、不跟随符号链接、名字 ASCII 折叠
    void CaseFormatAndCaps() {
        Require(std::strcmp(Ops()->Format(Context()), "iso9660") == 0, "格式名应为 iso9660");
        Storage::FileSystemCaps caps{};
        Ops()->Caps(Context(), &caps);
        Require(!caps.writable && caps.directoryListing && !caps.symlinks, "能力位应为只读、可列目录、不跟随符号链接");
        Require(caps.nameCase == Storage::NameCase::Insensitive, "ISO9660 名字比较应为 ASCII 折叠");
    }

    // 未挂载状态：查询与卷标识都返回参数非法
    void CaseNotMounted() {
        Ops()->Unmount(Context());
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::InvalidArgument, "未挂载时 Stat 应返回参数非法");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/", &object) == Storage::Error::InvalidArgument, "未挂载时 Open 应返回参数非法");
        uint8_t uuid[16] = {};
        Require(Ops()->VolumeIdentity(Context(), uuid) == Storage::Error::InvalidArgument, "未挂载时卷标识应返回参数非法");
    }

    // 挂载事实：根目录可解析且是目录
    void CaseMountFacts() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t root = 0;
        Require(Ops()->Open(Context(), "/", &root) == Storage::Error::Ok, "根路径应可解析");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::Ok && info.directory && info.bytes == 0, "根目录事实不符");
        Ops()->Unmount(Context());
    }

    // 卷标识：主卷描述符的 Volume Identifier 取前 16 字节
    void CaseVolumeIdentity() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        uint8_t identity[16] = {};
        Require(Ops()->VolumeIdentity(Context(), identity) == Storage::Error::Ok, "卷标识应成功");
        Require(std::memcmp(identity, kVolumeId, std::strlen(kVolumeId)) == 0, "卷标识前段应与 -V 指定的一致");
        Require(identity[std::strlen(kVolumeId)] == 0, "卷标识其余字节应补零");
        Require(Ops()->VolumeIdentity(Context(), nullptr) == Storage::Error::InvalidArgument, "空输出缓冲应拒绝");
        Ops()->Unmount(Context());
    }

    // 读写挂载、空设备表、非法逻辑块大小都拒绝
    void CaseRejectInvalidMount() {
        Medium medium;
        medium.bytes = g_iso;
        Require(MountMedium(medium, static_cast<uint32_t>(Storage::MountMode::ReadWrite)) == Storage::Error::NotSupported, "读写挂载应拒绝");
        Require(Ops()->Mount(Context(), nullptr, nullptr, 0) == Storage::Error::InvalidArgument, "空设备表应拒绝");
        Medium odd;
        odd.bytes = g_iso;
        odd.blockBytes = 500;
        Require(MountMedium(odd) == Storage::Error::InvalidArgument, "非 2 的幂逻辑块大小应拒绝");
        BlockDeviceOps broken = kDeviceOps;
        broken.Read = nullptr;
        Require(Ops()->Mount(Context(), &broken, &medium, 0) == Storage::Error::InvalidArgument, "缺读原语的设备表应拒绝");
    }

    // 根目录项集合：Rock Ridge NM 给出真实名字
    void CaseRootListing() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        const auto entries = ListDirectory("/");
        std::string names;
        for (const auto& entry : entries) names += "[" + entry.first + "]";
        Require(entries.size() == 4, "根目录应有四个目录项，实际 " + std::to_string(entries.size()) + " " + names);
        Require(entries.count("SystemVolume.img") == 1, "根目录应含 SystemVolume.img（Rock Ridge 长名），实际 " + names);
        Require(entries.count("Hello.txt") == 1, "根目录应含 Hello.txt");
        Require(entries.count("LongNameFile.bin") == 1, "根目录应含 LongNameFile.bin");
        Require(entries.count("sub") == 1 && entries.at("sub").directory, "根目录应含目录 sub");
        Require(!entries.at("SystemVolume.img").directory && entries.at("SystemVolume.img").bytes == kSystemVolumeBytes, "系统卷目录项事实不符");
        Ops()->Unmount(Context());
    }

    // 名字命中规则：大小写折叠，且 ";1" 版本后缀两侧都可省略
    void CaseNameMatchingRules() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t exact = 0;
        uint64_t folded = 0;
        uint64_t identifier = 0;
        uint64_t identifierNoVersion = 0;
        Require(Ops()->Open(Context(), kSystemVolumePath, &exact) == Storage::Error::Ok, "真实名字应可打开");
        Require(Ops()->Open(Context(), "/systemvolume.img", &folded) == Storage::Error::Ok, "小写形式应命中同一项");
        Require(Ops()->Open(Context(), "/SYSTEMVO.IMG;1", &identifier) == Storage::Error::Ok, "ISO 标识符加版本应命中同一项");
        Require(Ops()->Open(Context(), "/systemvo.img", &identifierNoVersion) == Storage::Error::Ok, "ISO 标识符去版本折叠应命中同一项");
        Require(exact == folded && exact == identifier && exact == identifierNoVersion, "四种写法的对象标识必须一致");
        Ops()->Unmount(Context());
    }

    // 系统卷文件事实与内容（16 MiB，跨多个 ISO 块）
    void CaseReadSystemVolume() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        ExpectFile(kSystemVolumePath, g_systemVolume);
        // 抽样：中部与尾部
        uint64_t object = 0;
        Require(Ops()->Open(Context(), kSystemVolumePath, &object) == Storage::Error::Ok, "系统卷应可打开");
        const uint64_t offsets[] = {kSystemVolumeBytes / 2, kSystemVolumeBytes - 8192};
        for (const uint64_t offset : offsets) {
            uint8_t buffer[8192] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, offset, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == sizeof(buffer), "抽样读取应成功");
            Require(std::memcmp(buffer, g_systemVolume.data() + offset, sizeof(buffer)) == 0, "抽样内容不符");
        }
        Ops()->Unmount(Context());
    }

    // 文件尾与参数边界
    void CaseReadTailBoundary() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), kLongNamePath, &object) == Storage::Error::Ok, "文件应可打开");
        uint8_t buffer[4096] = {};
        uint32_t read = 0;
        Require(Ops()->Read(Context(), object, kLongNameBytes - 100, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 100,
            "尾部读取应按实际长度回报");
        Require(std::memcmp(buffer, g_longName.data() + (kLongNameBytes - 100), 100) == 0, "尾部内容不符");
        read = 0xFFFF;
        Require(Ops()->Read(Context(), object, kLongNameBytes + 1000, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 0,
            "越过文件尾应读到 0");
        Require(Ops()->Read(Context(), object, 0, nullptr, 16, &read) == Storage::Error::InvalidArgument, "空缓冲应拒绝");
        Require(Ops()->Read(Context(), object, 0, buffer, 0, &read) == Storage::Error::InvalidArgument, "零长度应拒绝");
        Ops()->Unmount(Context());
    }

    // Rock Ridge 长名：真实名字与 8.3 标识符都能命中
    void CaseRockRidgeLongName() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        ExpectFile(kLongNamePath, g_longName);
        // 8.3 收敛后的标识符（LongNameFile.bin -> LONGNAME.BIN;1）同样应命中
        uint64_t byIdentifier = 0;
        Require(Ops()->Open(Context(), "/LONGNAME.BIN;1", &byIdentifier) == Storage::Error::Ok, "8.3 标识符应命中同一项");
        uint64_t byRealName = 0;
        Require(Ops()->Open(Context(), kLongNamePath, &byRealName) == Storage::Error::Ok, "真实名字应可打开");
        Require(byIdentifier == byRealName, "两种名字形式必须指向同一项");
        Ops()->Unmount(Context());
    }

    // 子目录：遍历与读取
    void CaseSubdirectory() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        const auto entries = ListDirectory(kSubDir);
        Require(entries.size() == 1 && entries.count("Module.osm") == 1, "子目录应只含 Module.osm");
        Require(!entries.at("Module.osm").directory && entries.at("Module.osm").bytes == kModuleBytes, "子目录文件事实不符");
        ExpectFile(kModulePath, g_module);
        Ops()->Unmount(Context());
    }

    // 目录游标与名字缓冲：缓冲不足返回 NameTooLong 且不推进游标
    void CaseDirectoryNameTooLong() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/", &object) == Storage::Error::Ok, "根目录应可打开");
        uint64_t cursor = 0;
        char tiny[4] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info{};
        bool truncated = false;
        for (;;) {
            const uint64_t before = cursor;
            const Storage::Error error = Ops()->ReadDirectory(Context(), object, &cursor, tiny, sizeof(tiny), &nameLength, &info);
            if (error == Storage::Error::Ok) continue;
            Require(error == Storage::Error::NameTooLong, "名字缓冲不足应返回 NameTooLong");
            Require(cursor == before, "名字缓冲不足时游标不得推进");
            truncated = true;
            break;
        }
        Require(truncated, "应至少出现一次名字缓冲不足");
        char name[256] = {};
        Require(Ops()->ReadDirectory(Context(), object, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::Ok,
            "换大缓冲后应能继续遍历");
        Ops()->Unmount(Context());
    }

    // 读取面错误映射：对目录读返回 IsDirectory，对文件列目录返回 NotDirectory
    void CaseReadErrors() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t dir = 0;
        Require(Ops()->Open(Context(), kSubDir, &dir) == Storage::Error::Ok, "目录应可打开");
        uint8_t buffer[16] = {};
        uint32_t read = 0;
        Require(Ops()->Read(Context(), dir, 0, buffer, sizeof(buffer), &read) == Storage::Error::IsDirectory, "对目录读取应返回 IsDirectory");
        uint64_t file = 0;
        Require(Ops()->Open(Context(), kHelloPath, &file) == Storage::Error::Ok, "文件应可打开");
        char name[32] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info{};
        uint64_t cursor = 0;
        Require(Ops()->ReadDirectory(Context(), file, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::NotDirectory,
            "对文件列目录应返回 NotDirectory");
        Ops()->Unmount(Context());
    }

    // 路径错误映射
    void CasePathErrors() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/nope", &object) == Storage::Error::NotFound, "不存在的路径应返回 NotFound");
        Require(Ops()->Open(Context(), "Hello.txt", &object) == Storage::Error::InvalidArgument, "非绝对路径应返回参数非法");
        Require(Ops()->Open(Context(), "/Hello.txt/", &object) == Storage::Error::InvalidArgument, "尾斜杠应返回参数非法");
        Require(Ops()->Open(Context(), "/./Hello.txt", &object) == Storage::Error::InvalidArgument, "含 . 的路径应返回参数非法");
        Require(Ops()->Open(Context(), "/sub/../Hello.txt", &object) == Storage::Error::InvalidArgument, "含 .. 的路径应返回参数非法");
        Require(Ops()->Open(Context(), "/Hello.txt/x", &object) == Storage::Error::NotDirectory, "中间项不是目录应返回 NotDirectory");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/nope", &info) == Storage::Error::NotFound, "Stat 应传播 NotFound");
        Ops()->Unmount(Context());
    }

    // 坏主卷描述符：CD001 不符按结构非法拒绝
    void CaseRejectBadPvd() {
        Bytes damaged = g_iso;
        damaged[kPvdOffset + 1] = 'X';
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::Corrupt, "CD001 不符应返回 Corrupt");
    }

    // 逻辑块大小不是 2048：明确拒绝而不是按 512/1024 猜
    void CaseRejectForeignBlockSize() {
        Bytes damaged = g_iso;
        Put16(damaged, kPvdOffset + kPvdLogicalBlock, 1024);
        Put16(damaged, kPvdOffset + kPvdLogicalBlock + 2, 1024);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "非 2048 逻辑块应返回 NotSupported");
    }

    // 双端序字段不一致：结构被破坏，拒绝
    void CaseRejectEndianMismatch() {
        Bytes damaged = g_iso;
        Put32(damaged, kPvdOffset + kPvdVolumeBlocks + 4, 1);   // 只改大端副本
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::Corrupt, "双端序不一致应返回 Corrupt");
    }

    // 读盘失败透传：挂载期与读取期都返回 Io
    void CaseReadFailurePropagates() {
        {
            Medium medium;
            medium.bytes = g_iso;
            medium.failRead = true;
            Require(MountMedium(medium) == Storage::Error::Io, "挂载期读失败应返回 Io");
        }
        {
            Medium medium;
            Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "挂载应成功");
            uint64_t object = 0;
            Require(Ops()->Open(Context(), kHelloPath, &object) == Storage::Error::Ok, "文件应可打开");
            medium.failRead = true;
            uint8_t buffer[512] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, 0, buffer, sizeof(buffer), &read) == Storage::Error::Io, "读文件失败应返回 Io");
            Ops()->Unmount(Context());
        }
    }

    // 512 字节设备逻辑块：按设备块拼装读取
    void CaseDeviceBlock512() {
        Medium medium;
        medium.blockBytes = 512;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "512 字节逻辑块设备上挂载应成功");
        ExpectFile(kHelloPath, g_hello);
        Ops()->Unmount(Context());
    }

    // 卸载与重挂
    void CaseRemount() {
        Medium medium;
        Require(MountVolume(g_iso, medium) == Storage::Error::Ok, "首次挂载应成功");
        ExpectFile(kHelloPath, g_hello);
        Ops()->Unmount(Context());
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::InvalidArgument, "卸载后 Stat 应返回参数非法");
        Require(MountMedium(medium) == Storage::Error::Ok, "重挂应成功");
        Require(Ops()->Stat(Context(), kHelloPath, &info) == Storage::Error::Ok && info.bytes == g_hello.size(), "重挂后事实应恢复");
        Ops()->Unmount(Context());
    }

    // 模块 Init 的登记路径：用替身 Fs.Vfs 回填区调用入口，核对参数与失败分类
    void CaseModuleInitRegisters() {
        static const FileSystemOps* registeredOps = nullptr;
        static void* registeredContext = nullptr;
        static FileSystemId registeredId = 0;
        _Contract_Import_FsVfs_v1.RegisterFileSystem = +[](const FileSystemOps* ops, void* context, FileSystemId* id) -> Storage::Error {
            registeredOps = ops;
            registeredContext = context;
            *id = 9;
            registeredId = *id;
            return Storage::Error::Ok;
        };
        KernelAbiContext context{};
        context.bytes = sizeof(KernelAbiContext);
        context.version = 1;
        context.kernelAbiVersion = 1;
        context.moduleBase = 0;
        context.moduleId = kModISO9660;
        Require(_Module_Init(&context) == KernelError::kOk, "Init 应成功");
        Require(registeredOps == KernelModules::Iso9660::FileSystemOpsTable(), "登记应使用模块的 FileSystemOps 表");
        Require(registeredContext == KernelModules::Iso9660::FileSystemContext(), "登记应使用模块内静态卷实例");
        Require(registeredId == 9, "登记标识应来自 Fs.Vfs 的返回");
        Require(_Module_Init(nullptr) == KernelError::kInvalidArgument, "空上下文应拒绝");
        KernelAbiContext wrongId = context;
        wrongId.moduleId = kModExt4;
        Require(_Module_Init(&wrongId) == KernelError::kCorrupt, "ModuleId 不符应拒绝");
        _Contract_Import_FsVfs_v1.RegisterFileSystem = nullptr;
        Require(_Module_Init(&context) == KernelError::kState, "导入未回填应返回 kState");
        Require(_Module_Abort(&context) == KernelError::kOk, "Abort 应为空操作");
    }

    // 跑一个用例
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckIso9660: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 依次跑全部用例
    void Execute() {
        Case("格式名与能力位", CaseFormatAndCaps);
        Case("未挂载状态的拒绝", CaseNotMounted);
        Case("挂载事实", CaseMountFacts);
        Case("卷标识", CaseVolumeIdentity);
        Case("拒绝非法挂载参数", CaseRejectInvalidMount);
        Case("根目录项集合（Rock Ridge 长名）", CaseRootListing);
        Case("名字命中规则（折叠与版本后缀）", CaseNameMatchingRules);
        Case("读系统卷镜像", CaseReadSystemVolume);
        Case("文件尾与参数边界", CaseReadTailBoundary);
        Case("Rock Ridge 长名与 8.3 标识符", CaseRockRidgeLongName);
        Case("子目录遍历与读取", CaseSubdirectory);
        Case("目录名字缓冲不足", CaseDirectoryNameTooLong);
        Case("读取面错误映射", CaseReadErrors);
        Case("路径错误映射", CasePathErrors);
        Case("拒绝坏主卷描述符", CaseRejectBadPvd);
        Case("拒绝非 2048 逻辑块", CaseRejectForeignBlockSize);
        Case("拒绝双端序字段不一致", CaseRejectEndianMismatch);
        Case("读盘失败透传", CaseReadFailurePropagates);
        Case("512 字节逻辑块设备", CaseDeviceBlock512);
        Case("卸载与重挂", CaseRemount);
        Case("模块 Init 登记路径", CaseModuleInitRegisters);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckIso9660: " << g_ran << "/" << kCaseCount << " cases passed" << std::endl;
    }
}

// 入口：解析选项、生成夹具并跑全部用例
int main(int argc, char** argv) {
    fs::path work;
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout <<
                "CheckIso9660 — 内核 ISO9660 只读模块与 FileSystemOps 九槽表的宿主侧检查\n"
                "\n"
                "用法：CheckIso9660 [--work <目录>] [--keep]\n"
                "\n"
                "  --work <目录>  工作目录（夹具与临时文件），默认建在系统临时目录下\n"
                "  --keep         无论成败都保留工作目录\n"
                "环境变量 XORRISO 指定 ISO 生成工具（默认 xorriso）；缺工具返回 2\n";
            return 0;
        }
        if (arg == "--work") {
            if (i + 1 >= argc) { std::cerr << "CheckIso9660: --work 缺少参数\n"; return 2; }
            work = argv[++i];
        } else if (arg == "--keep") {
            keep = true;
        } else {
            std::cerr << "CheckIso9660: 无法识别的参数：" << arg << '\n';
            return 2;
        }
    }
    try {
        if (work.empty()) {
            char pattern[] = "/tmp/checkiso9660.XXXXXX";
            const char* created = ::mkdtemp(pattern);
            Require(created != nullptr, "无法创建临时工作目录");
            work = created;
        }
        fs::create_directories(work);
        g_work = work;
        if (!MakeFixture()) {
            std::cerr << "CheckIso9660: 夹具生成失败（基础设施缺失）" << std::endl;
            if (!keep) fs::remove_all(work);
            return 2;
        }
        Execute();
        if (!keep) fs::remove_all(work);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckIso9660: " << error.what() << '\n';
        if (!work.empty() && !keep) fs::remove_all(work);
        return 1;
    }
}
