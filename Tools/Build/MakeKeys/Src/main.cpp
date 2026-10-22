/* main.cpp
    MakeKeys：生成与导出开发期 Ed25519 密钥，并从公钥生成引导器内嵌的信任锚源文件

    开发期自签约定：密钥对放在仓库外（默认 .cache/Keys，已被忽略），首次生成后复用；
    私钥不进仓库、不进镜像；换锚 = 重新生成密钥并重建引导器
    --anchor-cpp 从公钥生成 C++ 源，供 BaleenCore 编译进镜像；--anchor-text 生成同一张锚表的
    纯文本形态（每行 "<KeyId> <公钥 64 位十六进制>"），供 SignManifest 校验签名密钥在锚表里
    轮换窗口里的旧公钥由 --anchors 从进版本库的附加锚输入读入，与当前密钥合并成同一张表，
    这样“新旧两把公钥并存”由仓库里的输入复现，而不是靠本地构建产物
*/

#include <Ed25519.h>
#include <HostIo.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
    namespace fs = std::filesystem;
    // 种子与公钥文件名
    constexpr const char* kSeedName = "Development.sec";
    constexpr const char* kPublicName = "Development.pub";
    // 公钥的十六进制字符数
    constexpr std::size_t kPublicHexChars = hostbuild::kEd25519PublicKeyBytes * 2;

    // 信任锚条目：编号与 Ed25519 公钥，与 TrustAnchor.hpp 的条目同构
    struct AnchorEntry {
        uint8_t keyId;                                                          // 密钥编号，取值 1..255
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey;       // Ed25519 公钥
    };

    // 打印用法
    void Usage() {
        std::cout <<
            "MakeKeys — 开发期 Ed25519 密钥生成与信任锚源导出\n"
            "\n"
            "用法：MakeKeys --out <目录> [--key-id <n>] [--anchors <路径>]\n"
            "                [--anchor-cpp <路径>] [--anchor-text <路径>]\n"
            "\n"
            "  --out        密钥目录：Development.sec（32 字节种子，权限 600）与 Development.pub；\n"
            "               已存在时复用，不覆盖已有密钥\n"
            "  --key-id     当前密钥的信任锚编号，取值 1..255，默认 1\n"
            "  --anchors    附加锚输入（进版本库的轮换窗口列表）：每行 \"<KeyId> <公钥 64 位十六进制>\"，\n"
            "               '#' 开头与空行忽略；与当前密钥合并成同一张表，默认不读\n"
            "  --anchor-cpp 生成 C++ 源：定义 Baleen::Crypto 的 kTrustAnchors 与 kTrustAnchorCount；\n"
            "               路径可以只给文件名，此时写在当前目录\n"
            "  --anchor-text 生成纯文本锚表：每行 \"<KeyId> <公钥 64 位十六进制>\"，按 KeyId 升序，\n"
            "               与 --anchor-cpp 同源；供签名工具核对“签名密钥确实在锚表里”\n"
            "\n"
            "开发期密钥只用于本地构建与回归；发布密钥离线保管，不进仓库、不进镜像\n";
    }

    // 从一个十六进制字符取值；不是十六进制返回 -1
    int HexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // 去掉行两端的空白：空格、制表符与回车
    std::string Trim(std::string_view text) {
        std::size_t begin = 0, end = text.size();
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
        return std::string(text.substr(begin, end - begin));
    }

    // 按 "<KeyId> <公钥 64 位十六进制>" 解析一行；格式不符抛异常，line 是用于报告的 1 起行号
    AnchorEntry ParseAnchorLine(const fs::path& path, std::size_t line, const std::string& text) {
        const std::string where = path.string() + ":" + std::to_string(line);
        const auto at = text.find_first_of(" \t");
        if (at == std::string::npos) throw std::runtime_error(where + " 缺少公钥字段");
        const std::string idText = text.substr(0, at);
        const std::string keyText = Trim(text.substr(at + 1));
        const uint64_t keyId = hostbuild::ParseNumber(idText, where + " 的 KeyId");
        if (keyId < 1 || keyId > 255) throw std::runtime_error(where + " 的 KeyId 必须在 1..255 内");
        if (keyText.size() != kPublicHexChars) throw std::runtime_error(where + " 的公钥必须是 " + std::to_string(kPublicHexChars) + " 位十六进制");
        AnchorEntry entry{ static_cast<uint8_t>(keyId), {} };
        for (std::size_t i = 0; i < hostbuild::kEd25519PublicKeyBytes; ++i) {
            const int high = HexValue(keyText[i * 2]), low = HexValue(keyText[i * 2 + 1]);
            if (high < 0 || low < 0) throw std::runtime_error(where + " 的公钥含非十六进制字符");
            entry.publicKey[i] = static_cast<uint8_t>((high << 4) | low);
        }
        return entry;
    }

    // 读附加锚输入：返回全部条目；文件缺失或格式不符抛异常
    std::vector<AnchorEntry> ReadAnchors(const fs::path& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("无法读取附加锚输入 " + path.string());
        std::vector<AnchorEntry> entries;
        std::string line;
        std::size_t number = 0;
        while (std::getline(input, line)) {
            ++number;
            const std::string text = Trim(line);
            if (text.empty() || text[0] == '#') continue;
            entries.push_back(ParseAnchorLine(path, number, text));
        }
        return entries;
    }

    // 合并当前密钥与附加条目：按 KeyId 升序，重复编号报错（查找按编号取值，重复会让结果依赖顺序）
    std::vector<AnchorEntry> MergeAnchors(uint8_t keyId, const std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes>& publicKey, const std::vector<AnchorEntry>& extra) {
        std::vector<AnchorEntry> entries = extra;
        entries.push_back(AnchorEntry{ keyId, publicKey });
        std::stable_sort(entries.begin(), entries.end(), [](const AnchorEntry& left, const AnchorEntry& right) { return left.keyId < right.keyId; });
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i].keyId == entries[i - 1].keyId) throw std::runtime_error("信任锚编号重复：KeyId " + std::to_string(entries[i].keyId) + "（当前密钥与附加锚输入只能各出现一次）");
        }
        return entries;
    }

    // 从系统随机源读一段字节
    void ReadRandom(uint8_t* out, std::size_t bytes) {
        const int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) throw std::runtime_error("无法打开 /dev/urandom");
        std::size_t done = 0;
        while (done < bytes) {
            const ssize_t got = read(fd, out + done, bytes - done);
            if (got <= 0) {
                close(fd);
                throw std::runtime_error("读取 /dev/urandom 失败");
            }
            done += static_cast<std::size_t>(got);
        }
        close(fd);
    }

    // 读整个文件；读失败抛异常
    std::vector<uint8_t> ReadFile(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("无法读取 " + path.string());
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 覆盖写整个文件；mode 用于首次创建（如 0600 的私钥）
    void WriteFile(const fs::path& path, const uint8_t* data, std::size_t bytes, int mode) {
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
        if (fd < 0) throw std::runtime_error("无法写入 " + path.string());
        std::size_t done = 0;
        while (done < bytes) {
            const ssize_t put = write(fd, data + done, bytes - done);
            if (put <= 0) {
                close(fd);
                throw std::runtime_error("写入失败 " + path.string());
            }
            done += static_cast<std::size_t>(put);
        }
        close(fd);
    }

    // 生成内嵌信任锚的 C++ 源：条目按 KeyId 升序，同一组输入产出同样的源文本
    void WriteAnchorCpp(const fs::path& path, const std::vector<AnchorEntry>& entries) {
        std::string text;
        text += "/* TrustAnchors.cpp\n";
        text += "    引导器内嵌的信任锚表（生成物）：由 Tools/Bin/MakeKeys 从当前密钥与附加锚输入合并生成\n";
        text += "\n";
        text += "    附加锚输入是进版本库的轮换窗口列表（MakeKeys --anchors），当前密钥来自仓库外的开发期种子；\n";
        text += "    本文件不进版本库：换锚 = 重新生成密钥并重建引导器\n";
        text += "*/\n";
        text += "\n";
        text += "#include <TrustAnchor.hpp>\n";
        text += "\n";
        text += "namespace Baleen {\n";
        text += "    namespace Crypto {\n";
        text += "        // 信任锚表：KeyId 与 Ed25519 公钥，extern 保持外部链接\n";
        text += "        extern const TrustAnchor kTrustAnchors[] = {\n";
        char line[128];
        for (const auto& entry : entries) {
            std::snprintf(line, sizeof(line), "            { %u, {", static_cast<unsigned>(entry.keyId));
            text += line;
            for (std::size_t i = 0; i < entry.publicKey.size(); ++i) {
                if (i % 8 == 0) text += "\n                ";
                std::snprintf(line, sizeof(line), "0x%02x, ", entry.publicKey[i]);
                text += line;
            }
            text += "\n            } },\n";
        }
        text += "        };\n";
        text += "        // 表内锚数量\n";
        std::snprintf(line, sizeof(line), "        extern const uint32_t kTrustAnchorCount = %u;\n", static_cast<unsigned>(entries.size()));
        text += line;
        text += "        // 超容量的锚表在编译期拦下，不留给运行期静默截断\n";
        text += "        static_assert(kTrustAnchorCount <= kTrustAnchorMaxCount, \"信任锚表超过 TrustAnchor.hpp 的容量\");\n";
        text += "    }\n";
        text += "}\n";
        WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size(), 0644);
    }

    // 生成纯文本锚表：与 C++ 源同源的同一张表，每行 "<KeyId> <公钥 64 位十六进制>"，按 KeyId 升序
    // 格式与 --anchors 输入一致，因此这份文本可以被签名工具直接读回核对；同一组输入产出同样的文本
    void WriteAnchorText(const fs::path& path, const std::vector<AnchorEntry>& entries) {
        std::string text;
        static const char kDigits[] = "0123456789abcdef";
        for (const auto& entry : entries) {
            text += std::to_string(entry.keyId);
            text += ' ';
            for (const uint8_t byte : entry.publicKey) {
                text += kDigits[byte >> 4];
                text += kDigits[byte & 0x0f];
            }
            text += '\n';
        }
        WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size(), 0644);
    }
}

