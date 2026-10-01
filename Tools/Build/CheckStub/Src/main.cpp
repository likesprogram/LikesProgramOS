/* main.cpp
    CheckStub：Stub 阶段的宿主侧回归，手工执行，不构成默认构建依赖

    用真实的 IPL 与 Stub 产物加内嵌探针 Core 组装镜像，在 QEMU 里核对三类行为：
    成功路径的完整步骤、摘要校验与交权块四个服务在 Core 运行期的可用性；
    Stub 自身自检、描述符区与 Core 段、Core 镜像头各字段的拒绝路径；读盘失败与装载区约束
    失败用例读 0xE9 调试口输出的 FATAL 原因文本，成功用例核对 isa-debug-exit 的退出码
*/

#include <Fixtures.h>

#include <BaleenImage.h>
#include <CoreHandoff.hpp>
#include <ImageHeader.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
    namespace fs = std::filesystem;
    using Bytes = std::vector<uint8_t>;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    // 收到终止信号时记下的信号号，主循环轮询它并退出
    volatile std::sig_atomic_t interrupted = 0;

    // 信号处理：只记录信号号，不在处理函数里做其他事
    void OnSignal(int value) { interrupted = value; }
    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }
    // 已收到终止信号就中止
    void CheckSignal() { if (interrupted) Fail("收到终止信号 " + std::to_string(interrupted)); }
    // 等待毫秒并保持对终止信号的响应
    void Pause(int ms) { ::poll(nullptr, 0, ms); CheckSignal(); }
    // 读环境变量，未设置或为空时取默认值
    std::string Env(const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return value && *value ? value : fallback;
    }

    // 读入整个文件，超过 128 MiB 视为异常
    Bytes ReadBytes(const fs::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        Require(bool(input), "无法读取 " + path.string());
        const auto size = input.tellg();
        Require(size >= 0 && size <= 128 * 1024 * 1024, "文件大小异常 " + path.string());
        Bytes data(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!data.empty()) input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        Require(bool(input), "读取不完整 " + path.string());
        return data;
    }
    // 读入整个文件为文本
    std::string ReadText(const fs::path& path) {
        const auto bytes = ReadBytes(path);
        return std::string(bytes.begin(), bytes.end());
    }
    // 整个覆盖写入二进制内容
    void WriteBytes(const fs::path& path, const Bytes& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(bool(output), "无法写入 " + path.string());
        if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        Require(bool(output), "写入失败 " + path.string());
    }
    // 整个覆盖写入文本
    void WriteText(const fs::path& path, std::string_view text) {
        WriteBytes(path, Bytes(text.begin(), text.end()));
    }
    // 按小端序写入 1..8 字节，越界即中止
    void PutLe(Bytes& data, std::size_t at, uint64_t value, unsigned width) {
        Require(width <= 8 && at <= data.size() && width <= data.size() - at, "夹具写入越界");
        for (unsigned i = 0; i < width; ++i) data[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
    // 按小端序读取 1..8 字节，越界即中止
    uint64_t GetLe(const Bytes& data, std::size_t at, unsigned width) {
        Require(width <= 8 && at <= data.size() && width <= data.size() - at, "夹具读取越界");
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t(data[at + i]) << (8 * i);
        return value;
    }
    // 解析整数，要求整串都是数字
    uint64_t Number(std::string_view text) {
        uint64_t value = 0;
        for (const char c : text) {
            Require(c >= '0' && c <= '9', "无效数字 " + std::string(text));
            value = value * 10 + static_cast<uint64_t>(c - '0');
        }
        return value;
    }
    // 所有期望文本都出现
    bool AllPresent(std::string_view text, const std::vector<std::string>& needles) {
        for (const auto& needle : needles) if (text.find(needle) == std::string_view::npos) return false;
        return true;
    }
    // 把多行文本压成一行，便于放进错误消息
    std::string Quote(std::string_view text) {
        std::string out;
        for (const char c : text) {
            if (c == '\r' || c == '\n') out += ' ';
            else if (c >= 0x20 && c < 0x7F) out += c;
            else out += '.';
        }
        return out;
    }
    // 按 0x 加 8 位大写十六进制格式化，供 NASM 的 -D 使用
    std::string Hex(uint64_t value) {
        std::string text = "0x";
        for (int shift = 28; shift >= 0; shift -= 4) text += "0123456789ABCDEF"[(value >> shift) & 0xF];
        return text;
    }

    // 子进程：单独开进程组，退出时连同整组一起回收
    class Child {
    public:
        // fork/exec 命令，标准输出与标准错误重定向到日志
        Child(const std::vector<std::string>& command, const fs::path& log) {
            Require(!command.empty(), "空命令");
            std::vector<char*> argv;
            for (const auto& arg : command) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            Require(fd >= 0, "无法创建进程日志 " + log.string());
            m_pid = ::fork();
            if (m_pid < 0) { ::close(fd); Fail("fork 失败"); }
            if (m_pid == 0) {
                ::setpgid(0, 0);
                if (::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) _exit(126);
                ::close(fd);
                ::execvp(argv[0], argv.data());
                ::perror(argv[0]);
                _exit(127);
            }
            ::close(fd);
            ::setpgid(m_pid, m_pid);
        }
        // 不允许拷贝：pid 与退出状态是独占的
        Child(const Child&) = delete;
        Child& operator=(const Child&) = delete;
        // 析构时确保子进程已经结束
        ~Child() { Stop(); }
        // 轮询退出状态；还没退出时返回空
        std::optional<int> Status() {
            if (m_status) return m_status;
            int raw = 0;
            const auto result = ::waitpid(m_pid, &raw, WNOHANG);
            if (result == m_pid) m_status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
            else if (result < 0 && errno != EINTR) Fail("waitpid 失败");
            return m_status;
        }
        // 先 SIGTERM 再 SIGKILL 地结束整组，并回收子进程
        void Stop() noexcept {
            if (m_pid <= 0 || m_status) return;
            ::kill(-m_pid, SIGTERM);
            ::kill(m_pid, SIGTERM);
            int raw = 0;
            const auto until = Clock::now() + 1s;
            while (Clock::now() < until) {
                const auto result = ::waitpid(m_pid, &raw, WNOHANG);
                if (result == m_pid || (result < 0 && errno == ECHILD)) { m_status = -1; return; }
                ::poll(nullptr, 0, 10);
            }
            ::kill(-m_pid, SIGKILL);
            ::kill(m_pid, SIGKILL);
            while (::waitpid(m_pid, &raw, 0) < 0 && errno == EINTR) {}
            m_status = -1;
        }
    private:
        pid_t m_pid = -1;             // 子进程号，-1 表示还没 fork
        std::optional<int> m_status;  // 退出码；空表示还没退出
    };

    // 前台执行命令，等待有限时间，输出写日志
    void Run(const std::vector<std::string>& command, const fs::path& log) {
        Child child(command, log);
        const auto deadline = Clock::now() + 60s;
        while (Clock::now() < deadline) {
            CheckSignal();
            if (const auto status = child.Status()) {
                Require(*status == 0, command.front() + " 失败，退出=" + std::to_string(*status) + "\n" + ReadText(log));
                return;
            }
            Pause(10);
        }
        Fail(command.front() + " 执行超时；日志 " + log.string());
    }

    // —— 介质与镜像常量：与一级引导契约、Stub 说明及 CoreHandoff.hpp 一致 ——
    // 描述符区独占一个扇区：头部之后是段序列，每段自带长度；位置只按扇区表达
    constexpr std::size_t kAreaOffset = 0x22000;           // 描述符区在介质上的字节偏移（LBA 272 / 68 / 34）
    constexpr std::size_t kStubSegmentOffset = 0x22010;    // Stub 段：头部 16 字节之后
    constexpr std::size_t kCoreSegmentOffset = 0x22020;    // Core 段：紧随 Stub 段
    constexpr uint32_t kCoreLoad = 0x100000;               // Core 装入的物理地址
    // 探针 Core 的映像长度：跨过 64KiB，让 Stub 的高位装载分批与拷贝路径也被走到
    constexpr std::size_t kProbeImageBytes = 0x11000;
    // isa-debug-exit：写 0x10 时 QEMU 以 (0x10 << 1) | 1 退出
    constexpr int kExitOk = (0x10 << 1) | 1;
    // 描述符区头部字段偏移
    constexpr std::size_t kHeadMagic = 0x00;
    constexpr std::size_t kHeadBytes = 0x06;
    constexpr std::size_t kHeadLba = 0x08;
    constexpr std::size_t kHeadEndLba = 0x0C;
    // 段内字段偏移，各段共用同一套字节表
    constexpr std::size_t kSegMagic = 0x00;
    constexpr std::size_t kSegVersion = 0x04;
    constexpr std::size_t kSegLength = 0x06;
    constexpr std::size_t kSegLba = 0x08;
    constexpr std::size_t kSegEndLba = 0x0C;
    // 用例总数，Execute 末尾核对，防止用例被误删
    constexpr unsigned kCaseCount = 28;

    // 篡改目标：描述符区头部、两枚段、镜像里的 Stub 文件或 Core 文件
    enum class Target {
        DescriptorArea,   // 描述符区头部
        StubSegment,      // Stub 段
        CoreSegment,      // Core 段
        StubFile,         // 镜像里的 Stub 文件
        CoreFile,         // 镜像里的 Core 文件
    };

    // 命令行选项
    struct Options {
        fs::path stubDir;      // Stub 产物目录，含 BaleenStub.bin
        fs::path iplDir;       // IPL 产物目录，含 BaleenIPL.bin 与 BaleenIPLCd.bin
        fs::path root;         // 项目根，用于定位 Tools/Bin；为空时取可执行文件同目录
        bool keep = false;     // 保留工作目录
    };

    // 解析命令行并检查必选参数
    Options Parse(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            const auto value = [&](const char* name) -> std::string {
                Require(i + 1 < argc, std::string(name) + " 缺少参数");
                return argv[++i];
            };
            if (arg == "-h" || arg == "--help") {
                std::cout <<
                    "CheckStub — Stub 阶段的宿主侧回归（QEMU）\n"
                    "\n"
                    "用法：CheckStub --stub-dir <目录> --ipl-dir <目录> [--root <项目根>] [--keep]\n"
                    "\n"
                    "  --stub-dir  BaleenStub.bin 所在目录，通常是 Packages/Baleen/Stub/Out/Bin\n"
                    "  --ipl-dir   BaleenIPL.bin 与 BaleenIPLCd.bin 所在目录，通常是 Packages/Baleen/Ipl/Out/Bin\n"
                    "  --root      项目根；缺省时从可执行文件同目录找 MakeImage / TestInstaller\n"
                    "  --keep      保留每例的日志与证据目录\n"
                    "\n"
                    "环境变量：NASM、QEMU、CHECKSTUB_TEST_TIMEOUT（每例秒数，1..300，默认 12）\n";
                std::exit(0);
            }
            else if (arg == "--stub-dir") options.stubDir = value("--stub-dir");
            else if (arg == "--ipl-dir") options.iplDir = value("--ipl-dir");
            else if (arg == "--root") options.root = value("--root");
            else if (arg == "--keep") options.keep = true;
            else Fail("无法识别的参数：" + std::string(arg));
        }
        Require(!options.stubDir.empty() && !options.iplDir.empty(), "必须提供 --stub-dir 与 --ipl-dir");
        return options;
    }

    // Stub 回归：组装镜像、注入故障、跑 QEMU 并核对
    class Checks {
    public:
        // 记录路径与产物，写出探针夹具
        Checks(Options options, fs::path tools, fs::path work)
            : m_options(std::move(options)), m_tools(std::move(tools)), m_work(std::move(work)),
              m_nasm(Env("NASM", "nasm")), m_qemu(Env("QEMU", "qemu-system-x86_64")), m_stub(ReadBytes(m_options.stubDir / "BaleenStub.bin")) {
            const auto timeout = Number(Env("CHECKSTUB_TEST_TIMEOUT", "12"));
            Require(timeout >= 1 && timeout <= 300, "CHECKSTUB_TEST_TIMEOUT 必须在 1..300 秒内");
            m_timeout = std::chrono::seconds(timeout);
            for (const auto* tool : {"MakeImage", "TestInstaller"}) Require(::access((m_tools / tool).c_str(), X_OK) == 0, "缺少可执行镜像工具 " + (m_tools / tool).string());
            Require(m_stub.size() > hostbuild::ImageHeaderLayout::kEntryOffset, "Stub 产物过小");
            for (const auto* ipl : {"BaleenIPL.bin", "BaleenIPLCd.bin"}) Require(fs::exists(m_options.iplDir / ipl), "缺少 IPL 产物 " + (m_options.iplDir / ipl).string());
            WriteText(m_work / "ProbeCore.asm", checkstub::kProbeCoreAsm);
        }
        // 依次跑成功路径、Stub 自检、描述符与镜像头的拒绝路径、读盘与装载区约束
        void Execute() {
            m_hddBase = ReadBytes(Assemble(false, static_cast<uint32_t>(kProbeImageBytes)));
            m_cdBase = ReadBytes(Assemble(true, static_cast<uint32_t>(kProbeImageBytes)));
            m_4knBase = ReadBytes(Assemble(false, static_cast<uint32_t>(kProbeImageBytes), 4096));

            // 成功路径：完整步骤、摘要校验与交权块四个服务
            Case("hdd 完整引导/交权/四服务", m_hddBase, kExitOk, {"The baleen core image is intact", "CHECKSTUB-CORE-OK services"});
            Case("cd 完整引导/交权/四服务", m_cdBase, kExitOk, {"The baleen core image is intact", "CHECKSTUB-CORE-OK services"}, 128, true);
            // 4Kn：IPL 按 4096 逻辑扇区装载，Stub 的四项服务在同一会话单位下可用
            Case("hdd 4Kn 完整引导/交权/四服务", m_4knBase, kExitOk, {"The baleen core image is intact", "CHECKSTUB-CORE-OK services"}, 128, false, 4096);

            // Stub 自身自检：头字段、链接布局与摘要
            Mutate(Target::StubFile, "stub magic", 0x10, 4, 0, "The image header magic doesn't match");
            Mutate(Target::StubFile, "stub digest", 0x50, 4, 0, "The image digest doesn't match");
            Mutate(Target::StubFile, "stub image bytes", 0x20, 4, 0x1000, "The stub image size doesn't match the linked layout");
            Mutate(Target::StubFile, "stub memory bytes", 0x24, 4, 0x9000, "The stub memory size doesn't match the linked layout");

            // 描述符区头部：IPL 先于 Stub 校验，破坏在这里会由 IPL 以 D 停机
            Mutate(Target::DescriptorArea, "area magic", kHeadMagic, 4, 0, "D");
            Mutate(Target::DescriptorArea, "area head bytes", kHeadBytes, 2, 4, "D");
            Mutate(Target::DescriptorArea, "area sector", kHeadLba, 4, 1, "D");
            // 本区结束扇区只由 Stub 校验
            Mutate(Target::DescriptorArea, "area end inverted", kHeadEndLba, 4, 0, "The CoreDescriptor area range is inverted");

            // Stub 段：段长与标记由 IPL 的遍历先看到
            Mutate(Target::StubSegment, "stub segment length", kSegLength, 2, 0, "D");
            Mutate(Target::StubSegment, "stub segment magic", kSegMagic, 4, 0, "D");

            // Core 段：IPL 不读它，由 Stub 校验
            Mutate(Target::CoreSegment, "core segment lba zero", kSegLba, 4, 0, "The CoreDescriptor LBA is zero");
            Mutate(Target::CoreSegment, "core segment lba overlap", kSegLba, 4, 272, "The CoreDescriptor overlaps the descriptor sector");
            Mutate(Target::CoreSegment, "core segment end inverted", kSegEndLba, 4, 0, "The CoreDescriptor range is inverted");

            // Core 镜像头：与 Stub 同构的头字段与入口前缀
            using Head = hostbuild::ImageHeaderLayout;
            Mutate(Target::CoreFile, "core magic", Head::kOffset + Head::kFieldMagic, 4, 0, "The image header magic doesn't match");
            Mutate(Target::CoreFile, "core version", Head::kOffset + Head::kFieldVersion, 2, 2, "The image header version or header length isn't supported");
            Mutate(Target::CoreFile, "core flags", Head::kOffset + Head::kFieldFlags, 4, 1, "The image header flags aren't supported");
            Mutate(Target::CoreFile, "core digest algorithm", Head::kOffset + Head::kFieldDigestAlgorithm, 2, 2, "The image header digest algorithm isn't supported");
            Mutate(Target::CoreFile, "core build id length", Head::kOffset + Head::kFieldBuildIdBytes, 2, 16, "The image header build id length isn't supported");
            Mutate(Target::CoreFile, "core reserved", Head::kOffset + Head::kFieldReserved, 4, 1, "The image header reserved bytes aren't zero");
            Mutate(Target::CoreFile, "core entry prefix", 0x00, 1, 0x90, "The image entry prefix doesn't match the entry offset");
            Mutate(Target::CoreFile, "core image bytes", Head::kOffset + Head::kFieldImageBytes, 4, 0x1000, "The CoreDescriptor sector count doesn't match the core header");
            Mutate(Target::CoreFile, "core memory bytes", Head::kOffset + Head::kFieldMemoryBytes, 4, 0x400001, "The CoreMemoryBytes is beyond the limit");

            // 摘要不符：只改代码字节，头与描述符保持一致
            Mutate(Target::CoreFile, "core digest mismatch", 0x100, 4, 0, "The image digest doesn't match");

            // 读盘失败：Core 段的起止扇区一起指向介质之外，两者保持自洽
            {
                Bytes image = m_hddBase;
                const uint64_t outside = static_cast<uint64_t>(image.size()) / 512;
                PutLe(image, kCoreSegmentOffset + kSegLba, outside, 4);
                PutLe(image, kCoreSegmentOffset + kSegEndLba, outside + 4, 4);
                Case("core unreadable", image, std::nullopt, {"Read the BaleenCore header"});
            }

            // 装载区不可用：静态内存跨度到上限，虚拟机内存压到 4 MiB
            Case("hdd 装载区不可用", ReadBytes(Assemble(false, 0x400000)), std::nullopt, {"The BaleenCore load area isn't usable memory"}, 4);

            Require(m_count == kCaseCount, "内部回归用例数量不符");
            std::cout << "CheckStub: " << m_count << " 项全部通过" << std::endl;
        }
    private:
        // 用 NASM 汇编夹具源码，definitions 里的每项加一个 -D 定义
        void Assemble(std::string_view source, const fs::path& target, const std::vector<std::pair<std::string, uint64_t>>& definitions) {
            const auto file = m_work / "fixture.asm";
            WriteText(file, source);
            std::vector<std::string> command{m_nasm, "-f", "bin", "-Ox"};
            for (const auto& [name, value] : definitions) command.push_back("-D" + name + "=" + Hex(value));
            command.insert(command.end(), {"-o", target.string(), file.string()});
            Run(command, m_work / "nasm.log");
        }
        // 组装探针 Core：入口前缀、保留区、完整性头与探针代码，补零后填身份与摘要
        Bytes BuildProbe(uint32_t sectorBytes, uint32_t memoryBytes) {
            const auto code = m_work / "probe-core-code.bin";
            Assemble(checkstub::kProbeCoreAsm, code, {
                {"CORE_ENTRY", kCoreLoad + hostbuild::ImageHeaderLayout::kEntryOffset},
                {"EXPECT_SECT", sectorBytes},
                {"HANDOFF_MAGIC", Baleen::CoreHandoffLayout::kMagic},
                {"HANDOFF_VERSION", Baleen::CoreHandoffLayout::kVersion},
                {"HANDOFF_BYTES", Baleen::CoreHandoffLayout::kBytes},
                {"HANDOFF_SECTOR_BYTES", Baleen::CoreHandoffLayout::kSectorBytes},
                {"HANDOFF_WRITE", Baleen::CoreHandoffLayout::kWrite},
                {"HANDOFF_READ", Baleen::CoreHandoffLayout::kReadSectors},
                {"HANDOFF_SECT", Baleen::CoreHandoffLayout::kSectorSize},
                {"HANDOFF_QUERY", Baleen::CoreHandoffLayout::kMemoryMapQuery},
            });
            const Bytes body = ReadBytes(code);
            const std::size_t entryAt = hostbuild::ImageHeaderLayout::kEntryOffset;
            Require(body.size() + entryAt <= kProbeImageBytes, "探针 Core 代码超出映像长度上限");
            Bytes image(kProbeImageBytes, 0);
            // 入口前缀与头字段：与 MakePayloads 组装的占位 Core 同一套布局与填充顺序
            image[0] = hostbuild::ImageHeaderLayout::kNearJumpOpcode;
            PutLe(image, 1, entryAt - hostbuild::ImageHeaderLayout::kPrefixBytes, 2);
            std::copy(body.begin(), body.end(), image.begin() + static_cast<std::ptrdiff_t>(entryAt));
            const std::size_t head = hostbuild::ImageHeaderLayout::kOffset;
            const uint8_t magic[8] = {'B', 'L', 'N', 'C', 'O', 'R', 'E', 0};
            std::copy(std::begin(magic), std::end(magic), image.begin() + static_cast<std::ptrdiff_t>(head));
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldVersion, hostbuild::ImageHeaderLayout::kVersion, 2);
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldHeaderBytes, hostbuild::ImageHeaderLayout::kBytes, 2);
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldFlags, hostbuild::ImageHeaderLayout::kFlags, 4);
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldImageBytes, image.size(), 4);
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldMemoryBytes, memoryBytes, 4);
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldEntryOffset, entryAt, 4);
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldDigestAlgorithm, hostbuild::ImageHeaderLayout::kDigestSha256, 2);
            PutLe(image, head + hostbuild::ImageHeaderLayout::kFieldBuildIdBytes, hostbuild::ImageHeaderLayout::kBuildIdBytes, 2);
            hostbuild::FillImageIdentity(image);
            hostbuild::VerifyImage(image, hostbuild::ImageKind::Core);
            return image;
        }
        // 用 MakeImage / TestInstaller 组装一份镜像：真实 Stub 加探针 Core
        // sectorBytes 为本地逻辑扇区大小；0 表示按介质推断：光盘 2048、磁盘 512
        fs::path Assemble(bool cd, uint32_t memoryBytes, uint32_t sectorBytes = 0) {
            if (sectorBytes == 0) sectorBytes = cd ? 2048 : 512;
            const auto probe = m_work / "probe-core.bin";
            WriteBytes(probe, BuildProbe(sectorBytes, memoryBytes));
            const auto stub = m_work / "stub.bin";
            WriteBytes(stub, m_stub);
            const auto image = m_work / (cd ? "base.iso" : "base.hdd");
            std::vector<std::string> command{(m_tools / (cd ? "MakeImage" : "TestInstaller")).string(), "--out", image.string()};
            if (cd) command.insert(command.end(), {"--boot-image", (m_options.iplDir / "BaleenIPLCd.bin").string()});
            else command.insert(command.end(), {"--mbr", (m_options.iplDir / "BaleenIPL.bin").string()});
            // 4Kn 用例：分区表按 4096 编码，镜像留足容量（固件不从小容量 NVMe 引导）
            if (!cd && sectorBytes == 4096) command.insert(command.end(), {"--sector-bytes", "4096", "--pad-to", "67108864"});
            command.insert(command.end(), {"--stub", stub.string(), "--core", probe.string()});
            Run(command, m_work / "image.log");
            return image;
        }
        // 在镜像副本上按目标位置改一个字段，生成故障镜像并跑一例
        void Mutate(Target target, const char* name, std::size_t pos, unsigned width, uint64_t value, const char* expected) {
            Bytes image = m_hddBase;
            std::size_t base = 0;
            switch (target) {
                case Target::DescriptorArea: base = kAreaOffset; break;
                case Target::StubSegment: base = kStubSegmentOffset; break;
                case Target::CoreSegment: base = kCoreSegmentOffset; break;
                // 载荷位置只按扇区存：从段里取起始扇区，按本地单位换算成字节位置
                case Target::StubFile: base = static_cast<std::size_t>(GetLe(image, kStubSegmentOffset + kSegLba, 4)) * 512; break;
                case Target::CoreFile: base = static_cast<std::size_t>(GetLe(image, kCoreSegmentOffset + kSegLba, 4)) * 512; break;
            }
            PutLe(image, base + pos, value, width);
            Case(name, image, std::nullopt, {expected});
        }
        // 跑一个启动用例并核对结果
        void Case(const std::string& name, const Bytes& image, std::optional<int> expectExit, const std::vector<std::string>& expectText, int memoryMiB = 128, bool cd = false, uint32_t sectorBytes = 0) {
            CheckSignal();
            const auto local = m_work / ("case-" + std::to_string(m_count + 1));
            fs::create_directory(local);
            WriteText(local / "case.txt", name + "\n");
            const auto path = local / "image.bin";
            WriteBytes(path, image);
            RunQemu(path, expectExit, expectText, local, memoryMiB, cd, sectorBytes);
            ++m_count;
            std::cout << "CheckStub: " << m_count << "/" << kCaseCount << " " << name << " 通过" << std::endl;
        }
        // 启动 QEMU：成功路径等进程按 isa-debug-exit 退出，失败路径等 FATAL 原因文本出现
        void RunQemu(const fs::path& image, std::optional<int> expectExit, const std::vector<std::string>& expectText, const fs::path& work, int memoryMiB, bool cd, uint32_t sectorBytes) {
            const auto debug = work / "debug.log";
            WriteText(debug, "");
            std::vector<std::string> command{m_qemu, "-machine", "q35", "-m", std::to_string(memoryMiB), "-display", "none",
                "-serial", "none", "-monitor", "none", "-no-reboot", "-snapshot",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                "-debugcon", "file:" + debug.string()};
            if (cd) command.insert(command.end(), {"-drive", "if=ide,media=cdrom,format=raw,readonly=on,file=" + image.string(), "-boot", "order=d"});
            else if (sectorBytes == 4096) command.insert(command.end(), {"-drive", "file=" + image.string() + ",format=raw,if=none,id=disk",
                "-device", "nvme,drive=disk,serial=checkstub4kn,logical_block_size=4096,physical_block_size=4096", "-boot", "order=c"});
            else command.insert(command.end(), {"-drive", "file=" + image.string() + ",format=raw,if=ide", "-boot", "order=c"});
            Child child(command, work / "qemu.log");
            const auto deadline = Clock::now() + m_timeout;
            while (Clock::now() < deadline) {
                CheckSignal();
                const auto output = ReadText(debug);
                if (const auto status = child.Status()) {
                    Require(expectExit.has_value() && *status == *expectExit,
                            "退出=" + std::to_string(*status) + "，输出=" + Quote(output) + "\n" + ReadText(work / "qemu.log"));
                    Require(AllPresent(output, expectText), "缺少期望文本\n" + output);
                    WriteText(work / "console.txt", output);
                    return;
                }
                if (!expectExit.has_value() && AllPresent(output, expectText)) {
                    // 期望的 FATAL 行已经出现：留一段时间确认输出不再变化，随后结束这一例
                    Pause(300);
                    const auto settled = ReadText(debug);
                    Require(AllPresent(settled, expectText), "期望文本随后消失\n" + settled);
                    WriteText(work / "console.txt", settled);
                    return;
                }
                Pause(20);
            }
            Fail("启动超时，输出=" + Quote(ReadText(debug)));
        }
        Options m_options;              // 命令行选项
        fs::path m_tools, m_work;       // 工具目录与临时工作目录
        std::string m_nasm, m_qemu;     // 汇编器与模拟器的可执行文件
        std::chrono::seconds m_timeout{12};   // 每例启动的超时
        Bytes m_stub;                   // Stub 产物
        Bytes m_hddBase, m_cdBase, m_4knBase;   // 三份基础镜像，故障用例在副本上改字节
        unsigned m_count = 0;           // 已通过的用例数
    };
}

// 入口：解析参数、建临时工作目录并跑全部用例
int main(int argc, char** argv) {
    fs::path work;
    try {
        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
        std::signal(SIGHUP, OnSignal);
        const auto options = Parse(argc, argv);
        const fs::path tools = options.root.empty() ? fs::canonical("/proc/self/exe").parent_path() : options.root / "Tools" / "Bin";
        std::array<char, 64> pattern{};
        std::strcpy(pattern.data(), "/tmp/checkstub.XXXXXX");
        const char* directory = ::mkdtemp(pattern.data());
        Require(directory != nullptr, "无法创建临时工作目录");
        work = directory;
        Checks(options, tools, work).Execute();
        if (options.keep) std::cout << "CheckStub: 工作目录 " << work << '\n';
        else fs::remove_all(work);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckStub: " << error.what() << '\n';
        if (!work.empty()) std::cerr << "CheckStub: 失败证据保留在 " << work << '\n';
        return interrupted ? 128 + interrupted : 1;
    }
}
