// main.cpp
// RunBootCase：启动模拟器并监测标记，成功即停止，区分自行退出/超时，回收整个进程组

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
namespace {
    // 收到终止信号时记下的信号号，主循环轮询它并退出
    volatile std::sig_atomic_t interrupted = 0;
    // 信号处理：只记录信号号，不在处理函数里做其他事
    void OnSignal(int signal) { interrupted = signal; }

    // 把文本编码为 JSON 字符串字面量
    std::string Json(const std::string& text) {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : text) {
            if (c == '"' || c == '\\') out << '\\' << c;
            else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
            else out << c;
        }
        return out.str() + '"';
    }

    // 监视一个日志文件：只看本次启动后追加的内容，跨读取保留匹配用的尾巴
    struct Watch {
        fs::path path;       // 被监视的日志路径
        size_t offset = 0;   // 已经读到的位置
        std::string suffix;  // 上次读取留下的尾部，保证跨块也能匹配标记
        // 记住当前文件末尾，之后只识别新追加的内容
        void RememberExisting() {
            std::error_code error;
            const auto size = fs::file_size(path, error);
            offset = error ? 0 : size_t(size);
            suffix.clear();
        }
        // 从上次位置继续读，判断新增内容里是否出现标记
        bool Contains(const std::string& marker) {
            std::error_code error;
            const auto size = fs::file_size(path, error);
            if (error) return false;
            if (size < offset) { offset = 0; suffix.clear(); }
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            in.seekg(std::streamoff(offset));
            char buffer[8192];
            while (in) {
                in.read(buffer, sizeof(buffer));
                const size_t count = size_t(in.gcount());
                offset += count;
                suffix.append(buffer, count);
                if (suffix.find(marker) != std::string::npos) return true;
                const size_t keep = marker.size() - 1;
                if (suffix.size() > keep) suffix.erase(0, suffix.size() - keep);
            }
            return false;
        }
    };

    // 子进程：单独开进程组，退出时连同整组一起回收
    struct Child {
        pid_t pid = -1;       // 子进程号，-1 表示还没 fork
        bool reaped = false;  // 是否已经回收
        int status = 0;       // waitpid 拿到的原始状态
        // 非阻塞地探一次退出状态，返回是否已退出
        bool Poll() {
            if (reaped || pid < 0) return reaped;
            pid_t result;
            do { result = waitpid(pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
            if (result == pid) reaped = true;
            if (result < 0) throw std::runtime_error("waitpid: " + std::string(std::strerror(errno)));
            return reaped;
        }
        // 退出码；被信号杀死时返回负的信号号
        int Code() const {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return -WTERMSIG(status);
            return 1;
        }
        // 先 SIGTERM 再 SIGKILL 地结束整组，并回收子进程
        void Stop() noexcept {
            if (pid < 0) return;
            // 同组子进程也必须退出；父进程自行退出不意味着整组已结束
            kill(-pid, SIGTERM);
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (!reaped && Clock::now() < deadline) {
                const pid_t result = waitpid(pid, &status, WNOHANG);
                if (result == pid || (result < 0 && errno == ECHILD)) reaped = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            kill(-pid, SIGKILL);
            if (!reaped) {
                pid_t result;
                do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
                reaped = result == pid;
            }
        }
        // 析构时确保子进程已经结束
        ~Child() { if (pid >= 0 && !reaped) Stop(); }
    };

    // 把日志的最后几行打到标准错误，便于失败时直接看到现场
    void Tail(const fs::path& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return;
        const auto size = in.tellg();
        if (size < 0) return;
        in.seekg(std::max<std::streamoff>(0, std::streamoff(size) - 8192));
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            for (char& c : line) if (static_cast<unsigned char>(c) < 32) c = ' ';
            if (line.find_first_not_of(' ') != std::string::npos) lines.push_back(line);
        }
        std::cerr << "--- " << path.string() << " ---\n";
        for (size_t i = lines.size() > 8 ? lines.size() - 8 : 0; i < lines.size(); ++i) std::cerr << lines[i] << '\n';
    }

    // 参数解析、启动模拟器并等待标记；返回进程退出码
    int Main(int argc, char** argv) {
        double timeout = 0;
        std::string marker;
        fs::path console, cwd, input = "/dev/null";
        std::vector<Watch> watches;
        int commandIndex = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--") { commandIndex = i + 1; break; }
            if (i + 1 >= argc) throw std::runtime_error("选项缺值：" + option);
            const std::string value = argv[++i];
            if (option == "--timeout") {
                size_t consumed = 0;
                timeout = std::stod(value, &consumed);
                if (consumed != value.size()) throw std::runtime_error("timeout 格式非法");
            } else if (option == "--marker") marker = value;
            else if (option == "--console") console = fs::absolute(value);
            else if (option == "--cwd") cwd = fs::absolute(value);
            else if (option == "--stdin") input = fs::absolute(value);
            else if (option == "--log") watches.push_back({fs::absolute(value), 0, {}});
            else throw std::runtime_error("未知选项：" + option);
        }
        if (!(timeout > 0) || !std::isfinite(timeout) || marker.empty() || console.empty() || cwd.empty() ||
            watches.empty() || commandIndex == 0 || commandIndex >= argc)
            throw std::runtime_error("用法：RunBootCase --timeout 秒 --marker 标记 --console 文件 --log 文件 [--log 文件] --cwd 目录 [--stdin 文件] -- 命令 参数...");
        fs::create_directories(cwd);
        for (int signal : {SIGINT, SIGTERM, SIGHUP}) std::signal(signal, OnSignal);
        const rlimit core = {0, 0};
        setrlimit(RLIMIT_CORE, &core);
        const auto start = Clock::now();
        std::string state = "failed", detail;
        int exitCode = 1;
        bool stoppedAfterMarker = false;
        Child child;
        const int out = open(console.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out < 0) throw std::runtime_error("无法创建 console 日志");
        const int in = open(input.c_str(), O_RDONLY);
        if (in < 0) { close(out); throw std::runtime_error("无法读取 stdin 文件"); }
        // console 已清空；其他日志仅识别本次启动后追加的内容，不能命中旧标记
        for (auto& watch : watches) watch.RememberExisting();
        child.pid = fork();
        if (child.pid == 0) {
            for (int signal : {SIGINT, SIGTERM, SIGHUP}) std::signal(signal, SIG_DFL);
            // fd 绑定后再诊断启动失败，以保留错误日志
            if (dup2(out, STDOUT_FILENO) < 0 || dup2(out, STDERR_FILENO) < 0 || dup2(in, STDIN_FILENO) < 0)
                _exit(126);
            close(out);
            close(in);
            if (setsid() < 0 || chdir(cwd.c_str()) != 0) {
                std::cerr << "RunBootCase: 初始化子进程失败：" << std::strerror(errno) << '\n';
                _exit(126);
            }
            execvp(argv[commandIndex], argv + commandIndex);
            std::cerr << "RunBootCase: exec 失败：" << std::strerror(errno) << '\n';
            _exit(127);
        }
        close(out);
        close(in);
        if (child.pid < 0) {
            state = "launch-error";
            detail = "fork 失败：" + std::string(std::strerror(errno));
        } else {
            while (true) {
                const bool exited = child.Poll();
                bool found = false;
                for (auto& watch : watches) found = watch.Contains(marker) || found;
                if (interrupted) {
                    state = "interrupted";
                    detail = "收到信号 " + std::to_string(interrupted);
                    exitCode = 128 + interrupted;
                    break;
                }
                if (exited && child.Code() != 0) {
                    detail = "模拟器提前退出，exit=" + std::to_string(child.Code());
                    break;
                }
                if (found) {
                    state = "passed";
                    exitCode = 0;
                    stoppedAfterMarker = !exited;
                    detail = "看到 " + marker + "，结束模拟器";
                    break;
                }
                if (exited) {
                    detail = "模拟器提前退出，exit=0，未见 " + marker;
                    break;
                }
                if (std::chrono::duration<double>(Clock::now() - start).count() >= timeout) {
                    state = "timeout";
                    detail = "超时，未见 " + marker;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        for (int signal : {SIGINT, SIGTERM, SIGHUP}) std::signal(signal, SIG_IGN);
        child.Stop();
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        std::ofstream result(cwd / "result.json");
        result << "{\n  \"status\": " << Json(state) << ",\n  \"detail\": " << Json(detail)
               << ",\n  \"elapsed_seconds\": " << std::fixed << std::setprecision(3) << elapsed
               << ",\n  \"command\": [";
        for (int i = commandIndex; i < argc; ++i) result << (i == commandIndex ? "" : ", ") << Json(argv[i]);
        result << "],\n  \"cwd\": " << Json(cwd.string()) << ",\n  \"returncode\": "
               << (child.pid < 0 ? "null" : std::to_string(child.Code()))
               << ",\n  \"stopped_after_marker\": " << (stoppedAfterMarker ? "true" : "false") << "\n}\n";
        result.close();
        if (!result) throw std::runtime_error("无法写入 result.json");
        std::cout << "boot-test: " << state << "（" << std::fixed << std::setprecision(2)
                  << elapsed << "s）：" << detail << '\n';
        if (exitCode != 0) {
            Tail(console);
            for (const auto& watch : watches) Tail(watch.path);
        }
        return exitCode;
    }
}

// 入口：把异常转成退出码与一行诊断
int main(int argc, char** argv) {
    try { return Main(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << "RunBootCase: " << error.what() << '\n';
        return 1;
    }
}