// 入口：解析参数，生成或复用密钥并可选导出锚源
int main(int argc, char** argv) {
    try {
        fs::path outDir;
        fs::path anchorCpp;
        fs::path anchorText;
        fs::path anchors;
        uint64_t keyId = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&](const char* what) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("选项 ") + what + " 缺少参数");
                return argv[++i];
            };
            if (arg == "-h" || arg == "--help") {
                Usage();
                return 0;
            } else if (arg == "--out") outDir = next("--out");
            else if (arg == "--anchor-cpp") anchorCpp = next("--anchor-cpp");
            else if (arg == "--anchor-text") anchorText = next("--anchor-text");
            else if (arg == "--anchors") anchors = next("--anchors");
            else if (arg == "--key-id") keyId = hostbuild::ParseNumber(next("--key-id"), "密钥编号（--key-id）");
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        if (outDir.empty()) {
            Usage();
            return 2;
        }
        if (keyId < 1 || keyId > 255) throw std::runtime_error("--key-id 必须在 1..255 内");
        if (!anchors.empty() && anchorCpp.empty() && anchorText.empty()) throw std::runtime_error("--anchors 只用于生成锚表，须与 --anchor-cpp 或 --anchor-text 一起使用");
        fs::create_directories(outDir);

        const fs::path seedPath = outDir / kSeedName;
        const fs::path publicPath = outDir / kPublicName;

        // 种子：不存在时生成；已存在时复用，重复构建不会换锚
        std::array<uint8_t, hostbuild::kEd25519SeedBytes> seed{};
        std::array<uint8_t, hostbuild::kEd25519PublicKeyBytes> publicKey{};
        std::array<uint8_t, hostbuild::kEd25519PrivateKeyBytes> privateKey{};
        if (fs::exists(seedPath)) {
            const std::vector<uint8_t> stored = ReadFile(seedPath);
            if (stored.size() != seed.size()) throw std::runtime_error("种子文件长度不是 32 字节：" + seedPath.string());
            std::memcpy(seed.data(), stored.data(), seed.size());
            hostbuild::Ed25519DeriveKey(seed, publicKey, privateKey);
            std::cout << "MakeKeys：复用已有密钥 " << seedPath.string() << "\n";
        } else {
            ReadRandom(seed.data(), seed.size());
            hostbuild::Ed25519DeriveKey(seed, publicKey, privateKey);
            WriteFile(seedPath, seed.data(), seed.size(), 0600);
            std::cout << "MakeKeys：已生成开发期密钥 " << seedPath.string() << "（权限 600）\n";
        }
        // 公钥文件与种子派生结果保持一致
        WriteFile(publicPath, publicKey.data(), publicKey.size(), 0644);

        if (!anchorCpp.empty() || !anchorText.empty()) {
            // 附加锚输入：轮换窗口里仍要承认的旧公钥，进版本库；与当前密钥合并成同一张表
            const std::vector<AnchorEntry> extra = anchors.empty() ? std::vector<AnchorEntry>{} : ReadAnchors(anchors);
            const std::vector<AnchorEntry> entries = MergeAnchors(static_cast<uint8_t>(keyId), publicKey, extra);
            std::string ids;
            for (const auto& entry : entries) {
                if (!ids.empty()) ids += "、";
                ids += std::to_string(entry.keyId);
            }
            // 路径可以只给文件名：没有父目录时不建目录，避免把空路径当目录创建
            if (!anchorCpp.empty()) {
                if (!anchorCpp.parent_path().empty()) fs::create_directories(anchorCpp.parent_path());
                WriteAnchorCpp(anchorCpp, entries);
                std::cout << "MakeKeys：已生成信任锚源 " << anchorCpp.string() << "（" << entries.size() << " 条；KeyId " << ids << "）\n";
            }
            if (!anchorText.empty()) {
                if (!anchorText.parent_path().empty()) fs::create_directories(anchorText.parent_path());
                WriteAnchorText(anchorText, entries);
                std::cout << "MakeKeys：已生成信任锚文本 " << anchorText.string() << "（" << entries.size() << " 条；KeyId " << ids << "）\n";
            }
            // 吊销的前提检查：签名吊销记录必须由一把未被吊销的密钥签，锚表里只有一把密钥时，
            // 泄露之后没有任何密钥能签这份记录——所以第二把要在轮换窗口就装进设备，这里只警告不失败
            if (entries.size() < 2) {
                std::cerr << "MakeKeys: 警告 信任锚表只有 " << entries.size()
                          << " 条：该密钥泄露后无法用签名吊销记录吊销它，请把第二把公钥放进轮换窗口的保留锚条目"
                             "（--anchors）\n";
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MakeKeys: " << error.what() << "\n";
        return 1;
    }
}
