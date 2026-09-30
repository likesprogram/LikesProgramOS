/* main.cpp
    CheckIpl：IPL 的静态布局校验与 QEMU 启动回归，按需手工执行，不构成默认构建依赖
*/

#include <Fixtures.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
    namespace fs = std::filesystem;
    using Bytes = std::vector<uint8_t>;
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;
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
    void WriteBytes(const fs::path& path, std::span<const uint8_t> bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(bool(output), "无法写入 " + path.string());
        if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        Require(bool(output), "写入失败 " + path.string());
    }
    // 整个覆盖写入文本
    void WriteText(const fs::path& path, std::string_view text) {
        WriteBytes(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    }
    // 按小端序写入 1..8 字节，越界即中止
    void PutLe(Bytes& data, std::size_t at, unsigned width, uint64_t value) {
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
    uint64_t Number(std::string_view text, int base = 10) {
        uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
        Require(error == std::errc{} && end == text.data() + text.size(), "无效数字 " + std::string(text));
        return value;
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
            if (result == m_pid) {
                m_status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
            } else if (result < 0 && errno != EINTR) {
                Fail("waitpid 失败");
            }
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

    // 仅处理本工具的 QMP 行消息：单个命令在途，用 id 匹配响应并跳过异步事件
    // 协议依据：https://www.qemu.org/docs/master/interop/qmp-spec.html
    // 把文本编码为 JSON 字符串字面量
    std::string Quote(std::string_view text) {
        std::string result = "\"";
        const char* hex = "0123456789abcdef";
        for (unsigned char c : text) {
            if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
            else if (c < 0x20) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
            else result += char(c);
        }
        return result + '"';
    }
    // 返回 JSON 字符串的结束位置（收尾引号之后），不合法即中止
    std::size_t StringEnd(std::string_view text, std::size_t begin) {
        Require(begin < text.size() && text[begin] == '"', "QMP 字符串缺少引号");
        for (std::size_t pos = begin + 1; pos < text.size(); ++pos) {
            if (text[pos] == '\\') ++pos;
            else if (text[pos] == '"') return pos + 1;
        }
        Fail("QMP 字符串未结束");
    }
    // 解码 JSON 字符串，含转义与 UTF-16 代理对
    std::string Unquote(std::string_view text) {
        Require(text.size() >= 2 && StringEnd(text, 0) == text.size(), "QMP 返回值不是字符串");
        std::string result;
        for (std::size_t pos = 1; pos + 1 < text.size(); ++pos) {
            char c = text[pos];
            if (c != '\\') { result += c; continue; }
            Require(++pos + 1 < text.size(), "QMP 字符串转义不完整");
            c = text[pos];
            switch (c) {
            case '"': case '\\': case '/': result += c; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                Require(pos + 4 < text.size() - 1, "QMP Unicode 转义不完整");
                uint32_t cp = static_cast<uint32_t>(Number(text.substr(pos + 1, 4), 16));
                pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    Require(pos + 6 < text.size() - 1 && text.substr(pos + 1, 2) == "\\u", "QMP 缺少低代理项");
                    const auto low = Number(text.substr(pos + 3, 4), 16);
                    Require(low >= 0xDC00 && low <= 0xDFFF, "QMP 低代理项非法");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + static_cast<uint32_t>(low - 0xDC00);
                    pos += 6;
                } else Require(cp < 0xDC00 || cp > 0xDFFF, "QMP 孤立低代理项");
                if (cp < 0x80) result += char(cp);
                else if (cp < 0x800) { result += char(0xC0 | (cp >> 6)); result += char(0x80 | (cp & 63)); }
                else if (cp < 0x10000) {
                    result += char(0xE0 | (cp >> 12)); result += char(0x80 | ((cp >> 6) & 63)); result += char(0x80 | (cp & 63));
                } else {
                    result += char(0xF0 | (cp >> 18)); result += char(0x80 | ((cp >> 12) & 63));
                    result += char(0x80 | ((cp >> 6) & 63)); result += char(0x80 | (cp & 63));
                }
                break;
            }
            default: Fail("QMP 不认识的字符串转义");
            }
        }
        return result;
    }
    // 跳过空白字符
    void Space(std::string_view text, std::size_t& pos) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) ++pos;
    }
    // 取出 QMP 对象顶层字段的原始值；没有该字段返回空
    std::optional<std::string_view> Field(std::string_view text, std::string_view name) {
        std::size_t pos = 0;
        Space(text, pos);
        Require(pos < text.size() && text[pos++] == '{', "QMP 消息不是对象");
        while (pos < text.size()) {
            Space(text, pos);
            if (pos < text.size() && text[pos] == '}') return std::nullopt;
            const auto keyEnd = StringEnd(text, pos);
            const auto key = Unquote(text.substr(pos, keyEnd - pos));
            pos = keyEnd;
            Space(text, pos);
            Require(pos < text.size() && text[pos++] == ':', "QMP 字段缺少冒号");
            Space(text, pos);
            const auto begin = pos;
            unsigned depth = 0;
            for (; pos < text.size(); ++pos) {
                const char c = text[pos];
                if (c == '"') { pos = StringEnd(text, pos) - 1; continue; }
                if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') { if (!depth) break; --depth; }
                else if (c == ',' && !depth) break;
            }
            auto end = pos;
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
            if (key == name) return text.substr(begin, end - begin);
            if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
            Require(pos < text.size() && text[pos] == '}', "QMP 对象未结束");
            return std::nullopt;
        }
        Fail("QMP 对象不完整");
    }

    // QEMU 监视器（QMP）连接：按行收发 JSON 消息
    class Qmp {
    public:
        // 连接 socket 并完成 capabilities 握手；不可用返回空
        static std::unique_ptr<Qmp> Connect(const fs::path& path, Deadline deadline) {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            const std::string name = path.string();
            Require(name.size() < sizeof(address.sun_path), "QMP socket 路径过长");
            std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            Require(fd >= 0, "无法创建 QMP socket");
            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
                const int error = errno;
                ::close(fd);
                if (error == ENOENT || error == ECONNREFUSED || error == EINTR) return nullptr;
                Fail("连接 QMP 失败：" + std::string(std::strerror(error)));
            }
            auto qmp = std::unique_ptr<Qmp>(new Qmp(fd));
            Require(::fcntl(fd, F_SETFL, O_NONBLOCK) == 0, "设置 QMP 非阻塞失败");
            const auto greeting = qmp->Line(deadline);
            Require(Field(greeting, "QMP").has_value(), "QMP greeting 非法");
            qmp->Query("qmp_capabilities", "{}", deadline);
            return qmp;
        }
        // 关闭连接
        ~Qmp() { ::close(m_fd); }
        // 发一条命令，按 id 匹配响应并跳过异步事件
        std::string Query(std::string_view command, std::string_view arguments, Deadline deadline) {
            const std::string id = std::to_string(++m_id);
            std::string request = "{\"execute\":" + Quote(command) + ",\"arguments\":" + std::string(arguments) + ",\"id\":" + id + "}\r\n";
            std::size_t offset = 0;
            while (offset < request.size()) {
                Wait(POLLOUT, deadline);
                const auto sent = ::send(m_fd, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
                if (sent < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                Require(sent > 0, "QMP 发送失败");
                offset += static_cast<std::size_t>(sent);
            }
            while (true) {
                const std::string response = Line(deadline);
                if (Field(response, "event")) continue;
                const auto responseId = Field(response, "id");
                if (responseId && *responseId != id) continue;
                if (Field(response, "error")) Fail("QMP 命令失败：" + response);
                Require(responseId.has_value(), "QMP 响应缺少 id");
                const auto result = Field(response, "return");
                Require(result.has_value(), "QMP 响应缺少 return");
                return std::string(*result);
            }
        }
        // 走 human-monitor-command 执行监视器命令，返回其文本输出
        std::string Monitor(std::string_view command, Deadline deadline) {
            return Unquote(Query("human-monitor-command", "{\"command-line\":" + Quote(command) + "}", deadline));
        }
    private:
        // 接管已经连上的 socket
        explicit Qmp(int fd) : m_fd(fd) {}
        // 等到事件就绪、连接断开或超时
        void Wait(short events, Deadline deadline) {
            while (Clock::now() < deadline) {
                CheckSignal();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                pollfd state{m_fd, events, 0};
                const int ready = ::poll(&state, 1, static_cast<int>(std::clamp<int64_t>(ms, 1, 100)));
                if (ready < 0 && errno == EINTR) continue;
                Require(ready >= 0, "QMP poll 失败");
                if (state.revents & events) return;
                Require(!(state.revents & (POLLERR | POLLHUP | POLLNVAL)), "QMP 已断开");
            }
            Fail("QMP 响应超时");
        }
        // 读一行完整消息，必要时继续收数据
        std::string Line(Deadline deadline) {
            while (true) {
                const auto end = m_buffer.find('\n');
                if (end != std::string::npos) {
                    std::string line = m_buffer.substr(0, end);
                    m_buffer.erase(0, end + 1);
                    return line;
                }
                Wait(POLLIN, deadline);
                std::array<char, 4096> chunk{};
                const auto size = ::recv(m_fd, chunk.data(), chunk.size(), 0);
                if (size < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                Require(size > 0, "QMP 接收失败或提前关闭");
                m_buffer.append(chunk.data(), static_cast<std::size_t>(size));
                Require(m_buffer.size() <= 1024 * 1024, "QMP 行消息过大");
            }
        }
        int m_fd;              // 已连接的 socket 描述符
        unsigned m_id = 0;     // 请求序号，用来匹配响应
        std::string m_buffer;  // 已收到但还没取走的字节
    };

    // 从汇编清单里读 Code_End 之后的地址，即代码与数据末端
    std::size_t CodeEnd(const fs::path& listing) {
        std::istringstream lines(ReadText(listing));
        const std::regex address(R"(^\s*\d+\s+([0-9A-Fa-f]{8})\s+)");
        bool pending = false;
        for (std::string line; std::getline(lines, line);) {
            if (line.find("Code_End:") != std::string::npos) pending = true;
            std::smatch match;
            if (pending && std::regex_search(line, match, address)) return Number(match[1].str(), 16);
        }
        Fail(listing.string() + "：找不到 Code_End 后的地址");
    }
    // 静态检查两份 IPL 产物的长度、签名、代码上限与保留区
    void CheckLayout(const fs::path& out) {
        for (const bool cd : {false, true}) {
            const std::string name = cd ? "BaleenIPLCd" : "BaleenIPL";
            const std::size_t size = cd ? 2048 : 512;
            const std::size_t limit = cd ? 510 : 446;
            const auto data = ReadBytes(out / "Bin" / (name + ".bin"));
            const auto end = CodeEnd(out / "Obj" / (name + ".lst"));
            Require(data.size() == size, name + "：产物长度错误");
            Require(data[510] == 0x55 && data[511] == 0xAA, name + "：偏移 510 的签名错误");
            Require(end <= limit, name + "：代码越界");
            // 判断一段字节是否全为 0
            const auto zero = [](auto first, auto last) { return std::all_of(first, last, [](uint8_t b) { return b == 0; }); };
            Require(zero(data.begin() + end, data.begin() + limit), name + "：尾部填充错误");
            if (cd) {
                Require(limit - end >= 16, "CD 代码余量低于 16 字节");
                Require(zero(data.begin() + 8, data.begin() + 64), "CD Boot Info Table 保留区被占用");
                Require(zero(data.begin() + 512, data.end()), "CD 后三段不可放 IPL 代码/数据");
            }
            std::cout << "CheckIpl: " << name << " " << size << " B，代码末端 " << end << '/' << limit
                      << "，余量 " << limit - end << " B，签名位于 510\n";
        }
    }
    // 定位 IPL 里唯一的失败停机序列，返回 HLT 之后的 IP
    std::size_t HaltIp(const Bytes& firmware) {
        const std::array<uint8_t, 4> loop{0xFA, 0xF4, 0xEB, 0xFD};
        const auto end = firmware.begin() + std::min<std::size_t>(firmware.size(), 510);
        const auto found = std::search(firmware.begin(), end, loop.begin(), loop.end());
        Require(found != end, "IPL 中找不到 CLI/HLT/循环跳转的失败停机序列");
        Require(std::search(found + 1, end, loop.begin(), loop.end()) == end, "IPL 失败停机序列不唯一");
        return static_cast<std::size_t>(found - firmware.begin()) + 2; // HLT 后的 IP
    }
    // 按正则从寄存器转储里取一个十六进制值
    uint64_t Register(const std::string& text, const std::string& pattern) {
        std::smatch match;
        Require(std::regex_search(text, match, std::regex(pattern)), "QMP 缺少寄存器：" + pattern + "\n" + text);
        return Number(match[1].str(), 16);
    }

    // 命令行选项
    struct Options {
        fs::path iplDir;    // IPL 产物目录（.../Out/Bin）
        fs::path root;      // 项目根；空 = 从可执行文件同目录找工具
        fs::path layout;    // 只做静态检查时的 IPL 输出目录
        bool e9 = true;     // 是否要求 0xE9 也有错误码
        bool keep = false;  // 是否保留临时工作目录
    };
    // 解析命令行并检查必选参数
    Options Parse(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            // 取当前选项的值，缺值即报错
            const auto value = [&]() -> std::string {
                Require(i + 1 < argc, option + " 缺少值");
                return argv[++i];
            };
            if (option == "--ipl-dir") options.iplDir = fs::absolute(value());
            else if (option == "--root") options.root = fs::absolute(value());
            else if (option == "--layout-only") options.layout = fs::absolute(value());
            else if (option == "--keep") options.keep = true;
            else if (option == "--e9") {
                const auto mode = value();
                Require(mode == "0" || mode == "1", "--e9 只能为 0 或 1");
                options.e9 = mode == "1";
            } else if (option == "--help") {
                std::cout << "CheckIpl --layout-only <IplOut>\n"
                          << "CheckIpl --ipl-dir <IplOut/Bin> [--e9 0|1] [--keep] [--root <项目根>]\n"
                          << "默认从可执行文件同目录寻找 MakeHdd/MakeIso；--root 改用其 Tools/Bin。\n"
                          << "NASM/QEMU 可指定外部程序，IPL_TEST_TIMEOUT 为每例启动秒数（默认 12）。\n"
                          << "手工运行，不构成默认构建依赖；失败保留临时证据。\n";
                std::exit(0);
            } else Fail("未知参数 " + option);
        }
        Require(options.layout.empty() != options.iplDir.empty(), "必须且只能指定 --ipl-dir 或 --layout-only");
        return options;
    }

    // 启动回归：造镜像、跑 QEMU，核对退出码、停机位置与屏幕错误码
    class BootChecks {
    public:
        // 记录选项与工作目录，读入 IPL 产物，写出两个夹具源码
        BootChecks(Options options, fs::path tools, fs::path work)
            : m_options(std::move(options)), m_tools(std::move(tools)), m_work(std::move(work)),
              m_nasm(Env("NASM", "nasm")), m_qemu(Env("QEMU", "qemu-system-x86_64")) {
            const auto timeout = Number(Env("IPL_TEST_TIMEOUT", "12"));
            Require(timeout >= 1 && timeout <= 300, "IPL_TEST_TIMEOUT 必须在 1..300 秒内");
            m_timeout = std::chrono::seconds(timeout);
            for (const auto* tool : {"MakeHdd", "MakeIso"}) Require(::access((m_tools / tool).c_str(), X_OK) == 0, "缺少可执行镜像工具 " + (m_tools / tool).string());
            m_hdd = ReadBytes(m_options.iplDir / "BaleenIPL.bin");
            m_cd = ReadBytes(m_options.iplDir / "BaleenIPLCd.bin");
            Require(m_hdd.size() == 512 && m_cd.size() == 2048, "IPL 产物尺寸不符");
            m_hddHalt = HaltIp(m_hdd);
            m_cdHalt = HaltIp(m_cd);
            WriteText(m_work / "Probe.asm", checkipl::kProbeAsm);
            WriteText(m_work / "BiosFault.asm", checkipl::kFaultAsm);
        }
        // 依次跑交接用例、描述符字段用例、混合与 USB 用例、故障注入用例
        void Execute() {
            for (const bool cd : {false, true}) {
                const std::string media = cd ? "cd" : "hdd";
                const uint32_t maximum = cd ? 0x8000 : 0x8200;
                for (uint32_t size : {4096u, maximum - 1, maximum}) Case(media + " 交接/完整装载 " + std::to_string(size) + " B", Build(cd, size), media, "OK", cd);
                const auto base = Build(cd);
                // 一处描述符字段破坏：偏移、宽度、写入值与期望错误码
                struct Mutation {
                    const char* name;  // 用例名
                    unsigned pos;      // 描述符内的字节偏移
                    unsigned width;    // 字段宽度（字节）
                    uint64_t value;    // 写入值
                    const char* code;  // 期望的 IPL 错误码
                };
                const std::vector<Mutation> mutations{
                    {"magic", 0, 4, 0, "D"}, {"version", 4, 2, 2, "D"}, {"header", 6, 2, 31, "D"},
                    {"reserved-low", 24, 4, 1, "D"}, {"reserved-high", 28, 4, 1, "D"},
                    {"unaligned", 8, 8, 2049, "D"}, {"offset-high", 8, 8, 0x100000000ull, "D"},
                    {"offset-overlap", 8, 8, 0, "D"}, {"offset-overflow", 8, 8, 0xFFFFF800, "D"},
                    {"size-zero", 16, 8, 0, "S"}, {"size-high", 16, 8, 0x100000000ull, "S"},
                    {"size-over-limit", 16, 8, maximum + 1, "S"}, {"size-round-overflow", 16, 8, 0xFFFFFFFF, "S"},
                    {"unreadable-stub", 8, 8, 0x20000000, "L"}
                };
                for (const auto& mutation : mutations) {
                    auto bad = ReadBytes(base);
                    PutLe(bad, 0x22000 + mutation.pos, mutation.width, mutation.value);
                    const auto image = m_work / "bad.img";
                    WriteBytes(image, bad);
                    Case(media + " " + mutation.name + " -> " + mutation.code, image, media, mutation.code, cd);
                }
            }
            Case("混合 ISO 光盘入口", Build(true, 4096, true), "cd", "OK", true);
            auto hybridPath = Build(true, 4096, true);
            auto hybrid = ReadBytes(hybridPath);
            // 从描述符里取 Stub 的位置与长度（描述符在当前布局的固定偏移 0x22000）
            const auto offset = GetLe(hybrid, 0x22008, 8);
            const auto size = GetLe(hybrid, 0x22010, 8);
            const auto stub = ReadBytes(Probe(static_cast<uint32_t>(size), 2, 0x80));
            Require(offset <= hybrid.size() && size <= hybrid.size() - offset, "混合镜像 Stub 超出文件");
            std::copy(stub.begin(), stub.end(), hybrid.begin() + offset);
            hybrid.resize(std::max<std::size_t>(hybrid.size(), 1 << 20), 0);
            WriteBytes(hybridPath, hybrid);
            Case("混合 ISO USB/BIOS", hybridPath, "usb", "OK", false);
            Case("HDD USB/BIOS", Build(false), "usb", "OK", false);

            for (const bool cd : {false, true}) {
                for (const auto& fault : std::vector<std::pair<unsigned, std::string>>{
                         {0, "寄存器破坏"}, {2, "批量失败/DAP改写"}, {3, "前两次失败后恢复"}, {4, "永久失败"},
                         {cd ? 5u : 1u, cd ? "CD备用E1探测" : "无 EDD"}}) {
                    // HDD 的"无 EDD"与"永久失败"都读不到描述符，期望 R；CD 的永久失败期望 D
                    const std::string expected = fault.first == 4 ? (cd ? "D" : "R") : (fault.first == 1 ? "R" : "OK");
                    Fault(cd, fault.first, fault.second, 4096, fault.first == 5 ? 0 : (cd ? 0xE0 : 0x80), fault.first == 5 ? 0xE1 : (cd ? 0xE0 : 0x80), expected);
                }
            }
            Fault(false, 2, "最大 Stub 批量失败后完整重读/BX回绕", 0x8200, 0x80, 0x80, "OK");
            Fault(true, 2, "最大 Stub 批量失败后完整重读", 0x8000, 0xE0, 0xE0, "OK");
            Fault(true, 0, "原始 DL 无效，E0 回退", 4096, 0, 0xE0, "OK");
            Fault(true, 0, "原始 DL=90 优先并保留", 4096, 0x90, 0x90, "OK");
            // 全部候选失败时对 E1 只探测一轮：一次单扇区探测含 3 次批量尝试与 3 次逐扇区回退，
            // 旧行为在 E0 失败后会重复探测 E1，计数翻倍
            Fault(true, 6, "原始 DL=E1 只探测一轮 -> D", 4096, 0xE1, 0xE0, "D", 6);
            // AH=48：失败回落 512，短表与未知大小拒绝，报告 4096 时整条链按 4Kn 装载
            Fault(false, 0, "AH48 查询失败回落 512", 4096, 0x80, 0x80, "OK", 0, 512, 2);
            Fault(false, 0, "AH48 短表 -> D", 4096, 0x80, 0x80, "D", 0, 512, 3);
            Fault(false, 0, "AH48 未知扇区大小 -> D", 4096, 0x80, 0x80, "D", 0, 512, 4);
            Fault(false, 0, "AH48 报告 4Kn 完整装载", 4096, 0x80, 0x80, "OK", 0, 4096, 1);
            // 软盘与 USB-FDD 的 DL<0x80 不在支持范围：HDD 入口直接拒绝
            Fault(false, 0, "HDD DL<0x80 -> D", 4096, 0x00, 0x00, "D");
            Require(m_count == 57, "内部回归用例数量不符");
            std::cout << "CheckIpl: " << m_count << " 项全部通过（ENABLE_E9=" << m_options.e9 << "）" << std::endl;
        }
    private:
        // 用 NASM 汇编夹具，definitions 里的每项加一个 -D 定义
        void Assemble(const fs::path& source, const fs::path& target, const std::vector<std::pair<std::string, unsigned>>& definitions) {
            std::vector<std::string> command{m_nasm, "-f", "bin", "-Ox"};
            for (const auto& [name, value] : definitions) command.push_back("-D" + name + "=" + std::to_string(value));
            command.insert(command.end(), {"-o", target.string(), source.string()});
            Run(command, m_work / "nasm.log");
        }
        // 生成校验载荷并返回路径；size 为文件字节数，media/drive 为期望的交接状态
        // sector 为期望的 IPL 交接扇区大小；0 表示按介质推断：光盘 2048，磁盘 512
        fs::path Probe(uint32_t size, unsigned media, unsigned drive, unsigned resets = 0, unsigned chs = 0, unsigned sector = 0) {
            if (sector == 0) sector = media == 4 ? 2048 : 512;
            const auto path = m_work / "probe.bin";
            Assemble(m_work / "Probe.asm", path, {{"PAYLOAD_BYTES", size}, {"EXPECT_MEDIA", media}, {"EXPECT_DRIVE", drive},
                                                  {"EXPECT_RESET", resets}, {"EXPECT_CHS", chs}, {"EXPECT_SECTOR", sector}});
            return path;
        }
        // 用 MakeIso / MakeHdd 组装一份测试镜像并返回路径
        fs::path Build(bool cd, uint32_t size = 4096, bool hybrid = false) {
            const auto stub = Probe(size, cd ? 4 : 2, cd ? 0xE0 : 0x80);
            const auto image = m_work / (cd ? "base.iso" : "base.hdd");
            std::vector<std::string> command{(m_tools / (cd ? "MakeIso" : "MakeHdd")).string(), "--out", image.string()};
            if (cd) {
                command.insert(command.end(), {"--boot-image", (m_options.iplDir / "BaleenIPLCd.bin").string()});
                if (hybrid) command.insert(command.end(), {"--mbr", (m_options.iplDir / "BaleenIPL.bin").string()});
            } else command.insert(command.end(), {"--mbr", (m_options.iplDir / "BaleenIPL.bin").string()});
            // 校验载荷是内嵌夹具而不是 BaleenStub 正式产物：显式免除组装器的头与摘要校验
            command.insert(command.end(), {"--stub", stub.string(), "--stub-unchecked"});
            Run(command, m_work / "image.log");
            return image;
        }
        // 用故障夹具注入一类 BIOS 故障，核对 IPL 的反应；e1Tries 非零时另核对低内存里对 E1 的尝试次数
        // sector 为本地逻辑扇区大小（0 = 按介质推断：CD 2048、HDD 512）；ah48 选择 AH=48 的夹具应答
        void Fault(bool cd, unsigned fault, const std::string& name, uint32_t size, unsigned entryDrive, unsigned expectedDrive, const std::string& expected, unsigned e1Tries = 0, unsigned sector = 0, unsigned ah48 = 0) {
            if (sector == 0) sector = cd ? 2048 : 512;
            const unsigned shift = sector == 4096 ? 12u : (cd ? 11u : 9u);
            // 描述符与载荷按当前布局：描述符在 0x22000，载荷自 0x23000 起（三种单位下同一字节位置）
            constexpr std::size_t kDescriptorOffset = 0x22000;
            const std::size_t stubOffset = 0x23000;
            // fixture 缓冲排在 2048 字节 harness 之后：虚拟介质偏移 = 2048 + 缓冲偏移
            constexpr std::size_t kHarnessBytes = 2048;
            const std::size_t descriptorAt = kDescriptorOffset - kHarnessBytes;
            const std::size_t stubAt = stubOffset - kHarnessBytes;
            const auto harness = m_work / "harness.bin";
            Assemble(m_work / "BiosFault.asm", harness, {{"FAULT", fault}, {"SECT_SHIFT", shift}, {"ENTRY_DRIVE", entryDrive}, {"AH48_MODE", ah48}});
            const auto stub = ReadBytes(Probe(size, cd ? 4 : 2, expectedDrive, fault == 3 ? 2 : 0, 0, sector));
            Bytes fixture(0x30000, 0);
            const auto& firmware = cd ? m_cd : m_hdd;
            std::copy(firmware.begin(), firmware.end(), fixture.begin());
            PutLe(fixture, descriptorAt, 4, 0x52445342);
            PutLe(fixture, descriptorAt + 4, 2, 1);
            PutLe(fixture, descriptorAt + 6, 2, 32);
            PutLe(fixture, descriptorAt + 8, 8, stubOffset);
            PutLe(fixture, descriptorAt + 16, 8, stub.size());
            PutLe(fixture, descriptorAt + 24, 8, 0);
            Require(stub.size() <= fixture.size() - stubAt, "故障夹具载荷越界");
            std::copy(stub.begin(), stub.end(), fixture.begin() + static_cast<std::ptrdiff_t>(stubAt));
            auto image = ReadBytes(harness);
            Require(image.size() == 2048, "故障引导器必须为 2048 字节");
            image.insert(image.end(), fixture.begin(), fixture.end());
            image.resize(1 << 20, 0);
            const auto path = m_work / "fault.hdd";
            WriteBytes(path, image);
            m_expectE1 = e1Tries;
            Case(std::string(cd ? "cd " : "hdd ") + name, path, "hdd", expected, cd);
        }
        // 跑一个启动用例并核对结果
        void Case(const std::string& name, const fs::path& image, const std::string& media, const std::string& expected, bool cdIpl) {
            CheckSignal();
            const auto local = m_work / ("case-" + std::to_string(m_count + 1));
            fs::create_directory(local);
            WriteText(local / "case.txt", name + "\n期望=" + expected + "\n");
            RunQemu(image, media, expected, local, cdIpl);
            m_expectE1 = 0;
            ++m_count;
            std::cout << "CheckIpl: " << m_count << "/57 " << name << " 通过" << std::endl;
        }
        // 启动 QEMU，按期望值核对退出码、串口输出、停机位置与 VGA 画面
        void RunQemu(const fs::path& image, const std::string& media, const std::string& expected, const fs::path& work, bool cdIpl) {
            const auto debug = work / "debug.log";
            const auto socket = work / "qmp.sock";
            WriteText(debug, "");
            std::vector<std::string> command{m_qemu, "-machine", "q35", "-m", "64", "-display", "none", "-serial", "none",
                "-monitor", "none", "-no-reboot", "-snapshot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                "-debugcon", "file:" + debug.string(), "-qmp", "unix:" + socket.string() + ",server=on,wait=off"};
            if (media == "cd") command.insert(command.end(), {"-cdrom", image.string(), "-boot", "d"});
            else if (media == "usb") command.insert(command.end(), {"-device", "usb-ehci,id=ehci", "-drive", "if=none,id=disk,format=raw,file=" + image.string(), "-device", "usb-storage,bus=ehci.0,drive=disk", "-boot", "c"});
            else command.insert(command.end(), {"-drive", "file=" + image.string() + ",format=raw,if=ide", "-boot", "c"});
            Child child(command, work / "qemu.log");
            std::unique_ptr<Qmp> qmp;
            const auto deadline = Clock::now() + m_timeout;
            while (Clock::now() < deadline) {
                CheckSignal();
                const auto output = ReadText(debug);
                if (const auto status = child.Status()) {
                    Require(expected == "OK" && *status == 33 && output == "IPL-PROBE-OK", "退出=" + std::to_string(*status) + "，输出=" + Quote(output) + "\n" + ReadText(work / "qemu.log"));
                    return;
                }
                if (expected != "OK" && (output == expected || !m_options.e9)) {
                    const auto qmpDeadline = std::min(deadline, Clock::now() + 2s);
                    if (!qmp) qmp = Qmp::Connect(socket, qmpDeadline);
                    if (qmp) {
                        const auto registers = qmp->Monitor("info registers", qmpDeadline);
                        if (registers.find("HLT=1") != std::string::npos &&
                            Register(registers, R"(CS\s*=([0-9a-fA-F]+))") == 0x07C0) {
                            WriteText(work / "registers.txt", registers);
                            const auto flags = Register(registers, R"(EFL=([0-9a-fA-F]+))");
                            const auto ip = Register(registers, R"(EIP=([0-9a-fA-F]+))");
                            Require(!(flags & 0x200), "失败停机仍启用中断");
                            Require(ip == (cdIpl ? m_cdHalt : m_hddHalt), "失败未停在 IPL 诊断 HLT 位置，IP=" + std::to_string(ip));
                            const auto vgaPath = work / "vga.bin";
                            const auto response = qmp->Monitor("pmemsave 0xb8000 0x1000 \"" + vgaPath.string() + "\"", qmpDeadline);
                            Require(response.empty(), "VGA 读取失败：" + response);
                            const auto vga = ReadBytes(vgaPath);
                            Require(vga.size() == 0x1000, "VGA 快照长度不符");
                            bool standalone = false;
                            std::string screen;
                            for (unsigned row = 0; row < 25; ++row) {
                                std::string line;
                                for (unsigned col = 0; col < 80; ++col) line += char(vga[(row * 80 + col) * 2]);
                                // 判断一个字符算不算空白
                                const auto blank = [](char c) { return c == ' ' || c == '\0'; };
                                while (!line.empty() && blank(line.back())) line.pop_back();
                                auto first = std::find_if_not(line.begin(), line.end(), blank);
                                line.erase(line.begin(), first);
                                if (line == expected) standalone = true;
                                screen += line + '\n';
                            }
                            WriteText(work / "vga.txt", screen);
                            Require(standalone, "VGA 未见独立错误码 " + expected + "\n" + screen);
                            Require(output == (m_options.e9 ? expected : ""), "错误码不符：" + Quote(output));
                            // 需要核对 E1 尝试次数时读夹具写在低内存 0x504 的计数
                            if (m_expectE1 != 0) {
                                const auto memoryPath = work / "lowmem.bin";
                                const auto lowResponse = qmp->Monitor("pmemsave 0x500 0x10 \"" + memoryPath.string() + "\"", qmpDeadline);
                                Require(lowResponse.empty(), "低内存读取失败：" + lowResponse);
                                const auto low = ReadBytes(memoryPath);
                                Require(low.size() == 0x10, "低内存快照长度不符");
                                const auto tries = static_cast<unsigned>(low[4] | (low[5] << 8));
                                WriteText(work / "e1-tries.txt", std::to_string(tries) + "\n");
                                Require(tries == m_expectE1, "对 E1 的尝试次数为 " + std::to_string(tries) + "，期望 " + std::to_string(m_expectE1));
                            }
                            return;
                        }
                    }
                }
                Pause(20);
            }
            Fail("启动超时，期望=" + expected + "，输出=" + Quote(ReadText(debug)));
        }
        Options m_options;                                // 命令行选项
        fs::path m_tools, m_work;                         // 工具目录与临时工作目录
        std::string m_nasm, m_qemu;                       // 汇编器与模拟器的可执行文件
        std::chrono::seconds m_timeout{12};               // 每例启动的超时
        Bytes m_hdd, m_cd;                                // 两份 IPL 产物
        std::size_t m_hddHalt = 0, m_cdHalt = 0;          // 两份额外停机序列里 HLT 之后的 IP
        unsigned m_expectE1 = 0;                          // 失败用例要核对的 E1 尝试次数；0 表示不核对
        unsigned m_count = 0;                             // 已通过的用例数
    };
}

// 入口：--layout-only 只做静态检查，否则跑启动回归
int main(int argc, char** argv) {
    fs::path work;
    try {
        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
        std::signal(SIGHUP, OnSignal);
        const auto options = Parse(argc, argv);
        if (!options.layout.empty()) { CheckLayout(options.layout); return 0; }
        const fs::path tools = options.root.empty() ? fs::canonical("/proc/self/exe").parent_path() : options.root / "Tools" / "Bin";
        std::array<char, 64> pattern{};
        std::strcpy(pattern.data(), "/tmp/checkipl.XXXXXX");
        const char* directory = ::mkdtemp(pattern.data());
        Require(directory != nullptr, "无法创建临时工作目录");
        work = directory;
        BootChecks(options, tools, work).Execute();
        if (options.keep) std::cout << "CheckIpl: 工作目录 " << work << '\n';
        else fs::remove_all(work);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckIpl: " << error.what() << '\n';
        if (!work.empty()) std::cerr << "CheckIpl: 失败证据保留在 " << work << '\n';
        return interrupted ? 128 + interrupted : 1;
    }
}
