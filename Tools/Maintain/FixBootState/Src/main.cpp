/* main.cpp
    FixBootState：引导状态区域的维护工具，手工执行，不是构建或启动链的一环

    操作者是介质的所有者：工具在镜像（或整盘设备）上按 StateOffset 定位状态区域，提供
    报告、受控恢复（清零、按有效副本修复）与人工接受（下调已见最高安全版本）三个动作
    它不猜目标集、不碰系统卷：区域里两份都不像记录时只报告并拒绝修复，由操作者决定清零
    读写经与引导期、运行期同一份 BootState 实现，刷盘经 fsync，改动都留一行审计记录
    命令形态与记录方式对应 Docs/Specs/Baleen/引导状态记录.md 第八节的两条恢复入口
*/

#include <BootState.hpp>

#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>

#include <cerrno>
#include <cstdint>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {
    namespace BootState = Baleen::Core::BootState;
    using BootState::CopyStatus;
    using BootState::ReadResult;

    // 状态区域的默认位置与长度：与布局描述的默认值一致
    constexpr uint64_t kDefaultStateOffset = 0x21000;
    constexpr uint32_t kDefaultStateBytes = 4096;
    // 逻辑块大小：状态区域按 512 的整数倍对齐
    constexpr uint32_t kBlockBytes = 512;

    // 中止当前操作
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 文件块设备：按字节偏移读写镜像文件，Flush 用 fsync 落实刷盘
    class ImageDevice final : public Storage::BlockDevice {
    public:
        // 构造：已打开的文件描述符、容量与可写性
        ImageDevice(int fd, uint64_t bytes, bool writable) : m_fd(fd), m_bytes(bytes), m_writable(writable) {}
        // 逻辑块大小
        uint32_t BlockBytes() const override { return kBlockBytes; }
        // 文件长度
        uint64_t CapacityBytes() const override { return m_bytes; }
        // 是否以可写方式打开
        bool Writable() const override { return m_writable; }
        // 按字节偏移读；短读或越界都算失败
        Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override {
            if (offset > m_bytes || bytes > m_bytes - offset) return Storage::Error::InvalidArgument;
            uint8_t* out = static_cast<uint8_t*>(buffer);
            uint32_t done = 0;
            while (done < bytes) {
                const ssize_t got = ::pread(m_fd, out + done, bytes - done, static_cast<off_t>(offset + done));
                if (got <= 0) return Storage::Error::Io;
                done += static_cast<uint32_t>(got);
            }
            return Storage::Error::Ok;
        }
        // 按字节偏移写；只读打开时拒绝
        Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override {
            if (!m_writable) return Storage::Error::ReadOnly;
            if (offset > m_bytes || bytes > m_bytes - offset) return Storage::Error::InvalidArgument;
            const uint8_t* in = static_cast<const uint8_t*>(buffer);
            uint32_t done = 0;
            while (done < bytes) {
                const ssize_t put = ::pwrite(m_fd, in + done, bytes - done, static_cast<off_t>(offset + done));
                if (put <= 0) return Storage::Error::Io;
                done += static_cast<uint32_t>(put);
            }
            return Storage::Error::Ok;
        }
        // 刷盘：fsync 是宿主侧的刷新原语，与真实后端的 FLUSH CACHE 对应
        Storage::Error Flush() override {
            if (!m_writable) return Storage::Error::Ok;
            return ::fsync(m_fd) == 0 ? Storage::Error::Ok : Storage::Error::Io;
        }
    private:
        int m_fd = -1;         // 文件描述符
        uint64_t m_bytes = 0;  // 文件长度
        bool m_writable = false;   // 是否可写
    };

    // 命令行选项
    struct Options {
        std::string image;          // 镜像或设备路径
        uint64_t offset = kDefaultStateOffset;   // 状态区域偏移
        uint32_t bytes = kDefaultStateBytes;     // 状态区域长度
        bool report = false;        // 只报告
        bool clear = false;         // 清零全部槽位
        bool repair = false;        // 按有效副本修复
        bool acceptSeen = false;    // 人工下调已见最高安全版本
        uint64_t acceptedVersion = 0;   // 接受的安全版本
        bool publish = false;       // 发布候选：写 Pending 记录
        std::array<uint8_t, 32> candidateBuildId{};   // 候选集标识
        std::array<uint8_t, 32> candidateDigest{};    // 候选集清单摘要
        bool confirm = false;       // 提交确认：写 Confirmed 记录
        uint64_t confirmAttempt = 0;    // 要确认的尝试身份
        std::string audit;          // 审计记录追加到的文件
    };

    // 打印用法
    void Usage() {
        std::cout <<
            "FixBootState — 引导状态区域的维护工具\n"
            "\n"
            "用法：FixBootState --image <镜像> [--offset <偏移>] [--bytes <长度>] <动作> [--audit <文件>]\n"
            "\n"
            "  --image   镜像或整盘设备路径（不可写时只允许 --report）\n"
            "  --offset  状态区域偏移，默认 0x21000\n"
            "  --bytes   状态区域长度，默认 4096\n"
            "  --report  只报告：每个槽位各自的结论、选中记录与全部字段\n"
            "  --clear   受控恢复：清零全部槽位，回到出厂形态（选版按无记录直启缺省集）\n"
            "  --repair  受控恢复：用一份有效副本重建损坏副本，代次加一\n"
            "  --accept-seen <版本>  人工接受：把已见最高安全版本下调到该值（防回滚的唯一例外）\n"
            "  --publish --build-id <64 位十六进制> --digest <64 位十六进制>\n"
            "                        发布候选：写 Pending 记录（候选标识、清单摘要与默认尝试次数）\n"
            "                        这是运行期发布方 Updater 的写入步骤，运行期未就绪时由本工具代执行\n"
            "  --confirm <尝试身份>  提交确认：记录仍是 Trying 且尝试身份相符时写 Confirmed\n"
            "                        这是运行期确认方 Init 的写入步骤，身份不符一律不写\n"
            "  --audit   把本次动作追加成一行审计记录\n"
            "\n"
            "没有动作时等同于 --report。退出码 0 成功、1 动作失败、2 参数错误\n"
            "规格见 Docs/Specs/Baleen/引导状态记录.md 第八节\n";
    }

    // 解析命令行的版本号：十进制或 0x 前缀
    uint64_t Number(const std::string& text, const char* what) {
        try {
            std::size_t used = 0;
            const uint64_t value = std::stoull(text, &used, 0);
            Require(used == text.size(), std::string(what) + " 不是完整数字：" + text);
            return value;
        } catch (const std::exception&) {
            Fail(std::string(what) + " 不是数字：" + text);
        }
    }

    // 解析 64 位十六进制成 32 字节：长度不符或含非十六进制字符直接失败
    std::array<uint8_t, 32> Hex32(const std::string& text, const char* what) {
        Require(text.size() == 64, std::string(what) + " 必须是 64 位十六进制：" + text);
        const auto nibble = [&](char c) -> unsigned {
            if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
            Fail(std::string(what) + " 含非十六进制字符：" + text);
            return 0;
        };
        std::array<uint8_t, 32> bytes{};
        for (std::size_t i = 0; i < 32; ++i) bytes[i] = static_cast<uint8_t>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
        return bytes;
    }

    // 解析命令行
    Options Parse(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&](const char* name) -> std::string {
                Require(i + 1 < argc, std::string(name) + " 缺少参数");
                return argv[++i];
            };
            if (arg == "-h" || arg == "--help") { Usage(); std::exit(0); }
            else if (arg == "--image") options.image = value("--image");
            else if (arg == "--offset") options.offset = Number(value("--offset"), "状态区域偏移");
            else if (arg == "--bytes") options.bytes = static_cast<uint32_t>(Number(value("--bytes"), "状态区域长度"));
            else if (arg == "--report") options.report = true;
            else if (arg == "--clear") options.clear = true;
            else if (arg == "--repair") options.repair = true;
            else if (arg == "--accept-seen") { options.acceptSeen = true; options.acceptedVersion = Number(value("--accept-seen"), "接受的安全版本"); }
            else if (arg == "--publish") options.publish = true;
            else if (arg == "--build-id") options.candidateBuildId = Hex32(value("--build-id"), "候选集标识");
            else if (arg == "--digest") options.candidateDigest = Hex32(value("--digest"), "候选清单摘要");
            else if (arg == "--confirm") { options.confirm = true; options.confirmAttempt = Number(value("--confirm"), "要确认的尝试身份"); }
            else if (arg == "--audit") options.audit = value("--audit");
            else Fail("无法识别的参数：" + arg);
        }
        Require(!options.image.empty(), "必须提供 --image");
        Require(options.offset != 0, "状态区域偏移不能为 0：0 表示没有状态区域，是布局描述的约定");
        Require(options.bytes >= BootState::kAreaBytes, "状态区域长度不足 " + std::to_string(BootState::kAreaBytes) + " 字节");
        Require(!(options.clear && options.repair) && !(options.clear && options.acceptSeen) && !(options.repair && options.acceptSeen), "一次只做一个动作");
        Require(!(options.publish && (options.clear || options.repair || options.acceptSeen || options.confirm)), "--publish 不能与其它动作一起用");
        Require(!(options.confirm && (options.clear || options.repair || options.acceptSeen)), "--confirm 不能与其它动作一起用");
        Require(options.confirmAttempt == 0 || options.confirm, "--confirm 的尝试身份只能与 --confirm 一起给");
        Require(!options.publish || options.candidateBuildId != std::array<uint8_t, 32>{}, "--publish 需要一个非全零的 --build-id");
        return options;
    }

    // 区域结论的可读写法
    const char* ResultName(ReadResult result) {
        switch (result) {
            case ReadResult::Ok: return "有效";
            case ReadResult::Empty: return "空区域";
            case ReadResult::Corrupt: return "损坏";
            case ReadResult::NoRegion: return "无状态区域";
            case ReadResult::IoError: return "读取失败";
            case ReadResult::InvalidArea: return "区域参数非法";
        }
        return "未知结论";
    }

    // 副本结论的可读写法
    const char* StatusName(CopyStatus status) {
        switch (status) {
            case CopyStatus::NotWritten: return "未写过";
            case CopyStatus::Valid: return "有效";
            case CopyStatus::Invalid: return "不可用";
        }
        return "不可用";
    }

    // 记录状态的可读写法
    const char* StateName(BootState::State state) {
        switch (state) {
            case BootState::State::Confirmed: return "Confirmed";
            case BootState::State::Pending: return "Pending";
            case BootState::State::Trying: return "Trying";
            case BootState::State::Failed: return "Failed";
        }
        return "未知";
    }

    // 64 位数值的十六进制写法
    std::string Hex(uint64_t value) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const unsigned digit = static_cast<unsigned>((value >> shift) & 0xF);
            if (digit != 0 || started || shift == 0) { text += kDigits[digit]; started = true; }
        }
        return text;
    }

    // 32 字节标识的十六进制写法
    std::string HexBytes(const uint8_t* bytes, uint32_t count) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text;
        for (uint32_t i = 0; i < count; ++i) {
            text += kDigits[bytes[i] >> 4];
            text += kDigits[bytes[i] & 0x0F];
        }
        return text;
    }

    // 吊销位图里置位的 KeyId 升序列成文本：没有置位时写 none
    std::string MaskText(const uint8_t* mask) {
        std::string text;
        for (unsigned keyId = 1; keyId <= 255; ++keyId) {
            const uint32_t bit = keyId - 1;
            if ((mask[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) == 0) continue;
            if (!text.empty()) text += ",";
            text += std::to_string(keyId);
        }
        return text.empty() ? "none" : text;
    }

    // 报告：每个槽位的结论、选中记录与全部字段
    // 槽位名按 A..H 打印：区域切成 8 个 512 字节槽位，提交一次写其中一个
    void Report(const BootState::Inspection& inspection) {
        std::cout << "  槽位：";
        for (uint32_t slot = 0; slot < BootState::kSlots; ++slot) {
            std::cout << static_cast<char>('A' + slot) << "=" << StatusName(inspection.slots[slot].status);
            if (inspection.slots[slot].status == CopyStatus::Valid) std::cout << "(" << inspection.slots[slot].generation << ")";
            if (slot + 1 != BootState::kSlots) std::cout << " ";
        }
        std::cout << "\n  区域结论：" << ResultName(inspection.result) << "\n";
        if (inspection.result != ReadResult::Ok) {
            if (inspection.result == ReadResult::Corrupt) std::cout << "  没有可用记录：不猜目标集，可用 --repair 修复或 --clear 清零\n";
            else if (inspection.result == ReadResult::IoError) std::cout << "  读取状态区域失败：检查介质与 --offset/--bytes，未确认前不要清零或修复\n";
            else if (inspection.result == ReadResult::InvalidArea) std::cout << "  状态区域参数非法：核对 --offset 对齐与 --bytes 不小于 4096\n";
            return;
        }
        const auto& record = inspection.record;
        std::cout << "  状态=" << StateName(record.state) << " 代次=" << record.generation
                  << " 已见安全版本=" << record.seenSecurityVersion
                  << " 尝试身份=" << record.attemptId
                  << " 剩余尝试=" << static_cast<unsigned>(record.triesRemaining)
                  // 已试次数只增：它是对账用的——断电丢过一次提交时，这个数会露出来
                  << " 已试次数=" << static_cast<unsigned>(record.triesDone) << "\n";
        std::cout << "  已知良好集=" << HexBytes(record.confirmedBuildId, 32) << "\n";
        std::cout << "  候选集=" << HexBytes(record.candidateBuildId, 32) << "\n";
        std::cout << "  吊销事实：纪元=" << record.revocation.epoch
                  << " 最低允许安全版本=" << record.revocation.minimumSecurityVersion
                  << " 已吊销密钥=" << MaskText(record.revocation.revokedKeyMask) << "\n";
    }

    // 追加一行审计记录：时间、动作、镜像、区域、结果
    void Audit(const std::string& path, const std::string& action, const std::string& detail) {
        const std::time_t now = std::time(nullptr);
        char stamp[32] = {};
        std::tm utc{};
        gmtime_r(&now, &utc);
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        std::ofstream out(path, std::ios::app);
        Require(bool(out), "无法写入审计文件 " + path);
        out << stamp << " FixBootState " << action << " " << detail << "\n";
    }
}

