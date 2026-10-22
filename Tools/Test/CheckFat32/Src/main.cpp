/* main.cpp
    CheckFat32：内核 FAT32File 模块只读实现与 FileSystemOps 九槽表的宿主侧检查，手工执行

    检查对象是模块将要登记的那份源码：Packages/KernelModules/FAT32File/Src/Fat32Volume.cpp、
    Fat32Ops.cpp 与 Module.cpp，用 vpath 按相对路径直接编进本工具，不做第二份参考实现
    夹具用 mkfs.vfat 生成 256 MiB 的真实 FAT32 卷（-n TESTVOL -i 12345678），再用 mtools
    （mmd / mcopy）放入文件树；块设备按契约层 BlockDeviceOps 的形状用内存实现注入，
    挂载、查询与读取都经 FileSystemOps 表完成：
    BPB 与卷标识 / 根目录与子目录 / LFN 与 8.3 命中 / 跨簇文件 / 文件尾 / FAT 链损坏拒绝 /
    只读拒绝 / 读盘失败透传 / 模块 Init 登记路径
    缺 mkfs.vfat 或 mtools 时按基础设施缺失报告（退出码 2）；用例数固定，失败返回 1
*/

#include <Fat32Ops.hpp>
#include <Fat32Volume.hpp>

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
    constexpr unsigned kCaseCount = 20;
    // 已跑用例数
    unsigned g_ran = 0;
    // 夹具卷参数
    constexpr const char* kVolumeLabel = "TESTVOL";
    constexpr uint32_t kVolumeSerial = 0x12345678;
    constexpr uint64_t kImageBytes = 256ull * 1024 * 1024;
    constexpr uint64_t kHelloBytes = 100;
    constexpr uint64_t kLongNameBytes = 12345;      // 奇数长度
    constexpr uint64_t kBigBytes = 300 * 1024;      // 跨多个簇
    constexpr uint64_t kModuleBytes = 5000;
    // 夹具文件在卷内的路径
    constexpr const char* kHelloPath = "/Hello.txt";
    constexpr const char* kLongNamePath = "/LongNameFile.bin";
    constexpr const char* kBigPath = "/BigFile.bin";
    constexpr const char* kEmptyPath = "/Empty.bin";
    constexpr const char* kSubDir = "/DIR";
    constexpr const char* kModulePath = "/DIR/Module.osm";
    // BPB 字段偏移（与 Fat32Volume.cpp 的字节表一致，夹具侧独立核对用）
    constexpr uint32_t kBpbBytesPerSector = 0x0B;
    constexpr uint32_t kBpbSectorsPerCluster = 0x0D;
    constexpr uint32_t kBpbReservedSectors = 0x0E;
    constexpr uint32_t kBpbRootEntries = 0x11;
    constexpr uint32_t kBpbFatSize16 = 0x16;
    constexpr uint32_t kBpbFatSize32 = 0x24;
    constexpr uint32_t kBpbFsType = 0x52;

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
        Bytes bytes;                 // 介质内容（FAT32 卷镜像）
        uint32_t blockBytes = 512;   // 逻辑块大小，2 的幂
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
    const FileSystemOps* Ops() { return KernelModules::Fat32::FileSystemOpsTable(); }
    void* Context() { return KernelModules::Fat32::FileSystemContext(); }

    // 按只读模式挂载一份介质
    Storage::Error MountMedium(Medium& medium, uint32_t mode = 0) {
        return Ops()->Mount(Context(), &kDeviceOps, &medium, mode);
    }

    // 夹具目录与基准卷镜像
    fs::path g_work;
    Bytes g_volume;
    Bytes g_hello;
    Bytes g_longName;
    Bytes g_big;
    Bytes g_module;

    // 生成夹具：mkfs.vfat 建卷，mtools 放文件树；返回 false 表示基础设施缺失
    bool MakeFixture() {
        const std::string mkfs = Env("MKFS_VFAT", "mkfs.vfat");
        const std::string mmd = Env("MMD", "mmd");
        const std::string mcopy = Env("MCOPY", "mcopy");
        // mtools 对普通文件镜像做几何检查，夹具场景显式跳过
        Require(::setenv("MTOOLS_SKIP_CHECK", "1", 1) == 0, "设置 MTOOLS_SKIP_CHECK 失败");
        const fs::path image = g_work / "volume.img";
        g_hello = Pattern(kHelloBytes, 1);
        g_longName = Pattern(kLongNameBytes, 2);
        g_big = Pattern(kBigBytes, 3);
        g_module = Pattern(kModuleBytes, 4);
        const fs::path helloFile = g_work / "Hello.txt";
        const fs::path longFile = g_work / "LongNameFile.bin";
        const fs::path bigFile = g_work / "BigFile.bin";
        const fs::path emptyFile = g_work / "Empty.bin";
        const fs::path moduleFile = g_work / "Module.osm";
        WriteFile(helloFile, g_hello.data(), g_hello.size());
        WriteFile(longFile, g_longName.data(), g_longName.size());
        WriteFile(bigFile, g_big.data(), g_big.size());
        WriteFile(emptyFile, nullptr, 0);
        WriteFile(moduleFile, g_module.data(), g_module.size());
        const fs::path log = g_work / "fixture.log";
        // 夹具步骤失败时给出日志尾部：基础设施缺失与命令失败都能定位
        const auto failed = [&log](const char* what) {
            std::cerr << "CheckFat32: 夹具步骤失败：" << what << std::endl;
            ShowLogTail(log, 8);
            return false;
        };
        if (Run({"truncate", "-s", std::to_string(kImageBytes), image.string()}, log) != 0) return failed("truncate");
        if (Run({mkfs, "-F", "32", "-n", kVolumeLabel, "-i", "12345678", image.string()}, log) != 0) return failed("mkfs.vfat");
        if (Run({mmd, "-i", image.string(), "::/DIR"}, log) != 0) return failed("mmd");
        if (Run({mcopy, "-i", image.string(), helloFile.string(), "::/Hello.txt"}, log) != 0) return failed("mcopy Hello.txt");
        if (Run({mcopy, "-i", image.string(), longFile.string(), "::/LongNameFile.bin"}, log) != 0) return failed("mcopy LongNameFile.bin");
        if (Run({mcopy, "-i", image.string(), bigFile.string(), "::/BigFile.bin"}, log) != 0) return failed("mcopy BigFile.bin");
        if (Run({mcopy, "-i", image.string(), emptyFile.string(), "::/Empty.bin"}, log) != 0) return failed("mcopy Empty.bin");
        if (Run({mcopy, "-i", image.string(), moduleFile.string(), "::/DIR/Module.osm"}, log) != 0) return failed("mcopy DIR/Module.osm");
        g_volume = ReadFile(image);
        return g_volume.size() == kImageBytes;
    }

    // 挂载基准卷（可先做副本再改字节）
    Storage::Error MountVolume(const Bytes& volume, Medium& medium, uint32_t mode = 0) {
        medium.bytes = volume;
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

    // 读镜像里的 16 位字段（小端）
    uint16_t Get16(const Bytes& data, std::size_t at) {
        Require(at + 2 <= data.size(), "夹具字段越界");
        return static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
    }
    // 读镜像里的 32 位字段（小端）
    uint32_t Get32(const Bytes& data, std::size_t at) {
        Require(at + 4 <= data.size(), "夹具字段越界");
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[at + i]) << (8 * i);
        return value;
    }
    // 写镜像里的 16 位字段（小端）
    void Put16(Bytes& data, std::size_t at, uint16_t value) {
        Require(at + 2 <= data.size(), "夹具字段越界");
        data[at] = static_cast<uint8_t>(value);
        data[at + 1] = static_cast<uint8_t>(value >> 8);
    }
    // 写镜像里的 32 位字段（小端）
    void Put32(Bytes& data, std::size_t at, uint32_t value) {
        Require(at + 4 <= data.size(), "夹具字段越界");
        for (uint32_t i = 0; i < 4; ++i) data[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
    // 某个文件的起始簇：先经 FileSystemOps 打开取目录项偏移，再从镜像里读目录项
    uint32_t FirstClusterOf(const char* path) {
        uint64_t object = 0;
        Require(Ops()->Open(Context(), path, &object) == Storage::Error::Ok, "Open 应成功");
        Require(object != 0 && object != KernelModules::Fat32::Fat32Volume::kRootObject, "对象标识应是目录项偏移");
        const std::size_t at = static_cast<std::size_t>(object - 1);
        Require(at + 32 <= g_volume.size(), "目录项越界");
        return (static_cast<uint32_t>(Get16(g_volume, at + 0x14)) << 16) | Get16(g_volume, at + 0x1A);
    }
    // FAT 项在镜像里的字节偏移（用 BPB 的保留扇区数与扇区大小换算）
    std::size_t FatEntryOffset(const Bytes& volume, uint32_t cluster) {
        const uint32_t bytesPerSector = Get16(volume, kBpbBytesPerSector);
        const uint32_t reserved = Get16(volume, kBpbReservedSectors);
        return static_cast<std::size_t>(reserved) * bytesPerSector + static_cast<std::size_t>(cluster) * 4;
    }

    // —— 正例：挂载、事实与读取 ——

    // 格式名与能力位：只读、可列目录、不跟随符号链接、名字 ASCII 折叠
    void CaseFormatAndCaps() {
        Require(std::strcmp(Ops()->Format(Context()), "fat32") == 0, "格式名应为 fat32");
        Storage::FileSystemCaps caps{};
        Ops()->Caps(Context(), &caps);
        Require(!caps.writable && caps.directoryListing && !caps.symlinks, "能力位应为只读、可列目录、不跟随符号链接");
        Require(caps.nameCase == Storage::NameCase::Insensitive, "FAT32 名字比较应为 ASCII 折叠");
    }

    // 未挂载状态
    void CaseNotMounted() {
        Ops()->Unmount(Context());
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::InvalidArgument, "未挂载时 Stat 应返回参数非法");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/", &object) == Storage::Error::InvalidArgument, "未挂载时 Open 应返回参数非法");
        uint8_t identity[16] = {};
        Require(Ops()->VolumeIdentity(Context(), identity) == Storage::Error::InvalidArgument, "未挂载时卷标识应返回参数非法");
    }

    // 挂载事实：根目录可解析且是目录
    void CaseMountFacts() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t root = 0;
        Require(Ops()->Open(Context(), "/", &root) == Storage::Error::Ok, "根路径应可解析");
        Require(root == KernelModules::Fat32::Fat32Volume::kRootObject, "根目录对象标识应是固定哨兵");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::Ok && info.directory && info.bytes == 0, "根目录事实不符");
        Ops()->Unmount(Context());
    }

    // 卷标识：BPB 的 Volume ID（4 字节）+ Volume Label（11 字节）
    void CaseVolumeIdentity() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        uint8_t identity[16] = {};
        Require(Ops()->VolumeIdentity(Context(), identity) == Storage::Error::Ok, "卷标识应成功");
        Require(Get32(g_volume, 0x43) == kVolumeSerial, "夹具卷序号应为 mkfs 指定的值");
        Require(std::memcmp(identity, g_volume.data() + 0x43, 4) == 0, "卷标识前 4 字节应是 BPB 卷序号原字节");
        Require(std::memcmp(identity + 4, kVolumeLabel, std::strlen(kVolumeLabel)) == 0, "卷标识随后应是卷标");
        Require(identity[4 + std::strlen(kVolumeLabel)] == 0, "卷标识其余字节应补零");
        Ops()->Unmount(Context());
    }

    // 读写挂载、空设备表、非法逻辑块大小都拒绝
    void CaseRejectInvalidMount() {
        Medium medium;
        medium.bytes = g_volume;
        Require(MountMedium(medium, static_cast<uint32_t>(Storage::MountMode::ReadWrite)) == Storage::Error::NotSupported, "读写挂载应拒绝");
        Require(Ops()->Mount(Context(), nullptr, nullptr, 0) == Storage::Error::InvalidArgument, "空设备表应拒绝");
        Medium odd;
        odd.bytes = g_volume;
        odd.blockBytes = 500;
        Require(MountMedium(odd) == Storage::Error::InvalidArgument, "非 2 的幂逻辑块大小应拒绝");
        BlockDeviceOps broken = kDeviceOps;
        broken.Read = nullptr;
        Require(Ops()->Mount(Context(), &broken, &medium, 0) == Storage::Error::InvalidArgument, "缺读原语的设备表应拒绝");
    }

    // 根目录项集合：LFN 保留真实名字
    void CaseRootListing() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        const auto entries = ListDirectory("/");
        std::string names;
        for (const auto& entry : entries) names += "[" + entry.first + "]";
        Require(entries.size() == 5, "根目录应有五个目录项，实际 " + std::to_string(entries.size()) + " " + names);
        Require(entries.count("Hello.txt") == 1, "根目录应含 Hello.txt，实际 " + names);
        Require(entries.count("LongNameFile.bin") == 1, "根目录应含 LFN 长名 LongNameFile.bin，实际 " + names);
        Require(entries.count("BigFile.bin") == 1, "根目录应含 BigFile.bin");
        Require(entries.count("Empty.bin") == 1, "根目录应含 Empty.bin");
        Require(entries.count("DIR") == 1 && entries.at("DIR").directory, "根目录应含目录 DIR");
        Require(entries.at("BigFile.bin").bytes == kBigBytes, "大文件目录项长度不符");
        Ops()->Unmount(Context());
    }

    // 名字命中：LFN 真实名与 8.3 名（大小写折叠）
    void CaseNameMatchingRules() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t lfn = 0;
        uint64_t lfnFolded = 0;
        uint64_t shortName = 0;
        uint64_t shortFolded = 0;
        Require(Ops()->Open(Context(), kLongNamePath, &lfn) == Storage::Error::Ok, "LFN 真实名应可打开");
        Require(Ops()->Open(Context(), "/longnamefile.bin", &lfnFolded) == Storage::Error::Ok, "LFN 小写形式应命中");
        Require(Ops()->Open(Context(), "/LONGNA~1.BIN", &shortName) == Storage::Error::Ok, "8.3 短名（LONGNA~1.BIN）应命中同一项");
        Require(Ops()->Open(Context(), "/longna~1.bin", &shortFolded) == Storage::Error::Ok, "8.3 短名小写形式应命中");
        Require(lfn == lfnFolded && lfn == shortName && lfn == shortFolded, "四种写法必须指向同一项");
        Ops()->Unmount(Context());
    }

    // 读常规文件：小文件、奇数长度与空文件
    void CaseReadFiles() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        ExpectFile(kHelloPath, g_hello);
        ExpectFile(kLongNamePath, g_longName);
        ExpectFile(kModulePath, g_module);
        uint64_t object = 0;
        Require(Ops()->Open(Context(), kEmptyPath, &object) == Storage::Error::Ok, "空文件应可打开");
        uint8_t buffer[8] = {};
        uint32_t read = 0xFFFF;
        Require(Ops()->Read(Context(), object, 0, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 0, "空文件应读到 0");
        Ops()->Unmount(Context());
    }

    // 跨簇大文件：抽样比对 + 文件尾边界
    void CaseReadBigFile() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), kBigPath, &object) == Storage::Error::Ok, "大文件应可打开");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), kBigPath, &info) == Storage::Error::Ok && info.bytes == kBigBytes, "大文件长度不符");
        const uint64_t offsets[] = {0, kBigBytes / 2, kBigBytes - 8192};
        for (const uint64_t offset : offsets) {
            uint8_t buffer[8192] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, offset, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == sizeof(buffer), "抽样读取应成功");
            Require(std::memcmp(buffer, g_big.data() + offset, sizeof(buffer)) == 0, "抽样内容不符");
        }
        // 文件尾：不足一次请求按实际长度回报；越界读到 0
        uint8_t buffer[4096] = {};
        uint32_t read = 0;
        Require(Ops()->Read(Context(), object, kBigBytes - 100, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 100, "尾部读取应按实际长度回报");
        Require(std::memcmp(buffer, g_big.data() + (kBigBytes - 100), 100) == 0, "尾部内容不符");
        Require(Ops()->Read(Context(), object, kBigBytes + 1000, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 0, "越过文件尾应读到 0");
        Require(Ops()->Read(Context(), object, 0, nullptr, 16, &read) == Storage::Error::InvalidArgument, "空缓冲应拒绝");
        Require(Ops()->Read(Context(), object, 0, buffer, 0, &read) == Storage::Error::InvalidArgument, "零长度应拒绝");
        Ops()->Unmount(Context());
    }

    // 子目录：遍历与读取
    void CaseSubdirectory() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        const auto entries = ListDirectory(kSubDir);
        std::string names;
        for (const auto& entry : entries) names += "[" + entry.first + "]";
        Require(entries.size() == 1 && entries.count("Module.osm") == 1, "子目录应只含 Module.osm，实际 " + names);
        ExpectFile(kModulePath, g_module);
        Ops()->Unmount(Context());
    }

    // 目录名字缓冲不足：NameTooLong 且不推进游标
    void CaseDirectoryNameTooLong() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
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

    // 读取面错误映射
    void CaseReadErrors() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t dir = 0;
        Require(Ops()->Open(Context(), kSubDir, &dir) == Storage::Error::Ok, "目录应可打开");
        uint8_t buffer[16] = {};
        uint32_t read = 0;
        Require(Ops()->Read(Context(), dir, 0, buffer, sizeof(buffer), &read) == Storage::Error::IsDirectory, "对目录读取应返回 IsDirectory");
        uint64_t root = 0;
        Require(Ops()->Open(Context(), "/", &root) == Storage::Error::Ok, "根目录应可打开");
        Require(Ops()->Read(Context(), root, 0, buffer, sizeof(buffer), &read) == Storage::Error::IsDirectory, "对根目录读取应返回 IsDirectory");
        uint64_t file = 0;
        Require(Ops()->Open(Context(), kHelloPath, &file) == Storage::Error::Ok, "文件应可打开");
        char name[32] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info{};
        uint64_t cursor = 0;
        Require(Ops()->ReadDirectory(Context(), file, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::NotDirectory,
            "对文件列目录应返回 NotDirectory");
        Require(Ops()->Read(Context(), 0, 0, buffer, sizeof(buffer), &read) == Storage::Error::InvalidArgument, "对象标识 0 应返回参数非法");
        Ops()->Unmount(Context());
    }

    // 路径错误映射
    void CasePathErrors() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/nope", &object) == Storage::Error::NotFound, "不存在的路径应返回 NotFound");
        Require(Ops()->Open(Context(), "Hello.txt", &object) == Storage::Error::InvalidArgument, "非绝对路径应返回参数非法");
        Require(Ops()->Open(Context(), "/Hello.txt/", &object) == Storage::Error::InvalidArgument, "尾斜杠应返回参数非法");
        Require(Ops()->Open(Context(), "/./Hello.txt", &object) == Storage::Error::InvalidArgument, "含 . 的路径应返回参数非法");
        Require(Ops()->Open(Context(), "/DIR/../Hello.txt", &object) == Storage::Error::InvalidArgument, "含 .. 的路径应返回参数非法");
        Require(Ops()->Open(Context(), "/Hello.txt/x", &object) == Storage::Error::NotDirectory, "中间项不是目录应返回 NotDirectory");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/nope", &info) == Storage::Error::NotFound, "Stat 应传播 NotFound");
        Ops()->Unmount(Context());
    }

    // 非 FAT32 形态：FAT16 的根目录项数与 16 位 FAT 大小、文件系统类型字符串
    void CaseRejectFat16Shape() {
        {
            Bytes damaged = g_volume;
            Put16(damaged, kBpbRootEntries, 512);
            Medium medium;
            Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "根目录项数非零（FAT16 形态）应拒绝");
        }
        {
            Bytes damaged = g_volume;
            Put16(damaged, kBpbFatSize16, 64);
            Medium medium;
            Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "16 位 FAT 大小非零应拒绝");
        }
        {
            Bytes damaged = g_volume;
            damaged[kBpbFsType] = 'F';
            damaged[kBpbFsType + 3] = '1';
            damaged[kBpbFsType + 4] = '6';
            Medium medium;
            Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "文件系统类型不是 FAT32 应拒绝");
        }
    }

    // 非法几何：扇区大小不是 2 的幂
    void CaseRejectInvalidSectorSize() {
        Bytes damaged = g_volume;
        Put16(damaged, kBpbBytesPerSector, 500);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "非 2 的幂扇区大小应拒绝");
    }

    // FAT 链损坏：坏簇标记与自环都按结构非法拒绝
    void CaseRejectBrokenChain() {
        {
            Medium medium;
            Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
            const uint32_t cluster = FirstClusterOf(kBigPath);
            Ops()->Unmount(Context());
            Bytes damaged = g_volume;
            Put32(damaged, FatEntryOffset(damaged, cluster), 0x0FFFFFF7u);   // 坏簇
            Medium broken;
            Require(MountVolume(damaged, broken) == Storage::Error::Ok, "坏 FAT 项不影响挂载");
            uint64_t object = 0;
            Require(Ops()->Open(Context(), kBigPath, &object) == Storage::Error::Ok, "文件应可打开");
            uint8_t buffer[8192] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, 0, buffer, sizeof(buffer), &read) == Storage::Error::Corrupt, "坏簇应返回 Corrupt");
            Ops()->Unmount(Context());
        }
        {
            Medium medium;
            Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
            const uint32_t cluster = FirstClusterOf(kBigPath);
            Ops()->Unmount(Context());
            Bytes damaged = g_volume;
            Put32(damaged, FatEntryOffset(damaged, cluster), cluster);   // 自环
            Medium looped;
            Require(MountVolume(damaged, looped) == Storage::Error::Ok, "自环 FAT 项不影响挂载");
            uint64_t object = 0;
            Require(Ops()->Open(Context(), kBigPath, &object) == Storage::Error::Ok, "文件应可打开");
            uint8_t buffer[8192] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, 0, buffer, sizeof(buffer), &read) == Storage::Error::Corrupt, "FAT 自环应返回 Corrupt");
            Ops()->Unmount(Context());
        }
    }

    // 读盘失败透传：挂载期与读取期都返回 Io
    void CaseReadFailurePropagates() {
        {
            Medium medium;
            medium.bytes = g_volume;
            medium.failRead = true;
            Require(MountMedium(medium) == Storage::Error::Io, "挂载期读失败应返回 Io");
        }
        {
            Medium medium;
            Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "挂载应成功");
            uint64_t object = 0;
            Require(Ops()->Open(Context(), kHelloPath, &object) == Storage::Error::Ok, "文件应可打开");
            medium.failRead = true;
            uint8_t buffer[512] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, 0, buffer, sizeof(buffer), &read) == Storage::Error::Io, "读文件失败应返回 Io");
            Ops()->Unmount(Context());
        }
    }

    // 4096 字节设备逻辑块：按设备块拼装读取
    void CaseWideDeviceBlocks() {
        Medium medium;
        medium.blockBytes = 4096;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "4KiB 逻辑块设备上挂载应成功");
        ExpectFile(kHelloPath, g_hello);
        Ops()->Unmount(Context());
    }

    // 卸载与重挂
    void CaseRemount() {
        Medium medium;
        Require(MountVolume(g_volume, medium) == Storage::Error::Ok, "首次挂载应成功");
        ExpectFile(kHelloPath, g_hello);
        Ops()->Unmount(Context());
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::InvalidArgument, "卸载后 Stat 应返回参数非法");
        Require(MountMedium(medium) == Storage::Error::Ok, "重挂应成功");
        Require(Ops()->Stat(Context(), kHelloPath, &info) == Storage::Error::Ok && info.bytes == g_hello.size(), "重挂后事实应恢复");
        Ops()->Unmount(Context());
    }

    // 模块 Init 的登记路径
    void CaseModuleInitRegisters() {
        static const FileSystemOps* registeredOps = nullptr;
        static void* registeredContext = nullptr;
        static FileSystemId registeredId = 0;
        _Contract_Import_FsVfs_v1.RegisterFileSystem = +[](const FileSystemOps* ops, void* context, FileSystemId* id) -> Storage::Error {
            registeredOps = ops;
            registeredContext = context;
            *id = 11;
            registeredId = *id;
            return Storage::Error::Ok;
        };
        KernelAbiContext context{};
        context.bytes = sizeof(KernelAbiContext);
        context.version = 1;
        context.kernelAbiVersion = 1;
        context.moduleBase = 0;
        context.moduleId = kModFAT32File;
        Require(_Module_Init(&context) == KernelError::kOk, "Init 应成功");
        Require(registeredOps == KernelModules::Fat32::FileSystemOpsTable(), "登记应使用模块的 FileSystemOps 表");
        Require(registeredContext == KernelModules::Fat32::FileSystemContext(), "登记应使用模块内静态卷实例");
        Require(registeredId == 11, "登记标识应来自 Fs.Vfs 的返回");
        Require(_Module_Init(nullptr) == KernelError::kInvalidArgument, "空上下文应拒绝");
        KernelAbiContext wrongId = context;
        wrongId.moduleId = kModISO9660;
        Require(_Module_Init(&wrongId) == KernelError::kCorrupt, "ModuleId 不符应拒绝");
        _Contract_Import_FsVfs_v1.RegisterFileSystem = nullptr;
        Require(_Module_Init(&context) == KernelError::kState, "导入未回填应返回 kState");
        Require(_Module_Abort(&context) == KernelError::kOk, "Abort 应为空操作");
    }

    // 跑一个用例
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckFat32: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
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
        Case("根目录项集合（LFN 长名）", CaseRootListing);
        Case("名字命中规则（LFN 与 8.3 折叠）", CaseNameMatchingRules);
        Case("读常规文件与空文件", CaseReadFiles);
        Case("跨簇大文件与文件尾", CaseReadBigFile);
        Case("子目录遍历与读取", CaseSubdirectory);
        Case("目录名字缓冲不足", CaseDirectoryNameTooLong);
        Case("读取面错误映射", CaseReadErrors);
        Case("路径错误映射", CasePathErrors);
        Case("拒绝 FAT16 形态", CaseRejectFat16Shape);
        Case("拒绝非法扇区大小", CaseRejectInvalidSectorSize);
        Case("拒绝 FAT 链损坏", CaseRejectBrokenChain);
        Case("读盘失败透传", CaseReadFailurePropagates);
        Case("4096 字节逻辑块设备", CaseWideDeviceBlocks);
        Case("卸载与重挂", CaseRemount);
        Case("模块 Init 登记路径", CaseModuleInitRegisters);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckFat32: " << g_ran << "/" << kCaseCount << " cases passed" << std::endl;
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
                "CheckFat32 — 内核 FAT32File 只读模块与 FileSystemOps 九槽表的宿主侧检查\n"
                "\n"
                "用法：CheckFat32 [--work <目录>] [--keep]\n"
                "\n"
                "  --work <目录>  工作目录（夹具与临时文件），默认建在系统临时目录下\n"
                "  --keep         无论成败都保留工作目录\n"
                "环境变量 MKFS_VFAT / MMD / MCOPY 指定夹具工具；缺工具返回 2\n";
            return 0;
        }
        if (arg == "--work") {
            if (i + 1 >= argc) { std::cerr << "CheckFat32: --work 缺少参数\n"; return 2; }
            work = argv[++i];
        } else if (arg == "--keep") {
            keep = true;
        } else {
            std::cerr << "CheckFat32: 无法识别的参数：" << arg << '\n';
            return 2;
        }
    }
    try {
        if (work.empty()) {
            char pattern[] = "/tmp/checkfat32.XXXXXX";
            const char* created = ::mkdtemp(pattern);
            Require(created != nullptr, "无法创建临时工作目录");
            work = created;
        }
        fs::create_directories(work);
        g_work = work;
        if (!MakeFixture()) {
            std::cerr << "CheckFat32: 夹具生成失败（基础设施缺失）" << std::endl;
            if (!keep) fs::remove_all(work);
            return 2;
        }
        Execute();
        if (!keep) fs::remove_all(work);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckFat32: " << error.what() << '\n';
        if (!work.empty() && !keep) fs::remove_all(work);
        return 1;
    }
}
