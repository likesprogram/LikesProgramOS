#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace makeiso {
    // 宿主文件与路径
    uint64_t FileSize(const std::string& path);
    std::vector<uint8_t> ReadFile(const std::string& path);
    // 只读文件开头 bytes 字节（文件更短时返回实际长度）；供结构头校验用，避免整份读入
    std::vector<uint8_t> ReadHead(const std::string& path, uint64_t bytes);
    std::string BaseName(const std::string& path);

    // 解析十进制或 0x 前缀的整数；失败时按 what 报错
    uint64_t ParseNumber(const std::string& text, const std::string& what);

    constexpr uint64_t AlignUp(uint64_t value, uint64_t align) { return (value + align - 1) / align * align; }

    // 布局记账

    // 构建清单里的一段载荷
    struct Placed {
        std::string name;
        uint64_t offset = 0;
        uint64_t bytes = 0;
    };

    // 已占用区间：任何两段载荷都不允许重叠，重叠就是布局错误
    class Reservations {
    public:
        // 预留一段区间，name 用于报错信息
        void Reserve(const std::string& name, uint64_t offset, uint64_t bytes);
    private:
        // 预留区间的描述
        struct Range {
            std::string name;    // 用于报错信息
            uint64_t begin = 0;  // 起始字节偏移
            uint64_t end = 0;    // 结束字节偏移（不含）
        };
        std::vector<Range> m_ranges;
    };

    // 构建清单
    std::string JsonEscape(const std::string& text);
    std::string JsonPlaced(const Placed* placed);  // 空指针写 null
}