// 入口：解析参数、打开镜像并执行一个动作
int main(int argc, char** argv) {
    try {
        const Options options = Parse(argc, argv);
        const bool mutate = options.clear || options.repair || options.acceptSeen || options.publish || options.confirm;
        const int fd = ::open(options.image.c_str(), mutate ? O_RDWR : O_RDONLY);
        Require(fd >= 0, std::string("无法打开 ") + options.image + "：" + std::strerror(errno));
        struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
        const off_t length = ::lseek(fd, 0, SEEK_END);
        Require(length >= 0, "无法取得文件长度");
        ImageDevice device(fd, static_cast<uint64_t>(length), mutate);

        std::cout << "FixBootState: " << options.image << " 状态区域 offset=0x" << Hex(options.offset)
                  << " bytes=" << options.bytes << "\n";
        const auto inspection = BootState::Inspect(device, options.offset, options.bytes);
        Report(inspection);

        if (!mutate) return 0;

        std::string action;
        std::string detail;
        uint64_t generation = 0;
        bool ok = false;
        if (options.clear) {
            action = "--clear";
            ok = BootState::Clear(device, options.offset, options.bytes);
            detail = std::string("原区域结论=") + ResultName(inspection.result);
        } else if (options.repair) {
            action = "--repair";
            ok = BootState::Repair(device, options.offset, options.bytes, generation);
            detail = "代次=" + std::to_string(generation) + " 原区域结论=" + ResultName(inspection.result);
        } else if (options.publish) {
            action = "--publish";
            ok = BootState::PublishCandidate(device, options.offset, options.bytes, options.candidateBuildId.data(), options.candidateDigest.data(), BootState::kDefaultTries, generation);
            detail = "原有状态=" + std::string(inspection.result == ReadResult::Ok ? StateName(inspection.record.state) : "无记录")
                   + " 候选=" + HexBytes(options.candidateBuildId.data(), 32)
                   + " 尝试次数=" + std::to_string(BootState::kDefaultTries) + " 代次=" + std::to_string(generation);
        } else if (options.confirm) {
            action = "--confirm";
            const uint64_t attempt = options.confirmAttempt;
            ok = BootState::ConfirmAttempt(device, options.offset, options.bytes, attempt, generation);
            detail = "尝试身份=" + std::to_string(attempt) + " 代次=" + std::to_string(generation);
        } else {
            action = "--accept-seen";
            ok = BootState::AcceptSeenSecurityVersion(device, options.offset, options.bytes, options.acceptedVersion, generation);
            detail = "已见安全版本=" + std::to_string(inspection.result == ReadResult::Ok ? inspection.record.seenSecurityVersion : 0)
                   + "->" + std::to_string(options.acceptedVersion) + " 代次=" + std::to_string(generation);
        }
        if (!ok) {
            // 失败原因按动作给具体理由：区域结论只是现场，不是原因的全部
            std::string why;
            if (options.clear) why = "介质不可写、写失败或读回不是全零";
            else if (options.repair) why = inspection.result == ReadResult::Ok ? "写不成立（介质不可写或写失败）" : "没有可用记录，不猜测目标集";
            else if (options.publish) why = inspection.result == ReadResult::Corrupt ? "记录损坏，不猜测目标集" : "写不成立（介质不可写、写失败或候选标识全零）";
            else if (options.confirm) why = inspection.result == ReadResult::Ok ? "记录不是 Trying 或尝试身份不符（比较再提交），也可能是写不成立" : "没有可用记录";
            else why = inspection.result == ReadResult::Ok ? "接受值必须低于当前已见安全版本（抬高由选择器随清单完成）" : "没有可用记录";
            std::cerr << "FixBootState: " << action << " 失败：" << why << "（区域结论 " << ResultName(inspection.result) << "）\n";
            return 1;
        }
        std::cout << "FixBootState: " << action << " 完成：" << detail << "\n";
        if (!options.audit.empty()) Audit(options.audit, action, options.image + " offset=0x" + Hex(options.offset) + " " + detail);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FixBootState: " << error.what() << '\n';
        return 2;
    }
}
