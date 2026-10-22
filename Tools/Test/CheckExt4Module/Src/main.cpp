/* main.cpp
    CheckExt4Module：内核 Ext4 模块只读实现与 FileSystemOps 九槽表的宿主侧检查，手工执行

    检查对象是模块将要登记的那份源码：Packages/KernelModules/Ext4/Src/Ext4Volume.cpp 与
    Ext4Ops.cpp，用 vpath 按相对路径直接编进本工具，不做第二份参考实现
    夹具用 mke2fs 生成带已知文件树的真实 Ext4 卷，再按契约层 BlockDeviceOps 的形状构造
    内存块设备（逻辑块 512 可配 4096），经 FileSystemOps 表挂载、查询与读取：
    挂载事实 / 卷 UUID / 按路径读常规文件 / extent 与洞 / 目录遍历与游标 / 错误映射 /
    特性拒绝 / 读盘失败透传 / 卸载与重挂
    缺 mkfs 时按基础设施缺失报告（退出码 2）；用例数固定，失败返回 1，成功打印计数
*/

#include <Ext4Ops.hpp>

#include <ModuleAbi.hpp>
#include <Ext4Volume.hpp>

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
    constexpr unsigned kCaseCount = 26;
    // 已跑用例数
    unsigned g_ran = 0;
    // 夹具卷的固定参数：32 MiB、4096 字节块，卷标识与 mkfs 参数一致
    constexpr uint64_t kVolumeBytes = 32 * 1024 * 1024;
    constexpr uint32_t kVolumeBlocks = 8192;
    constexpr const char* kVolumeUuidText = "11111111-2222-3333-4444-555555555555";
    constexpr uint8_t kVolumeUuid[16] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    // 夹具文件的事实：路径按系统卷的落点布置（/LikesProgram/System 下）
    constexpr const char* kKernelPath = "/LikesProgram/System/LikesProgram.os";      // 内核镜像路径
    constexpr const char* kModulePath = "/LikesProgram/System/Modules/Module.osm";   // 模块路径
    constexpr const char* kSystemDir = "/LikesProgram/System";                       // 内核集目录
    constexpr uint64_t kKernelBytes = 32 * 1024;      // 内核镜像长度
    constexpr uint64_t kModuleBytes = 12345;          // 模块长度，奇数便于测非整块读取
    constexpr uint64_t kBigBytes = 4 * 1024 * 1024;   // /big.bin 长度，读取跨多个 extent
    // superblock 与字段偏移（与 Ext4Volume.cpp 的字节表一致，夹具侧独立核对用）
    constexpr uint64_t kSuperOffset = 1024;
    constexpr uint32_t kSuperFieldMagic = 0x38;
    constexpr uint32_t kSuperFieldState = 0x3A;
    constexpr uint32_t kSuperFieldFeatureIncompat = 0x60;
    constexpr uint32_t kSuperFieldFeatureRoCompat = 0x64;
    constexpr uint32_t kSuperFieldDescBytes = 0xFE;
    constexpr uint32_t kSuperFieldBlocksCountHi = 0x150;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }
    // 环境变量取值，空时用默认值
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
    // 确定性伪随机内容：同一 seed 产出同一串字节
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
    // 跑一条外部命令，输出重定向到日志；返回退出码（找不到命令返回 127）
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

    // 内存块设备：按契约层 BlockDeviceOps 的读侧形状提供读原语，按 512 或 4096 字节逻辑块访问
    struct Medium {
        Bytes bytes;              // 介质内容（Ext4 卷镜像）
        uint32_t blockBytes = 512;   // 逻辑块大小，2 的幂
        bool failRead = false;    // 注入读失败
        uint32_t reads = 0;       // 读调用次数
    };

    // BlockDeviceOps 槽 0：逻辑块大小
    uint32_t DeviceBlockBytes(void* context) {
        return static_cast<Medium*>(context)->blockBytes;
    }
    // BlockDeviceOps 槽 1：容量
    uint64_t DeviceCapacityBytes(void* context) {
        return static_cast<Medium*>(context)->bytes.size();
    }
    // BlockDeviceOps 槽 2：可写性（Ext4 只读，本夹具恒只读）
    uint32_t DeviceWritable(void* context) {
        (void)context;
        return 0;
    }
    // BlockDeviceOps 槽 3：读
    Storage::Error DeviceRead(void* context, uint64_t offset, void* buffer, uint32_t bytes) {
        Medium* medium = static_cast<Medium*>(context);
        ++medium->reads;
        if (medium->failRead) return Storage::Error::Io;
        if (offset > medium->bytes.size() || bytes > medium->bytes.size() - offset) return Storage::Error::InvalidArgument;
        std::memcpy(buffer, medium->bytes.data() + offset, bytes);
        return Storage::Error::Ok;
    }
    // BlockDeviceOps 槽 4：写（只读设备拒绝）
    Storage::Error DeviceWrite(void* context, uint64_t offset, const void* buffer, uint32_t bytes) {
        (void)context;
        (void)offset;
        (void)buffer;
        (void)bytes;
        return Storage::Error::ReadOnly;
    }
    // BlockDeviceOps 槽 5：刷新（无缓存）
    Storage::Error DeviceFlush(void* context) {
        (void)context;
        return Storage::Error::Ok;
    }

    // 设备调用表：契约层 BlockDeviceOps v1，context 指向 Medium
    const BlockDeviceOps kDeviceOps = {
        0,
        DeviceBlockBytes,
        DeviceCapacityBytes,
        DeviceWritable,
        DeviceRead,
        DeviceWrite,
        DeviceFlush,
    };

    // 检查对象：模块将要登记的 FileSystemOps 表与 context
    const FileSystemOps* Ops() { return KernelModules::Ext4::FileSystemOpsTable(); }
    void* Context() { return KernelModules::Ext4::FileSystemContext(); }

    // 按只读模式挂载一份介质
    Storage::Error MountMedium(Medium& medium, uint32_t mode = 0) {
        return Ops()->Mount(Context(), &kDeviceOps, &medium, mode);
    }

    // 夹具目录与基准卷镜像
    fs::path g_work;      // 本次检查的工作目录
    fs::path g_root;      // 夹具卷的根目录内容
    fs::path g_volume;    // 基准卷镜像路径
    Bytes g_kernel;       // 内核镜像内容
    Bytes g_module;       // 模块内容
    Bytes g_big;          // /big.bin 内容
    Bytes g_volumeBytes;  // 基准卷镜像字节

    // 生成夹具：写文件树，用 mkfs 产出带内容的 Ext4 卷；返回 false 表示基础设施缺失
    bool MakeFixture() {
        const std::string mkfs = Env("MKFS", "mke2fs");
        g_root = g_work / "root";
        fs::create_directories(g_root / "LikesProgram" / "System" / "Modules");
        g_kernel = Pattern(kKernelBytes, 1);
        g_module = Pattern(kModuleBytes, 2);
        g_big = Pattern(kBigBytes, 3);
        WriteFile(g_root / "LikesProgram" / "System" / "LikesProgram.os", g_kernel.data(), g_kernel.size());
        WriteFile(g_root / "LikesProgram" / "System" / "Modules" / "Module.osm", g_module.data(), g_module.size());
        WriteFile(g_root / "big.bin", g_big.data(), g_big.size());
        WriteFile(g_root / "empty.bin", nullptr, 0);
        g_volume = g_work / "volume.img";
        const std::string options =
            std::string("hash_seed=66666666-7777-8888-9999-aaaaaaaaaaaa,root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0");
        // -b 显式指定 4096 字节块：块数按块大小计
        const int code = Run({mkfs, "-t", "ext4", "-b", "4096", "-q", "-F", "-U", kVolumeUuidText, "-E", options, "-d", g_root.string(),
                              g_volume.string(), std::to_string(kVolumeBlocks)},
                             g_work / "mkfs.log");
        if (code != 0) {
            std::cerr << "CheckExt4Module: mkfs 失败（" << mkfs << "，退出码 " << code << "）" << std::endl;
            ShowLogTail(g_work / "mkfs.log", 8);
            return false;
        }
        g_volumeBytes = ReadFile(g_volume);
        return g_volumeBytes.size() == kVolumeBytes;
    }

    // 挂载基准卷（可先做一份副本再改字节）
    Storage::Error MountVolume(const Bytes& volume, Medium& medium, uint32_t mode = 0) {
        medium.bytes = volume;
        return MountMedium(medium, mode);
    }

    // 按路径读整个文件并与期望字节比对
    void ExpectFile(const char* path, const Bytes& expected) {
        Storage::FileInfo info;
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
        // 文件尾之后：读返回 0
        uint32_t tail = 0;
        Require(Ops()->Read(Context(), object, expected.size(), buffer.data(), 16, &tail) == Storage::Error::Ok && tail == 0,
            std::string("文件尾之后应读到 0：") + path);
    }

    // 列一个目录的全部目录项：从游标 0 起反复读，直到遍历结束返回 NotFound
    std::map<std::string, Storage::FileInfo> ListDirectory(const char* path) {
        uint64_t object = 0;
        Require(Ops()->Open(Context(), path, &object) == Storage::Error::Ok, std::string("Open 应成功：") + path);
        std::map<std::string, Storage::FileInfo> entries;
        uint64_t cursor = 0;
        for (;;) {
            char name[128] = {};
            uint32_t nameLength = 0;
            Storage::FileInfo info;
            const Storage::Error error = Ops()->ReadDirectory(Context(), object, &cursor, name, sizeof(name), &nameLength, &info);
            if (error == Storage::Error::NotFound) break;
            Require(error == Storage::Error::Ok, std::string("ReadDirectory 应成功：") + path);
            Require(nameLength > 0 && name[nameLength] == '\0', "目录项名字应以空字符结尾");
            const std::string entry(name, nameLength);
            Require(entries.find(entry) == entries.end(), "目录项不应重复：" + entry);
            entries.emplace(entry, info);
        }
        return entries;
    }

    uint32_t Get32(const Bytes& data, std::size_t at) {
        Require(at + 4 <= data.size(), "夹具字段越界");
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[at + i]) << (8 * i);
        return value;
    }
    // 写卷镜像里的 16 位字段（小端）
    void Put16(Bytes& data, std::size_t at, uint16_t value) {
        Require(at + 2 <= data.size(), "夹具字段越界");
        data[at] = static_cast<uint8_t>(value);
        data[at + 1] = static_cast<uint8_t>(value >> 8);
    }
    // 写卷镜像里的 32 位字段（小端）
    void Put32(Bytes& data, std::size_t at, uint32_t value) {
        Require(at + 4 <= data.size(), "夹具字段越界");
        for (uint32_t i = 0; i < 4; ++i) data[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // —— 正例：挂载、事实与读取 ——

    // 格式名与能力位：只读、可列目录、不跟随符号链接、名字逐字节比较
    void CaseFormatAndCaps() {
        Require(std::strcmp(Ops()->Format(Context()), "ext4") == 0, "格式名应为 ext4");
        Storage::FileSystemCaps caps{};
        Ops()->Caps(Context(), &caps);
        Require(!caps.writable && caps.directoryListing && !caps.symlinks, "能力位应为只读、可列目录、不跟随符号链接");
        Require(caps.nameCase == Storage::NameCase::Sensitive, "Ext4 名字比较应区分大小写");
    }

    // 未挂载状态：卷标识与卷事实不可用，读取返回参数非法
    void CaseNotMounted() {
        Ops()->Unmount(Context());
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::InvalidArgument, "未挂载时 Stat 应返回参数非法");
        uint8_t uuid[16] = {};
        Require(Ops()->VolumeIdentity(Context(), uuid) == Storage::Error::InvalidArgument, "未挂载时卷标识应返回参数非法");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/", &object) == Storage::Error::InvalidArgument, "未挂载时 Open 应返回参数非法");
    }

    // 挂载事实：块大小、块数、inode 数、根目录
    void CaseMountFacts() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        auto* volume = static_cast<KernelModules::Ext4::Ext4Volume*>(Context());
        Require(volume->BlockBytes() == 4096, "块大小应为 4096");
        Require(volume->InodesCount() > 0 && volume->BlocksCount() == kVolumeBlocks, "卷块数应为 8192");
        uint64_t root = 0;
        Require(Ops()->Open(Context(), "/", &root) == Storage::Error::Ok && root == 2, "根路径应解析到 inode 2");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::Ok && info.directory && info.bytes == 0, "根目录事实不符");
        Ops()->Unmount(Context());
    }

    // 卷标识：16 字节原字节，与 mkfs 指定的 UUID 一致
    void CaseVolumeIdentity() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        uint8_t uuid[16] = {};
        Require(Ops()->VolumeIdentity(Context(), uuid) == Storage::Error::Ok, "卷标识应成功");
        Require(std::memcmp(uuid, kVolumeUuid, 16) == 0, "卷标识应与 mkfs 指定的一致");
        Require(Ops()->VolumeIdentity(Context(), nullptr) == Storage::Error::InvalidArgument, "空输出缓冲应拒绝");
        Ops()->Unmount(Context());
    }

    // 读写挂载模式不在只读实现范围内：返回 NotSupported
    void CaseRejectReadWrite() {
        Medium medium;
        medium.bytes = g_volumeBytes;
        Require(MountMedium(medium, static_cast<uint32_t>(Storage::MountMode::ReadWrite)) == Storage::Error::NotSupported,
            "读写挂载应返回 NotSupported");
        auto* volume = static_cast<KernelModules::Ext4::Ext4Volume*>(Context());
        Require(volume->BlockBytes() == 0, "被拒绝的挂载不得留下挂载状态");
    }

    // 设备表与底层块大小非法：参数非法拒绝
    void CaseRejectInvalidDevice() {
        Require(Ops()->Mount(Context(), nullptr, nullptr, 0) == Storage::Error::InvalidArgument, "空设备表应拒绝");
        Medium medium;
        medium.bytes = g_volumeBytes;
        medium.blockBytes = 500;   // 不是 2 的幂
        Require(MountMedium(medium) == Storage::Error::InvalidArgument, "非 2 的幂逻辑块大小应拒绝");
        BlockDeviceOps broken = kDeviceOps;
        broken.Read = nullptr;
        Require(Ops()->Mount(Context(), &broken, &medium, 0) == Storage::Error::InvalidArgument, "缺读原语的设备表应拒绝");
    }

    // 多级目录事实：目录 bytes 为 0，子项可按路径解析
    void CaseStatNested() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), kSystemDir, &info) == Storage::Error::Ok && info.directory && info.bytes == 0, "目录事实不符");
        Require(Ops()->Stat(Context(), kKernelPath, &info) == Storage::Error::Ok && !info.directory && info.bytes == kKernelBytes,
            "内核镜像事实不符");
        Ops()->Unmount(Context());
    }

    // 读内核镜像（32 KiB）
    void CaseReadKernel() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        ExpectFile(kKernelPath, g_kernel);
        Ops()->Unmount(Context());
    }

    // 读奇数长度模块文件（12345 字节）
    void CaseReadNestedModule() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        ExpectFile(kModulePath, g_module);
        Ops()->Unmount(Context());
    }

    // 空文件读取：读到 0
    void CaseReadEmpty() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/empty.bin", &object) == Storage::Error::Ok, "空文件应可打开");
        uint8_t buffer[8] = {};
        uint32_t read = 0xFFFF;
        Require(Ops()->Read(Context(), object, 0, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 0, "空文件应读到 0");
        Ops()->Unmount(Context());
    }


    // 文件尾与参数边界：尾部不足一次请求时按实际长度回报；越过文件尾读到 0；空缓冲与零长度拒绝
    void CaseReadTail() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), kKernelPath, &object) == Storage::Error::Ok, "文件应可打开");
        uint8_t buffer[4096] = {};
        uint32_t read = 0;
        Require(Ops()->Read(Context(), object, kKernelBytes - 100, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 100,
            "尾部读取应按实际长度回报");
        Require(std::memcmp(buffer, g_kernel.data() + (kKernelBytes - 100), 100) == 0, "尾部内容不符");
        read = 0xFFFF;
        Require(Ops()->Read(Context(), object, kKernelBytes + 1000, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == 0,
            "越过文件尾应读到 0");
        Require(Ops()->Read(Context(), object, 0, nullptr, 16, &read) == Storage::Error::InvalidArgument, "空缓冲应拒绝");
        Require(Ops()->Read(Context(), object, 0, buffer, 0, &read) == Storage::Error::InvalidArgument, "零长度应拒绝");
        Ops()->Unmount(Context());
    }
    // 大文件跨多个 extent 抽样读取（4 MiB）
    void CaseReadBig() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/big.bin", &object) == Storage::Error::Ok, "大文件应可打开");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/big.bin", &info) == Storage::Error::Ok && info.bytes == kBigBytes, "大文件长度不符");
        const uint64_t offsets[] = {0, kBigBytes / 2, kBigBytes - 8192};
        for (const uint64_t offset : offsets) {
            uint8_t buffer[8192] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, offset, buffer, sizeof(buffer), &read) == Storage::Error::Ok && read == sizeof(buffer),
                "大文件读取应成功");
            Require(std::memcmp(buffer, g_big.data() + offset, sizeof(buffer)) == 0, "大文件内容不符");
        }
        Ops()->Unmount(Context());
    }

    // —— 错误映射与遍历 ——

    // 路径解析的错误映射：相对路径 / 不存在 / 中间项不是目录 / "." 与 ".." / 尾斜杠
    void CasePathErrors() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), "/nope", &object) == Storage::Error::NotFound, "不存在的路径应上报 NotFound");
        Require(Ops()->Open(Context(), "/LikesProgram/System/Nope.osm", &object) == Storage::Error::NotFound, "不存在的子项应上报 NotFound");
        Require(Ops()->Open(Context(), "/LikesProgram/System/LikesProgram.os/x", &object) == Storage::Error::NotDirectory,
            "基础项不是目录应上报 NotDirectory");
        Require(Ops()->Open(Context(), "LikesProgram/System/LikesProgram.os", &object) == Storage::Error::InvalidArgument,
            "非绝对路径应上报参数非法");
        Require(Ops()->Open(Context(), "/LikesProgram/./System", &object) == Storage::Error::InvalidArgument, "含 . 的路径应上报参数非法");
        Require(Ops()->Open(Context(), "/LikesProgram/System/..", &object) == Storage::Error::InvalidArgument, "含 .. 的路径应上报参数非法");
        Require(Ops()->Open(Context(), "/LikesProgram/System/", &object) == Storage::Error::InvalidArgument, "尾斜杠应上报参数非法");
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/nope", &info) == Storage::Error::NotFound, "Stat 应传播 NotFound");
        Ops()->Unmount(Context());
    }

    // 读取面错误：对目录读文件返回 IsDirectory，对文件列目录返回 NotDirectory，越界对象标识返回 Corrupt
    void CaseReadErrors() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t dir = 0;
        Require(Ops()->Open(Context(), kSystemDir, &dir) == Storage::Error::Ok, "目录应可打开");
        uint8_t buffer[16] = {};
        uint32_t read = 0;
        Require(Ops()->Read(Context(), dir, 0, buffer, sizeof(buffer), &read) == Storage::Error::IsDirectory, "对目录读取应返回 IsDirectory");
        uint64_t file = 0;
        Require(Ops()->Open(Context(), kKernelPath, &file) == Storage::Error::Ok, "文件应可打开");
        char name[32] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info{};
        uint64_t cursor = 0;
        Require(Ops()->ReadDirectory(Context(), file, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::NotDirectory,
            "对文件列目录应返回 NotDirectory");
        Require(Ops()->Read(Context(), 0, 0, buffer, sizeof(buffer), &read) == Storage::Error::Corrupt, "对象标识 0 应返回 Corrupt");
        Require(Ops()->Read(Context(), 99999999, 0, buffer, sizeof(buffer), &read) == Storage::Error::Corrupt, "越界 inode 应返回 Corrupt");
        Ops()->Unmount(Context());
    }

    // 目录遍历：内核集目录的目录项集合、类型与长度
    void CaseListDirectory() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        const auto entries = ListDirectory(kSystemDir);
        Require(entries.size() == 2, "内核集目录应有两个目录项");
        Require(entries.count("LikesProgram.os") == 1 && entries.count("Modules") == 1, "目录项集合不符");
        Require(entries.at("LikesProgram.os").directory == false && entries.at("LikesProgram.os").bytes == kKernelBytes, "文件目录项事实不符");
        Require(entries.at("Modules").directory && entries.at("Modules").bytes == 0, "子目录目录项事实不符");
        // 根目录：目录项集合也覆盖大文件与空文件
        const auto rootEntries = ListDirectory("/");
        Require(rootEntries.count("LikesProgram") == 1 && rootEntries.count("big.bin") == 1 && rootEntries.count("empty.bin") == 1,
            "根目录项集合不符");
        Ops()->Unmount(Context());
    }

    // 名字缓冲过小：NameTooLong 且不推进游标；换大缓冲可以继续
    void CaseDirectoryNameTooLong() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
        uint64_t object = 0;
        Require(Ops()->Open(Context(), kSystemDir, &object) == Storage::Error::Ok, "目录应可打开");
        uint64_t cursor = 0;
        char tiny[4] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info{};
        bool truncated = false;
        for (;;) {
            uint64_t before = cursor;
            const Storage::Error error = Ops()->ReadDirectory(Context(), object, &cursor, tiny, sizeof(tiny), &nameLength, &info);
            if (error == Storage::Error::Ok) continue;
            Require(error == Storage::Error::NameTooLong, "名字缓冲不足应返回 NameTooLong");
            Require(cursor == before, "名字缓冲不足时游标不得推进");
            truncated = true;
            break;
        }
        Require(truncated, "应至少出现一次名字缓冲不足");
        // 换大缓冲：从同一游标继续并能读到目录项
        char name[128] = {};
        Require(Ops()->ReadDirectory(Context(), object, &cursor, name, sizeof(name), &nameLength, &info) == Storage::Error::Ok,
            "换大缓冲后应能继续遍历");
        Ops()->Unmount(Context());
    }

    // 卸载后可重挂：卸载会清状态，重挂后事实与读取恢复
    void CaseRemount() {
        Medium medium;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "首次挂载应成功");
        ExpectFile(kKernelPath, g_kernel);
        Ops()->Unmount(Context());
        Storage::FileInfo info{};
        Require(Ops()->Stat(Context(), "/", &info) == Storage::Error::InvalidArgument, "卸载后 Stat 应返回参数非法");
        Require(MountMedium(medium) == Storage::Error::Ok, "重挂应成功");
        Require(Ops()->Stat(Context(), kKernelPath, &info) == Storage::Error::Ok && info.bytes == kKernelBytes, "重挂后事实应恢复");
        Ops()->Unmount(Context());
    }

    // —— 特性拒绝与读盘失败 ——

    // 坏 superblock 魔数：结构非法
    void CaseRejectBadMagic() {
        Bytes damaged = g_volumeBytes;
        Put16(damaged, kSuperOffset + kSuperFieldMagic, 0);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::Corrupt, "坏魔数应返回 Corrupt");
    }

    // 未知 INCOMPAT 位：不受支持
    void CaseRejectUnknownIncompat() {
        Bytes damaged = g_volumeBytes;
        Put32(damaged, kSuperOffset + kSuperFieldFeatureIncompat, Get32(g_volumeBytes, kSuperOffset + kSuperFieldFeatureIncompat) | 0x80000000u);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "未知 INCOMPAT 位应返回 NotSupported");
    }

    // 缺解析前提位：FILETYPE 缺失即拒绝
    void CaseRejectMissingRequired() {
        Bytes damaged = g_volumeBytes;
        Put32(damaged, kSuperOffset + kSuperFieldFeatureIncompat, Get32(g_volumeBytes, kSuperOffset + kSuperFieldFeatureIncompat) & ~0x0002u);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "缺 FILETYPE 应返回 NotSupported");
    }

    // RO_COMPAT 的 BIGALLOC：改变 extent 单位，拒绝
    void CaseRejectBigalloc() {
        Bytes damaged = g_volumeBytes;
        Put32(damaged, kSuperOffset + kSuperFieldFeatureRoCompat, Get32(g_volumeBytes, kSuperOffset + kSuperFieldFeatureRoCompat) | 0x0200u);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "RO_COMPAT BIGALLOC 应返回 NotSupported");
    }

    // 未干净卸载的卷（s_state 干净位清零）：需要日志回放，只读路径拒绝
    void CaseRejectUncleanVolume() {
        Bytes damaged = g_volumeBytes;
        Put16(damaged, kSuperOffset + kSuperFieldState, 0);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "未干净卸载的卷应返回 NotSupported");
    }

    // 64 位卷高位块数非零：卷超过 2^32 块，读路径表示不了，拒绝
    void CaseReject64BitHighBlocks() {
        Bytes damaged = g_volumeBytes;
        // 构造 64 位卷形态：置 64BIT 位、描述符 64 字节、高位块数非零
        Put32(damaged, kSuperOffset + kSuperFieldFeatureIncompat, Get32(g_volumeBytes, kSuperOffset + kSuperFieldFeatureIncompat) | 0x0080u);
        Put16(damaged, kSuperOffset + kSuperFieldDescBytes, 64);
        Put32(damaged, kSuperOffset + kSuperFieldBlocksCountHi, 1);
        Medium medium;
        Require(MountVolume(damaged, medium) == Storage::Error::NotSupported, "高位块数非零应返回 NotSupported");
    }

    // 读盘失败透传：挂载读 superblock 失败返回 Io；挂载成功后读文件失败也返回 Io
    void CaseReadFailurePropagates() {
        {
            Medium medium;
            medium.bytes = g_volumeBytes;
            medium.failRead = true;
            Require(MountMedium(medium) == Storage::Error::Io, "挂载期读失败应返回 Io");
        }
        {
            Medium medium;
            Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "挂载应成功");
            uint64_t object = 0;
            Require(Ops()->Open(Context(), kKernelPath, &object) == Storage::Error::Ok, "文件应可打开");
            medium.failRead = true;
            uint8_t buffer[512] = {};
            uint32_t read = 0;
            Require(Ops()->Read(Context(), object, 0, buffer, sizeof(buffer), &read) == Storage::Error::Io, "读文件失败应返回 Io");
            Ops()->Unmount(Context());
        }
    }

    // 4096 字节设备逻辑块的介质上也应能挂载与读取（块大小换算）
    void CaseWideDeviceBlocks() {
        Medium medium;
        medium.blockBytes = 4096;
        Require(MountVolume(g_volumeBytes, medium) == Storage::Error::Ok, "4KiB 逻辑块设备上挂载应成功");
        ExpectFile(kKernelPath, g_kernel);
        Ops()->Unmount(Context());
    }

    // 模块 Init 的登记路径：用替身 Fs.Vfs 回填区调用入口，核对参数与失败分类
    // 这一段直接编 Module.cpp（模块入口），检查的是模块将要交出去的 Init 语义
    void CaseModuleInitRegisters() {
        // 替身 RegisterFileSystem：记录调用参数，返回成功并给出非零登记标识
        static const FileSystemOps* registeredOps = nullptr;
        static void* registeredContext = nullptr;
        static FileSystemId registeredId = 0;
        _Contract_Import_FsVfs_v1.RegisterFileSystem = +[](const FileSystemOps* ops, void* context, FileSystemId* id) -> Storage::Error {
            registeredOps = ops;
            registeredContext = context;
            *id = 7;
            registeredId = *id;
            return Storage::Error::Ok;
        };
        KernelAbiContext context{};
        context.bytes = sizeof(KernelAbiContext);
        context.version = 1;
        context.kernelAbiVersion = 1;
        context.moduleBase = 0;
        context.moduleId = kModExt4;
        Require(_Module_Init(&context) == KernelError::kOk, "Init 应成功");
        Require(registeredOps == KernelModules::Ext4::FileSystemOpsTable(), "登记应使用模块的 FileSystemOps 表");
        Require(registeredContext == KernelModules::Ext4::FileSystemContext(), "登记应使用模块内静态卷实例");
        Require(registeredId == 7, "登记标识应来自 Fs.Vfs 的返回");
        // 上下文校验：空指针、长度不足、ModuleId 不符都拒绝
        Require(_Module_Init(nullptr) == KernelError::kInvalidArgument, "空上下文应拒绝");
        KernelAbiContext shortContext = context;
        shortContext.bytes = kKernelAbiContextMinBytes - 1;
        Require(_Module_Init(&shortContext) == KernelError::kInvalidArgument, "上下文过短应拒绝");
        KernelAbiContext wrongId = context;
        wrongId.moduleId = kModVfs;
        Require(_Module_Init(&wrongId) == KernelError::kCorrupt, "ModuleId 不符应拒绝");
        // 导入未回填：拒绝而不是空指针调用
        _Contract_Import_FsVfs_v1.RegisterFileSystem = nullptr;
        Require(_Module_Init(&context) == KernelError::kState, "导入未回填应返回 kState");
        Require(_Module_Abort(&context) == KernelError::kOk, "Abort 应为空操作");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckExt4Module: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
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
        Case("卷标识 UUID", CaseVolumeIdentity);
        Case("拒绝读写挂载", CaseRejectReadWrite);
        Case("拒绝非法设备表与块大小", CaseRejectInvalidDevice);
        Case("多级目录事实", CaseStatNested);
        Case("读内核镜像", CaseReadKernel);
        Case("读奇数长度模块文件", CaseReadNestedModule);
        Case("读空文件", CaseReadEmpty);
        Case("文件尾与参数边界", CaseReadTail);
        Case("读大文件跨 extent", CaseReadBig);
        Case("路径错误映射", CasePathErrors);
        Case("读取面错误映射", CaseReadErrors);
        Case("目录遍历", CaseListDirectory);
        Case("目录名字缓冲不足", CaseDirectoryNameTooLong);
        Case("卸载与重挂", CaseRemount);
        Case("拒绝坏 superblock 魔数", CaseRejectBadMagic);
        Case("拒绝未知 INCOMPAT 位", CaseRejectUnknownIncompat);
        Case("拒绝缺解析前提位", CaseRejectMissingRequired);
        Case("拒绝 RO_COMPAT BIGALLOC", CaseRejectBigalloc);
        Case("拒绝未干净卸载的卷", CaseRejectUncleanVolume);
        Case("拒绝 64 位卷高位块数", CaseReject64BitHighBlocks);
        Case("读盘失败透传", CaseReadFailurePropagates);
        Case("4KiB 逻辑块设备", CaseWideDeviceBlocks);
        Case("模块 Init 登记路径", CaseModuleInitRegisters);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckExt4Module: " << g_ran << "/" << kCaseCount << " cases passed" << std::endl;
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
                "CheckExt4Module — 内核 Ext4 只读模块与 FileSystemOps 九槽表的宿主侧检查\n"
                "\n"
                "用法：CheckExt4Module [--work <目录>] [--keep]\n"
                "\n"
                "  --work <目录>  工作目录（夹具与临时文件），默认建在系统临时目录下\n"
                "  --keep         无论成败都保留工作目录\n"
                "环境变量 MKFS 指定卷生成工具（默认 mke2fs）；缺工具返回 2\n";
            return 0;
        }
        if (arg == "--work") {
            if (i + 1 >= argc) { std::cerr << "CheckExt4Module: --work 缺少参数\n"; return 2; }
            work = argv[++i];
        } else if (arg == "--keep") {
            keep = true;
        } else {
            std::cerr << "CheckExt4Module: 无法识别的参数：" << arg << '\n';
            return 2;
        }
    }
    try {
        if (work.empty()) {
            char pattern[] = "/tmp/checkext4module.XXXXXX";
            const char* created = ::mkdtemp(pattern);
            Require(created != nullptr, "无法创建临时工作目录");
            work = created;
        }
        fs::create_directories(work);
        g_work = work;
        if (!MakeFixture()) {
            std::cerr << "CheckExt4Module: 夹具生成失败（基础设施缺失）" << std::endl;
            if (!keep) fs::remove_all(work);
            return 2;
        }
        Execute();
        if (!keep) fs::remove_all(work);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckExt4Module: " << error.what() << '\n';
        if (!work.empty() && !keep) fs::remove_all(work);
        return 1;
    }
}
