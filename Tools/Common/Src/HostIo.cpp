/* HostIo.cpp
    宿主文件读写、布局记账与构建清单实现
*/

#include <HostIo.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace hostbuild {
    uint64_t FileSize(const std::string& path) {
        std::error_code error;
        const uint64_t size = std::filesystem::file_size(path, error);
        if (error) throw std::runtime_error("无法读取文件：" + path);
        return size;
    }

    std::vector<uint8_t> ReadFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("无法读取文件：" + path);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::vector<uint8_t> ReadHead(const std::string& path, uint64_t bytes) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("无法读取文件：" + path);
        std::vector<uint8_t> head(static_cast<std::size_t>(bytes));
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(in.gcount()));
        return head;
    }

    std::string BaseName(const std::string& path) { return std::filesystem::path(path).filename().string(); }

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

    std::string JsonPlaced(const Placed* placed) {
        if (placed == nullptr) return "null";
        return "{\"offset\": " + std::to_string(placed->offset) + ", \"bytes\": " + std::to_string(placed->bytes) + "}";
    }
}
