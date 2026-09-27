#include "HostIo.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace makeiso {
    // 宿主文件与路径
    uint64_t FileSize(const std::string& path) {
        std::error_code error;
        const uint64_t size = std::filesystem::file_size(path, error);
        if (error) throw std::runtime_error("无法读取文件：" + path);
        return size;
    }

    // 读取整个文件到内存
    std::vector<uint8_t> ReadFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("无法读取文件：" + path);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // 只读文件开头 bytes 字节（文件更短时返回实际长度）；供结构头校验用，避免整份读入
    std::vector<uint8_t> ReadHead(const std::string& path, uint64_t bytes) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("无法读取文件：" + path);
        std::vector<uint8_t> head(static_cast<std::size_t>(bytes));
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(in.gcount()));
        return head;
    }

    // 返回路径的基本名（去掉目录前缀）
    std::string BaseName(const std::string& path) { return std::filesystem::path(path).filename().string(); }

    // 解析十进制或 0x 前缀的整数；失败时按 what 报错
    uint64_t ParseNumber(const std::string& text, const std::string& what) {
        try {
            std::size_t pos = 0;
            const uint64_t value = std::stoull(text, &pos, 0);  // 支持 0x 前缀
            if (pos != text.size()) throw std::invalid_argument("trailing");
            return value;
        } catch (const std::exception&) {
            throw std::runtime_error("无法解析" + what + "：" + text);
        }
    }

    void Reservations::Reserve(const std::string& name, uint64_t offset, uint64_t bytes) {
        const uint64_t end = offset + bytes;
        for (const auto& range : m_ranges) if (offset < range.end && range.begin < end) throw std::runtime_error("布局重叠：" + name + " 与 " + range.name);
        m_ranges.push_back(Range{name, offset, end});
    }

    // 构建清单
    std::string JsonEscape(const std::string& text) {
        std::string out;
        out.reserve(text.size() + 2);
        for (const char c : text) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(c); break;
            }
        }
        return out;
    }

    // 构建清单里的一段载荷
    std::string JsonPlaced(const Placed* placed) {
        if (placed == nullptr) return "null"; // 空指针写 null
        return "{\"offset\": " + std::to_string(placed->offset) + ", \"bytes\": " + std::to_string(placed->bytes) + "}";
    }
}
