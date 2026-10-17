/* main.cpp
    CheckExt4：BaleenCore 的 Ext4 只读实现与块设备适配器的宿主侧检查，按需手工执行，不构成默认构建依赖

    检查对象是目标侧源码：Packages/Baleen/Core 的 Ext4.cpp 与 BiosBlockDevice.cpp，
    与内核侧共用同一份存储接口（Packages/Common/Include/Storage）——本工具把这两份源码
    按宿主选项编译后直接运行，不做第二份参考解析
    检查分两类：
    1. 真实卷夹具：用 mkfs 生成带已知文件的 Ext4 卷，核对按路径读取的字节与原文件一致，
       覆盖空文件、奇数长度、跨 extent 树的文件与多级目录，以及目录遍历的目录项集合、
       类型、长度、游标推进与名字缓冲过小
    2. 故障注入：坏 superblock、未知与拒绝特性、缺失解析前提位、64 位卷的高位块数与
       描述符大小、卷状态位、越界块号、错误卷偏移，以及块设备适配器的窗口中转、
       对齐与读盘失败路径

    缺 mkfs 时按基础设施缺失报告（退出码 2），--strict 把它当作失败
*/

#include <BiosBlockDevice.hpp>
#include <Ext4.hpp>

#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/SubDevice.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
    namespace fs = std::filesystem;
    using Bytes = std::vector<uint8_t>;
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 25;
    // 已跑用例数
    unsigned g_ran = 0;
    // 夹具卷的固定参数：16 MiB、4096 字节块，卷标识与 mkfs 参数一致
    constexpr uint64_t kVolumeBytes = 32 * 1024 * 1024;
    constexpr uint32_t kVolumeBlocks = 8192;
    constexpr const char* kVolumeUuidText = "11111111-2222-3333-4444-555555555555";
    constexpr uint8_t kVolumeUuid[16] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    // 模拟低内存与窗口：readSectors 只接受落低 1MiB、16 字节对齐的目的地
    constexpr uint32_t kLowBytes = 1024 * 1024;
    constexpr uint32_t kWindowBytes = 64 * 1024;
    // 夹具文件的事实：路径按系统卷布局布置，见 Docs/Specs/Common/系统卷布局.md
    constexpr const char* kKernelPath = "/LikesProgram/System/LikesProgram.os";      // 内核镜像路径
    constexpr const char* kModulePath = "/LikesProgram/System/Modules/Module.osm";   // 模块路径
    constexpr const char* kSystemDir = "/LikesProgram/System";                       // 内核集目录
    constexpr uint64_t kKernelBytes = 32 * 1024;      // 内核镜像的长度
    constexpr uint64_t kModuleBytes = 12345;          // 模块的长度，奇数便于测非整块读取
    constexpr uint64_t kBigBytes = 4 * 1024 * 1024;   // /big.bin 的长度，读取会跨多个窗口

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckExt4: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 读环境变量，未设置或为空时取默认值
    std::string Env(const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return value && *value ? value : fallback;
    }

    // 读取整个文件
    Bytes ReadBytes(const fs::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        Require(bool(input), "无法读取 " + path.string());
        const auto size = input.tellg();
        Require(size >= 0, "文件大小异常 " + path.string());
        Bytes data(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!data.empty()) input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return data;
    }

    // 整个覆盖写入二进制内容
    void WriteBytes(const fs::path& path, std::span<const uint8_t> bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(bool(output), "无法写入 " + path.string());
        if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        Require(bool(output), "写入失败 " + path.string());
    }

    // 确定性伪随机内容：同一 seed 产出同一串字节，便于夹具重建与比对
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

    // 运行一个命令，标准输出与标准错误写进日志，返回退出码；exec 失败返回 127
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

    // 打印日志尾部：失败时给出最后几行线索
    void ShowLogTail(const fs::path& log, unsigned lines) {
        std::ifstream input(log);
        std::vector<std::string> all;
        std::string line;
        while (std::getline(input, line)) all.push_back(line);
        const std::size_t first = all.size() > lines ? all.size() - lines : 0;
        for (std::size_t i = first; i < all.size(); ++i) std::cerr << "  | " << all[i] << std::endl;
    }

    // 模拟的低内存与介质：验证块设备适配器只把低地址窗口交给读盘服务
    class Medium {
    public:
        // 建一块低内存与一份介质影像；介质前部留出 lead 字节，供偏移视图用例使用
        Medium(std::size_t mediumBytes, uint64_t lead) : m_medium(mediumBytes, 0), m_lead(lead) {
            m_low = static_cast<uint8_t*>(std::malloc(kLowBytes));
            Require(m_low != nullptr, "无法分配模拟低内存");
        }
        // 释放低内存
        ~Medium() { std::free(m_low); }
        // 低内存起点
        uint8_t* Low() const { return m_low; }
        // 介质影像
        Bytes& Data() { return m_medium; }
        // 读盘服务：只接受落低 1MiB、16 字节对齐的目的地，失败返回 0；与 Stub 的服务契约一致
        uint32_t Read(uint32_t lba, uint32_t count, void* dest, uint32_t sectorBytes) {
            ++m_calls;
            if (m_fail) return 0;
            const uintptr_t address = reinterpret_cast<uintptr_t>(dest);
            const uintptr_t low = reinterpret_cast<uintptr_t>(m_low);
            if (address < low || address + static_cast<uintptr_t>(count) * sectorBytes > low + kLowBytes) return 0;
            if ((address & 15) != 0) return 0;
            const uint64_t offset = static_cast<uint64_t>(lba) * sectorBytes;
            const uint64_t bytes = static_cast<uint64_t>(count) * sectorBytes;
            if (offset > m_medium.size() || bytes > m_medium.size() - offset) return 0;
            std::memcpy(dest, m_medium.data() + offset, bytes);
            return 1;
        }
        // 读盘调用次数
        uint32_t Calls() const { return m_calls; }
        // 让后续读盘全部失败
        void FailReads(bool fail) { m_fail = fail; }
        // 把窗口起始位置固定到低内存开头的一个 16 字节对齐偏移，整段窗口落低 1MiB 之内
        uint8_t* Window() const { return m_low + 16; }

    private:
        Bytes m_medium;        // 介质影像
        uint8_t* m_low = nullptr;   // 模拟的低 1MiB 内存
        uint64_t m_lead = 0;        // 卷在介质影像里的起始偏移，模拟分区内的卷
        uint32_t m_calls = 0;       // 读盘调用次数
        bool m_fail = false;        // 是否让读盘失败
    };

    // 挂载好的卷：设备、视图与文件系统实例都放在静态存储里，随工具生命周期存在
    struct Mounted {
        Medium medium{kVolumeBytes + (1u << 20), 1u << 20};   // 介质前留 1 MiB，模拟分区内的卷
        Baleen::Core::BiosBlockDevice device;
        Storage::SubDevice view;
        Baleen::Core::Ext4FileSystem fs;
    };

    // 夹具目录与卷镜像的位置
    fs::path g_work;              // 本次检查的工作目录
    fs::path g_root;              // 夹具卷的根目录内容
    fs::path g_volume;            // 基准卷镜像
    Bytes g_kernel;               // 内核镜像的内容
    Bytes g_module;               // 模块的内容
    Bytes g_big;                  // /big.bin 的内容

    // 生成夹具：写文件树，用 mkfs 产出带内容的 Ext4 卷
    void MakeFixture() {
        const std::string mkfs = Env("MKFS", "mke2fs");
        g_root = g_work / "root";
        fs::create_directories(g_root / "LikesProgram" / "System" / "Modules");
        g_kernel = Pattern(kKernelBytes, 1);
        g_module = Pattern(kModuleBytes, 2);
        g_big = Pattern(kBigBytes, 3);
        WriteBytes(g_root / "LikesProgram" / "System" / "LikesProgram.os", g_kernel);
        WriteBytes(g_root / "LikesProgram" / "System" / "Modules" / "Module.osm", g_module);
        WriteBytes(g_root / "big.bin", g_big);
        WriteBytes(g_root / "empty.bin", {});
        g_volume = g_work / "volume.img";
        const std::string options =
            std::string("hash_seed=66666666-7777-8888-9999-aaaaaaaaaaaa,root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0");
        // -b 显式指定 4096 字节块：块数按块大小计，不加这一项时 mke2fs 会按 1024 字节块解释块数
        const int code = Run({mkfs, "-t", "ext4", "-b", "4096", "-q", "-F", "-U", kVolumeUuidText, "-E", options, "-d", g_root.string(),
                              g_volume.string(), std::to_string(kVolumeBlocks)},
                             g_work / "mkfs.log");
        if (code != 0) {
            std::cerr << "CheckExt4: mkfs 失败（" << mkfs << "）" << std::endl;
            ShowLogTail(g_work / "mkfs.log", 8);
            Fail("夹具生成失败");
        }
        Require(fs::file_size(g_volume) == kVolumeBytes, "夹具卷大小不符");
    }

    // 当前绑定的挂载实例：读盘服务是自由函数，需要它转发到模拟介质（工具内单实例使用）
    Mounted*& MountedRef() {
        static Mounted* instance = nullptr;
        return instance;
    }

    // 读盘服务：转发给当前挂载实例的模拟介质
    uint32_t ForwardRead(uint32_t lba, uint32_t count, void* dest, uint32_t sectorBytes) {
        Require(MountedRef() != nullptr, "读盘服务未绑定介质");
        return MountedRef()->medium.Read(lba, count, dest, sectorBytes);
    }

    // 写盘服务替身：只用来让块设备成为可写设备，内容不进介质
    uint32_t AcceptWrite(uint32_t lba, uint32_t count, const void* src, uint32_t sectorBytes) {
        (void)lba;
        (void)count;
        (void)src;
        (void)sectorBytes;
        return 1;
    }

    // 刷新服务替身：返回当前设定的结论
    Baleen::Core::FlushOutcome g_flushOutcome = Baleen::Core::FlushOutcome::Unsupported;
    Baleen::Core::FlushOutcome FlushStub() { return g_flushOutcome; }

    // 把基准卷装进介质影像、绑定块设备并挂载：返回挂载结果
    Storage::Error MountFixture(Mounted& mounted, const Bytes& volume) {
        Require(volume.size() <= mounted.medium.Data().size() - (1u << 20), "卷镜像超出介质");
        std::copy(volume.begin(), volume.end(), mounted.medium.Data().begin() + static_cast<std::ptrdiff_t>(1u << 20));
        MountedRef() = &mounted;
        mounted.device.Bind(&ForwardRead, 512, mounted.medium.Window(), kWindowBytes, static_cast<uint64_t>(512) << 32);
        if (mounted.device.BlockBytes() == 0) return Storage::Error::InvalidArgument;
        if (const Storage::Error error = Storage::SubDevice::Open(mounted.device, 1u << 20, volume.size(), mounted.view); error != Storage::Error::Ok) return error;
        return mounted.fs.Mount(mounted.view, Storage::MountMode::ReadOnly);
    }

    // 按路径读整个文件并与期望字节比对
    void ExpectFile(Baleen::Core::Ext4FileSystem& fs, const char* path, const Bytes& expected) {
        Storage::FileInfo info;
        Require(fs.Stat(path, info) == Storage::Error::Ok, std::string("Stat 应成功：") + path);
        Require(info.bytes == expected.size() && !info.directory, std::string("文件事实不符：") + path);
        Storage::FileId id;
        Require(fs.Open(path, id) == Storage::Error::Ok, std::string("Open 应成功：") + path);
        Bytes buffer(expected.size() + 16, 0xEE);
        uint32_t read = 0;
        Require(fs.Read(id, 0, buffer.data(), static_cast<uint32_t>(buffer.size()), read) == Storage::Error::Ok, std::string("Read 应成功：") + path);
        Require(read == expected.size(), std::string("读取长度不符：") + path);
        Require(std::memcmp(buffer.data(), expected.data(), expected.size()) == 0, std::string("读取内容不符：") + path);
        // 文件尾之后：读返回 0
        uint32_t tail = 0;
        Require(fs.Read(id, expected.size(), buffer.data(), 16, tail) == Storage::Error::Ok && tail == 0, std::string("文件尾之后应读到 0：") + path);
    }

    // 列一个目录的全部目录项：从游标 0 起反复读，直到遍历结束返回 NotFound
    // 名字按字典序归集，事实一并带回；顺手核对结尾空字符与重复项
    std::map<std::string, Storage::FileInfo> ListDirectory(Storage::FileSystem& filesystem, const char* path) {
        Storage::FileId id;
        Require(filesystem.Open(path, id) == Storage::Error::Ok, std::string("Open 应成功：") + path);
        std::map<std::string, Storage::FileInfo> entries;
        uint64_t cursor = 0;
        for (;;) {
            char name[128] = {};
            uint32_t nameLength = 0;
            Storage::FileInfo info;
            const Storage::Error error = filesystem.ReadDirectory(id, cursor, name, sizeof(name), nameLength, info);
            if (error == Storage::Error::NotFound) break;
            Require(error == Storage::Error::Ok, std::string("ReadDirectory 应成功：") + path);
            Require(nameLength > 0 && name[nameLength] == '\0', "目录项名字应以空字符结尾");
            const std::string entry(name, nameLength);
            Require(entries.find(entry) == entries.end(), "目录项不应重复：" + entry);
            entries.emplace(entry, info);
        }
        return entries;
    }

    // 用例：挂载卷并核对卷事实
    void CaseMountFacts() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Require(mounted.fs.BlockBytes() == 4096, "块大小应为 4096");
        Require(mounted.fs.InodesCount() > 0 && mounted.fs.BlocksCount() == kVolumeBlocks, "卷块数应为 8192");
        Require(mounted.fs.Uuid() != nullptr && std::memcmp(mounted.fs.Uuid(), kVolumeUuid, 16) == 0, "卷标识应与 mkfs 指定的一致");
        const Storage::FileSystemCaps caps = mounted.fs.Caps();
        Require(!caps.writable && caps.directoryListing && !caps.symlinks, "能力位应为只读、可列目录、不跟随符号链接");
        Require(std::strcmp(mounted.fs.Format(), "ext4") == 0, "格式名应为 ext4");
        // 根目录可解析
        Storage::FileId root;
        Require(mounted.fs.Open("/", root) == Storage::Error::Ok && root.value == 2, "根路径应解析到 inode 2");
    }

    // 用例：按路径读常规文件（32 KiB）
    void CaseReadKernel() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        ExpectFile(mounted.fs, kKernelPath, g_kernel);
    }

    // 用例：多级目录与奇数长度文件
    void CaseReadNested() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        ExpectFile(mounted.fs, kModulePath, g_module);
        // 目录事实：目录的 bytes 为 0
        Storage::FileInfo dirInfo;
        Require(mounted.fs.Stat(kSystemDir, dirInfo) == Storage::Error::Ok && dirInfo.directory && dirInfo.bytes == 0, "目录事实不符");
    }

    // 用例：空文件读取
    void CaseReadEmpty() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Storage::FileId id;
        Require(mounted.fs.Open("/empty.bin", id) == Storage::Error::Ok, "空文件应可打开");
        uint8_t buffer[8] = {};
        uint32_t read = 0xFFFF;
        Require(mounted.fs.Read(id, 0, buffer, sizeof(buffer), read) == Storage::Error::Ok && read == 0, "空文件应读到 0");
    }

    // 用例：大文件跨多窗口读取（4 MiB）
    void CaseReadBig() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Storage::FileId id;
        Require(mounted.fs.Open("/big.bin", id) == Storage::Error::Ok, "大文件应可打开");
        Storage::FileInfo info;
        Require(mounted.fs.Stat("/big.bin", info) == Storage::Error::Ok && info.bytes == kBigBytes, "大文件长度应为 8 MiB");
        // 抽样比对：首部、8 MiB 边界之外的中部与尾部
        const uint64_t offsets[] = {0, kBigBytes / 2, kBigBytes - 8192};
        for (const uint64_t offset : offsets) {
            uint8_t buffer[8192] = {};
            uint32_t read = 0;
            Require(mounted.fs.Read(id, offset, buffer, sizeof(buffer), read) == Storage::Error::Ok && read == sizeof(buffer), "大文件读取应成功");
            Require(std::memcmp(buffer, g_big.data() + offset, sizeof(buffer)) == 0, "大文件内容不符");
        }
    }

    // 用例：路径解析的错误映射
    void CasePathErrors() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Storage::FileId id;
        Require(mounted.fs.Open("/nope", id) == Storage::Error::NotFound, "不存在的路径应上报 NotFound");
        Require(mounted.fs.Open("/LikesProgram/System/Nope.osm", id) == Storage::Error::NotFound, "不存在的子项应上报 NotFound");
        Require(mounted.fs.Open("/LikesProgram/System/LikesProgram.os/x", id) == Storage::Error::NotDirectory, "基础项不是目录应上报 NotDirectory");
        Require(mounted.fs.Open("LikesProgram/System/LikesProgram.os", id) == Storage::Error::InvalidArgument, "非绝对路径应上报参数非法");
        Storage::FileInfo info;
        Require(mounted.fs.Stat("/nope", info) == Storage::Error::NotFound, "Stat 应传播 NotFound");
    }

    // 用例：坏 superblock 魔数
    void CaseBadMagic() {
        Bytes volume = ReadBytes(g_volume);
        volume[1024 + 0x38] ^= 0xFF;   // s_magic 改坏
        Mounted mounted;
        Require(MountFixture(mounted, volume) == Storage::Error::Corrupt, "魔数不符应上报 Corrupt");
    }

    // 用例：未知与拒绝的 INCOMPAT 特性
    void CaseUnknownFeatures() {
        {
            Bytes volume = ReadBytes(g_volume);
            uint32_t incompat = 0;
            std::memcpy(&incompat, volume.data() + 1024 + 0x60, 4);
            incompat |= 0x40000000u;   // 未知位
            std::memcpy(volume.data() + 1024 + 0x60, &incompat, 4);
            Mounted mounted;
            Require(MountFixture(mounted, volume) == Storage::Error::NotSupported, "未知 INCOMPAT 位应上报 NotSupported");
        }
        {
            Bytes volume = ReadBytes(g_volume);
            uint32_t incompat = 0;
            std::memcpy(&incompat, volume.data() + 1024 + 0x60, 4);
            incompat |= 0x8000u;   // INLINE_DATA：明确拒绝
            std::memcpy(volume.data() + 1024 + 0x60, &incompat, 4);
            Mounted mounted;
            Require(MountFixture(mounted, volume) == Storage::Error::NotSupported, "内联数据应上报 NotSupported");
        }
        {
            Bytes volume = ReadBytes(g_volume);
            uint32_t roCompat = 0;
            std::memcpy(&roCompat, volume.data() + 1024 + 0x64, 4);
            roCompat |= 0x200u;   // BIGALLOC：改变 extent 单位，必须拒绝
            std::memcpy(volume.data() + 1024 + 0x64, &roCompat, 4);
            Mounted mounted;
            Require(MountFixture(mounted, volume) == Storage::Error::NotSupported, "bigalloc 应上报 NotSupported");
        }
    }

    // 用例：缺失 FILETYPE 特性位（目录项按 16 位 name_len 解析的旧布局）
    void CaseMissingFiletype() {
        Bytes volume = ReadBytes(g_volume);
        uint32_t incompat = 0;
        std::memcpy(&incompat, volume.data() + 1024 + 0x60, 4);
        incompat &= ~0x2u;   // 去掉 FILETYPE
        std::memcpy(volume.data() + 1024 + 0x60, &incompat, 4);
        Mounted mounted;
        Require(MountFixture(mounted, volume) == Storage::Error::NotSupported, "缺 FILETYPE 应在挂载时拒绝");
    }

    // 用例：缺失 EXTENTS 特性位（i_block 走传统块映射的旧布局）
    void CaseMissingExtents() {
        Bytes volume = ReadBytes(g_volume);
        uint32_t incompat = 0;
        std::memcpy(&incompat, volume.data() + 1024 + 0x60, 4);
        incompat &= ~0x40u;   // 去掉 EXTENTS
        std::memcpy(volume.data() + 1024 + 0x60, &incompat, 4);
        Mounted mounted;
        Require(MountFixture(mounted, volume) == Storage::Error::NotSupported, "缺 EXTENTS 应在挂载时拒绝");
    }

    // 用例：64 位卷的高位块数非零（卷超过 2 的 32 次方块）
    void CaseHighBlocks() {
        Bytes volume = ReadBytes(g_volume);
        uint32_t incompat = 0;
        std::memcpy(&incompat, volume.data() + 1024 + 0x60, 4);
        Require((incompat & 0x80u) != 0, "夹具卷未启用 64 位特性");
        const uint32_t high = 1;
        std::memcpy(volume.data() + 1024 + 0x150, &high, 4);   // s_blocks_count_hi
        Mounted mounted;
        Require(MountFixture(mounted, volume) == Storage::Error::NotSupported, "高位块数非零应拒绝");
    }

    // 用例：64 位卷的描述符大小取 32（非法组合，会让高位 inode 表块号被丢弃）
    void CaseDescSize() {
        Bytes volume = ReadBytes(g_volume);
        uint32_t incompat = 0;
        std::memcpy(&incompat, volume.data() + 1024 + 0x60, 4);
        Require((incompat & 0x80u) != 0, "夹具卷未启用 64 位特性");
        const uint16_t desc = 32;
        std::memcpy(volume.data() + 1024 + 0xFE, &desc, 2);   // s_desc_size
        Mounted mounted;
        Require(MountFixture(mounted, volume) == Storage::Error::Corrupt, "描述符大小非 64 应拒绝");
    }

    // 用例：卷状态位不含 EXT4_VALID_FS（未干净卸载或处于错误状态）
    void CaseDirtyState() {
        Bytes volume = ReadBytes(g_volume);
        const uint16_t state = 0;
        std::memcpy(volume.data() + 1024 + 0x3A, &state, 2);   // s_state
        Mounted mounted;
        Require(MountFixture(mounted, volume) == Storage::Error::NotSupported, "非干净卷应在挂载时拒绝");
    }

    // 用例：块号越界（把卷的块数声明改小，映射检查必须拒绝）
    // 取根目录下的文件：路径解析只读根目录块与 inode 表，仍落在被截断范围内；
    // 多级路径要读的目录块自身也可能越界，会停在路径解析处，测不到文件数据这一层
    void CaseOutOfRangeBlock() {
        Bytes volume = ReadBytes(g_volume);
        const uint32_t small = 64;
        std::memcpy(volume.data() + 1024 + 0x04, &small, 4);   // s_blocks_count_lo
        Mounted mounted;
        Require(MountFixture(mounted, volume) == Storage::Error::Ok, "挂载本身应成功");
        Storage::FileId id;
        Require(mounted.fs.Open("/big.bin", id) == Storage::Error::Ok, "目录项解析仍应成功");
        uint8_t buffer[4096] = {};
        uint32_t read = 0;
        Require(mounted.fs.Read(id, 0, buffer, sizeof(buffer), read) == Storage::Error::Corrupt, "越界块号应上报 Corrupt");
    }

    // 用例：错误的卷偏移（偏移视图指向卷之外）
    void CaseWrongVolumeOffset() {
        Mounted mounted;
        Bytes volume = ReadBytes(g_volume);
        Require(volume.size() <= mounted.medium.Data().size(), "卷镜像超出介质");
        std::copy(volume.begin(), volume.end(), mounted.medium.Data().begin());
        MountedRef() = &mounted;
        mounted.device.Bind(&ForwardRead, 512, mounted.medium.Window(), kWindowBytes, static_cast<uint64_t>(512) << 32);
        // 从卷中部开视图：superblock 位置读到的不是有效结构
        const uint64_t middle = 8u << 20;
        Require(Storage::SubDevice::Open(mounted.device, middle, volume.size() - middle, mounted.view) == Storage::Error::Ok, "偏移视图应能建立");
        Require(mounted.fs.Mount(mounted.view, Storage::MountMode::ReadOnly) == Storage::Error::Corrupt, "错误偏移应上报 Corrupt");
    }

    // 用例：偏移视图的非法参数
    void CaseViewChecks() {
        Mounted mounted;
        MountedRef() = &mounted;
        mounted.device.Bind(&ForwardRead, 512, mounted.medium.Window(), kWindowBytes, static_cast<uint64_t>(512) << 32);
        Storage::SubDevice view;
        Require(Storage::SubDevice::Open(mounted.device, 512 + 256, 4096, view) == Storage::Error::InvalidArgument, "未对齐起点应拒绝");
        Require(Storage::SubDevice::Open(mounted.device, 0, 0, view) == Storage::Error::InvalidArgument, "零长度应拒绝");
        Require(Storage::SubDevice::Open(mounted.device, 4096, mounted.device.CapacityBytes(), view) == Storage::Error::InvalidArgument, "越过容量应拒绝");
        Require(Storage::SubDevice::Open(mounted.device, 0, 1u << 20, view) == Storage::Error::Ok, "合法区间应绑定");
    }

    // 用例：块设备适配器的窗口中转（跨窗口读、未对齐与越界拒绝）
    void CaseDeviceWindow() {
        Mounted mounted;
        MountedRef() = &mounted;
        mounted.device.Bind(&ForwardRead, 512, mounted.medium.Window(), kWindowBytes, static_cast<uint64_t>(512) << 32);
        Require(mounted.device.BlockBytes() == 512, "块大小应为会话扇区大小");
        Bytes source(256 * 1024);
        for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(i * 31 + 7);
        std::copy(source.begin(), source.end(), mounted.medium.Data().begin());
        Bytes target(source.size(), 0);
        // 一次 256 KiB 的读会被拆成多个窗口大小的读盘调用
        Require(mounted.device.Read(0, target.data(), static_cast<uint32_t>(target.size())) == Storage::Error::Ok, "大范围读应成功");
        Require(std::memcmp(target.data(), source.data(), source.size()) == 0, "大范围读的内容应一致");
        Require(mounted.medium.Calls() >= target.size() / kWindowBytes, "应拆成多次窗口读盘");
        // 未对齐与越界
        Require(mounted.device.Read(256, target.data(), 512) == Storage::Error::InvalidArgument, "未对齐偏移应拒绝");
        Require(mounted.device.Read(0, target.data(), 513) == Storage::Error::InvalidArgument, "未对齐长度应拒绝");
        Require(mounted.device.Read(0, nullptr, 512) == Storage::Error::InvalidArgument, "空指针应拒绝");
        Require(mounted.device.Read(mounted.device.CapacityBytes(), target.data(), 512) == Storage::Error::InvalidArgument, "越过容量应拒绝");
    }

    // 用例：读盘失败向上传播为 Io
    void CaseReadFailure() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Storage::FileId id;
        Require(mounted.fs.Open(kKernelPath, id) == Storage::Error::Ok, "读盘正常时 Open 应成功");
        mounted.medium.FailReads(true);
        uint8_t buffer[512] = {};
        uint32_t read = 0;
        Require(mounted.fs.Read(id, 0, buffer, sizeof(buffer), read) == Storage::Error::Io, "读盘失败应上报 Io");
        Storage::FileId again;
        Require(mounted.fs.Open(kKernelPath, again) == Storage::Error::Io, "读盘失败时 Open 应上报 Io");
        mounted.medium.FailReads(false);
    }

    // 用例：未绑定的块设备行为
    void CaseUnboundDevice() {
        Baleen::Core::BiosBlockDevice device;
        Require(device.BlockBytes() == 0 && device.CapacityBytes() == 0, "未绑定时事实查询为零");
        Require(!device.Writable(), "块设备只读");
        uint8_t buffer[512] = {};
        Require(device.Read(0, buffer, 512) == Storage::Error::InvalidArgument, "未绑定时读应拒绝");
        // 非法绑定参数保持未绑定
        uint8_t window[1024] = {};
        device.Bind(&ForwardRead, 500, window, 1024, 1 << 20);
        Require(device.BlockBytes() == 0, "非 2 的幂扇区大小应拒绝绑定");
        device.Bind(&ForwardRead, 512, window, 1024, 1 << 20);
        Require(device.BlockBytes() == 512 && device.CapacityBytes() == (1u << 20), "合法参数应绑定");
        device.Unbind();
        Require(device.BlockBytes() == 0, "解绑后应回到未绑定");
    }

    // 用例：写缓存刷新的返回值映射——只有绑定方报失败时才按写失败处理
    void CaseFlushMapping() {
        Baleen::Core::BiosBlockDevice device;
        uint8_t window[1024] = {};
        device.Bind(&ForwardRead, 512, window, 1024, 1u << 20);
        device.BindWrite(&AcceptWrite);
        // 没有绑定刷新服务：按“无可用原语”通过
        Require(device.Flush() == Storage::Error::Ok, "未绑定刷新服务时应通过");
        // 覆盖核验成立：主流程已确认这个可刷新端口就是启动介质所在的设备
        device.BindFlush(&FlushStub, true);
        Require(device.HasFlushPrimitive(), "写服务与刷新服务都在且覆盖成立时才算有原语");
        g_flushOutcome = Baleen::Core::FlushOutcome::Unsupported;
        Require(device.Flush() == Storage::Error::Ok, "无可用原语时应通过");
        g_flushOutcome = Baleen::Core::FlushOutcome::Flushed;
        Require(device.Flush() == Storage::Error::Ok, "刷新成立时应通过");
        g_flushOutcome = Baleen::Core::FlushOutcome::Failed;
        Require(device.Flush() == Storage::Error::Io, "刷新失败时按写失败返回 Io");
        // 端口是别人的：刷新函数在，但它刷的不是本介质——不算有原语，状态写入要据此报持久化边界
        Baleen::Core::BiosBlockDevice foreign;
        foreign.Bind(&ForwardRead, 512, window, 1024, 1u << 20);
        foreign.BindWrite(&AcceptWrite);
        foreign.BindFlush(&FlushStub, false);
        Require(!foreign.HasFlushPrimitive(), "覆盖核验不成立时不得声称有刷新原语");
        g_flushOutcome = Baleen::Core::FlushOutcome::Flushed;
        Require(foreign.Flush() == Storage::Error::Ok, "覆盖不成立时刷新照旧下发（读到通过即可）");
        // 只读设备没有写缓存要刷：即使绑了刷新服务也不调用
        Baleen::Core::BiosBlockDevice readOnly;
        readOnly.Bind(&ForwardRead, 512, window, 1024, 1u << 20);
        readOnly.BindFlush(&FlushStub, true);
        Require(!readOnly.HasFlushPrimitive(), "没有写服务的介质不得声称有刷新原语");
        Require(readOnly.Flush() == Storage::Error::Ok, "只读设备的刷新应通过");
    }

    // 用例：列出内核集目录——目录项集合、类型与长度都要对，"." 与 ".." 不作为目录项交出
    void CaseListSetDirectory() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        const std::map<std::string, Storage::FileInfo> entries = ListDirectory(mounted.fs, kSystemDir);
        Require(entries.size() == 2, "内核集目录应有两个目录项");
        Require(entries.count(".") == 0 && entries.count("..") == 0, "\".\" 与 \"..\" 不应作为目录项交出");
        const auto kernel = entries.find("LikesProgram.os");
        Require(kernel != entries.end() && !kernel->second.directory, "内核镜像应是常规文件");
        Require(kernel != entries.end() && kernel->second.bytes == kKernelBytes, "内核镜像长度应与夹具一致");
        const auto modules = entries.find("Modules");
        Require(modules != entries.end() && modules->second.directory, "Modules 应是目录");
        Require(modules != entries.end() && modules->second.bytes == 0, "目录的长度应为 0");
    }

    // 用例：列根目录——目录项集合与空文件的零长度；mkfs 会在根目录建 lost+found，一并核对
    void CaseListRoot() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        const std::map<std::string, Storage::FileInfo> entries = ListDirectory(mounted.fs, "/");
        Require(entries.size() == 4, "根目录应有四个目录项");
        Require(entries.count("lost+found") == 1 && entries.at("lost+found").directory, "mkfs 建的 lost+found 应是目录");
        Require(entries.count("LikesProgram") == 1 && entries.at("LikesProgram").directory, "LikesProgram 应是目录");
        Require(entries.count("big.bin") == 1 && entries.at("big.bin").bytes == kBigBytes, "big.bin 长度应与夹具一致");
        Require(entries.count("empty.bin") == 1 && entries.at("empty.bin").bytes == 0, "空文件长度应为 0");
    }

    // 用例：名字缓冲放不下时返回 NameTooLong 且不推进游标，换大缓冲可以接着读
    void CaseListSmallBuffer() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Storage::FileId id;
        Require(mounted.fs.Open(kSystemDir, id) == Storage::Error::Ok, "内核集目录应可打开");
        char small[4] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info;
        uint64_t cursor = 0;
        Require(mounted.fs.ReadDirectory(id, cursor, small, sizeof(small), nameLength, info) == Storage::Error::NameTooLong, "缓冲过小应返回 NameTooLong");
        Require(cursor == 0, "缓冲过小时不应推进游标");
        char large[128] = {};
        Require(mounted.fs.ReadDirectory(id, cursor, large, sizeof(large), nameLength, info) == Storage::Error::Ok, "换大缓冲应能继续");
        Require(nameLength > 0 && cursor != 0, "读出目录项后应推进游标");
    }

    // 用例：游标语义——按上一项返回的游标接着读，读完之后停在 NotFound
    void CaseListCursor() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Storage::FileId id;
        Require(mounted.fs.Open(kSystemDir, id) == Storage::Error::Ok, "内核集目录应可打开");
        uint32_t seen = 0;
        uint64_t cursor = 0;
        for (;;) {
            char name[128] = {};
            uint32_t nameLength = 0;
            Storage::FileInfo info;
            if (mounted.fs.ReadDirectory(id, cursor, name, sizeof(name), nameLength, info) == Storage::Error::NotFound) break;
            ++seen;
            Require(seen <= 8, "遍历没有在有限步内结束");
        }
        Require(seen == 2, "游标遍历应恰好走完两个目录项");
        // 游标越过目录尾之后仍是遍历结束，不是错误
        uint64_t beyond = 1u << 20;
        char name[128] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info;
        Require(mounted.fs.ReadDirectory(id, beyond, name, sizeof(name), nameLength, info) == Storage::Error::NotFound, "目录尾之后应返回 NotFound");
    }

    // 用例：对常规文件与越界标识调用目录遍历的错误映射
    void CaseListErrors() {
        Mounted mounted;
        Require(MountFixture(mounted, ReadBytes(g_volume)) == Storage::Error::Ok, "挂载应成功");
        Storage::FileId file;
        Require(mounted.fs.Open("/big.bin", file) == Storage::Error::Ok, "常规文件应可打开");
        char name[128] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info;
        uint64_t cursor = 0;
        Require(mounted.fs.ReadDirectory(file, cursor, name, sizeof(name), nameLength, info) == Storage::Error::NotDirectory, "对常规文件应返回 NotDirectory");
        Require(mounted.fs.ReadDirectory(file, cursor, nullptr, sizeof(name), nameLength, info) == Storage::Error::InvalidArgument, "名字缓冲为空应返回 InvalidArgument");
        Storage::FileId missing{999999};
        Require(mounted.fs.ReadDirectory(missing, cursor, name, sizeof(name), nameLength, info) != Storage::Error::Ok, "越界标识应上报失败");
    }

    // 跑全部用例并核对数量
    void RunCases() {
        Case("挂载与卷事实", CaseMountFacts);
        Case("读取 32 KiB 常规文件", CaseReadKernel);
        Case("多级目录与奇数长度文件", CaseReadNested);
        Case("空文件", CaseReadEmpty);
        Case("大文件跨多窗口读取", CaseReadBig);
        Case("路径错误映射", CasePathErrors);
        Case("坏 superblock 魔数", CaseBadMagic);
        Case("未知与拒绝特性", CaseUnknownFeatures);
        Case("缺失 FILETYPE 特性", CaseMissingFiletype);
        Case("缺失 EXTENTS 特性", CaseMissingExtents);
        Case("64 位卷高位块数", CaseHighBlocks);
        Case("64 位卷描述符大小", CaseDescSize);
        Case("非干净卷状态位", CaseDirtyState);
        Case("块号越界", CaseOutOfRangeBlock);
        Case("错误卷偏移", CaseWrongVolumeOffset);
        Case("偏移视图非法参数", CaseViewChecks);
        Case("块设备窗口中转", CaseDeviceWindow);
        Case("读盘失败传播", CaseReadFailure);
        Case("未绑定块设备", CaseUnboundDevice);
        Case("写缓存刷新的返回值映射", CaseFlushMapping);
        Case("列出内核集目录", CaseListSetDirectory);
        Case("列出根目录", CaseListRoot);
        Case("目录项名字缓冲过小", CaseListSmallBuffer);
        Case("目录遍历游标", CaseListCursor);
        Case("目录遍历错误映射", CaseListErrors);
        Require(g_ran == kCaseCount, "用例数量与登记不符");
        std::cout << "CheckExt4: " << g_ran << " 项用例全部通过" << std::endl;
    }

    // CheckExt4 的命令行选项
    struct Options {
        fs::path work;         // 工作目录，空表示用临时目录
        bool keep = false;     // 无论成败都保留工作目录
        bool strict = false;   // 缺少 mkfs 视为失败
        bool casesOnly = false;// 只跑用例（当前全部用例都是本地检查，保留给初始化失败时的分步排查）
    };

    // 打印用法
    void Usage() {
        std::cout << "CheckExt4：BaleenCore 的 Ext4 只读实现与块设备适配器的宿主侧检查\n"
                     "用法：CheckExt4 [选项]\n"
                     "  --work <目录>   工作目录，默认建在临时目录下\n"
                     "  --keep          无论成败都保留工作目录\n"
                     "  --strict        缺少 mkfs 时返回失败（默认返回 2）\n"
                     "  -h, --help      显示本说明\n"
                     "环境变量：MKFS 指定卷生成工具（默认 mke2fs）\n"
                     "退出码：0 全部通过、1 有失败、2 缺少 mkfs（未加 --strict）\n";
    }

    // 解析命令行
    Options Parse(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            if (argument == "-h" || argument == "--help") { Usage(); std::exit(0); }
            if (argument == "--keep") { options.keep = true; continue; }
            if (argument == "--strict") { options.strict = true; continue; }
            if (argument == "--cases-only") { options.casesOnly = true; continue; }
            if (argument == "--work") {
                Require(i + 1 < argc, "--work 缺少取值");
                options.work = argv[++i];
                continue;
            }
            Fail(std::string("未知选项：") + std::string(argument));
        }
        return options;
    }

    // 准备本次检查的工作目录
    fs::path MakeWorkDir(const fs::path& requested) {
        if (!requested.empty()) {
            std::error_code error;
            fs::create_directories(requested, error);
            Require(fs::is_directory(requested), "无法创建工作目录 " + requested.string());
            return fs::absolute(requested);
        }
        const fs::path base = fs::temp_directory_path() / ("checkext4." + std::to_string(::getpid()));
        std::error_code error;
        fs::remove_all(base, error);
        fs::create_directories(base, error);
        Require(fs::is_directory(base), "无法创建工作目录 " + base.string());
        return base;
    }
}

// 进程入口
int main(int argc, char** argv) {
    fs::path work;
    bool keepWork = false;
    try {
        const Options options = Parse(argc, argv);
        keepWork = options.keep;
        work = MakeWorkDir(options.work);
        g_work = work;
        MakeFixture();
        RunCases();
        if (keepWork) std::cout << "CheckExt4: 工作目录保留在 " << work.string() << std::endl;
        else fs::remove_all(work);
        std::cout << "CheckExt4: 全部检查通过" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckExt4: 失败：" << error.what() << std::endl;
        if (!work.empty()) std::cerr << "CheckExt4: 工作目录保留在 " << work.string() << "（便于查错）" << std::endl;
        return 1;
    }
}
