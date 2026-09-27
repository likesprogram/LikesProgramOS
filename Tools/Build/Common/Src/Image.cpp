#include "Image.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace makeiso {
    Image::Image(const std::string& path) : file_(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc) {
        if (!file_) throw std::runtime_error("无法创建镜像：" + path);
    }

    void Image::Seek(uint64_t offset) {
        file_.seekp(static_cast<std::streamoff>(offset));
        if (!file_) throw std::runtime_error("镜像定位失败，偏移 " + std::to_string(offset));
    }

    void Image::Write(uint64_t offset, std::span<const uint8_t> bytes) {
        if (bytes.empty()) return;
        Seek(offset);
        file_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file_) throw std::runtime_error("镜像写入失败，偏移 " + std::to_string(offset));
        size_ = std::max(size_, offset + bytes.size());
    }

    void Image::Fill(uint64_t offset, uint64_t bytes, uint8_t value) {
        std::array<char, 64 * 1024> buffer;
        buffer.fill(static_cast<char>(value));
        uint64_t written = 0;
        while (written < bytes) {
            const auto chunk = static_cast<std::size_t>(std::min<uint64_t>(bytes - written, buffer.size()));
            Seek(offset + written);
            file_.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!file_) throw std::runtime_error("镜像填充失败，偏移 " + std::to_string(offset + written));
            written += chunk;
        }
        size_ = std::max(size_, offset + bytes);
    }

    void Image::ExtendTo(uint64_t bytes) {
        if (bytes > size_) Fill(size_, bytes - size_, 0);
    }

    uint64_t Image::CopyFile(uint64_t offset, const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("无法读取载荷文件：" + path);
        std::array<char, 1 << 20> buffer;
        uint64_t copied = 0;
        while (true) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = in.gcount();
            if (got <= 0) break;
            const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.data());
            Write(offset + copied, std::span<const uint8_t>(bytes, static_cast<std::size_t>(got)));
            copied += static_cast<uint64_t>(got);
        }
        if (!in.eof()) throw std::runtime_error("读取载荷文件失败：" + path);
        return copied;
    }
}
