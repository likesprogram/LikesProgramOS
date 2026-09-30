/* Path.cpp
    统一路径的语法校验、分量切分与共用名字比较的实现
*/

#include <Storage/Path.hpp>

namespace Storage {
    Error CheckPath(const char* path) {
        if (path == nullptr) return Error::InvalidArgument;
        // 起点必须是分隔符：统一路径只有绝对形式
        if (path[0] != PathRules::kSeparator) return Error::InvalidArgument;
        uint32_t length = 0;
        while (path[length] != '\0') {
            if (length >= PathRules::kMaxBytes) return Error::NameTooLong;
            ++length;
        }
        // 根路径
        if (length == 1) return Error::Ok;
        // 结尾必须是分量："/usr/" 这类写法不合法
        if (path[length - 1] == PathRules::kSeparator) return Error::InvalidArgument;
        // 逐分量检查：空分量、"." 与 ".."、单个名字的长度上限
        uint32_t nameBytes = 0;
        for (uint32_t i = 1; i <= length; ++i) {
            const char byte = path[i];
            if (byte != PathRules::kSeparator && byte != '\0') {
                ++nameBytes;
                if (nameBytes > PathRules::kMaxNameBytes) return Error::NameTooLong;
                continue;
            }
            if (nameBytes == 0) return Error::InvalidArgument;
            const char* const name = path + i - nameBytes;
            if (nameBytes == 1 && name[0] == '.') return Error::InvalidArgument;
            if (nameBytes == 2 && name[0] == '.' && name[1] == '.') return Error::InvalidArgument;
            nameBytes = 0;
        }
        return Error::Ok;
    }

    uint32_t PathWalker::SegmentBytes(const char* name) {
        uint32_t bytes = 0;
        while (name[bytes] != '\0' && name[bytes] != PathRules::kSeparator) ++bytes;
        return bytes;
    }

    PathWalker::PathWalker(const char* path) {
        m_check = CheckPath(path);
        if (m_check != Error::Ok) return;
        const char* const first = path + 1;
        if (*first == '\0') return;   // 根路径：没有分量
        m_name = first;
        m_nameBytes = SegmentBytes(first);
        // 语法已通过，分量总数就是分隔符数加一，逐字节数一遍即可
        m_count = 1;
        for (const char* cursor = first; *cursor != '\0'; ++cursor) {
            if (*cursor == PathRules::kSeparator) ++m_count;
        }
    }

    void PathWalker::Advance() {
        if (m_name == nullptr) return;
        const char* const next = m_name + m_nameBytes;
        if (*next == '\0') {
            m_name = nullptr;
            m_nameBytes = 0;
            return;
        }
        m_name = next + 1;
        m_nameBytes = SegmentBytes(m_name);
    }

    bool NameEquals(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes) {
        if (left == nullptr || right == nullptr) return false;
        if (leftBytes != rightBytes) return false;
        for (uint32_t i = 0; i < leftBytes; ++i) {
            if (left[i] != right[i]) return false;
        }
        return true;
    }

    bool NameEqualsIgnoreCase(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes) {
        if (left == nullptr || right == nullptr) return false;
        if (leftBytes != rightBytes) return false;
        for (uint32_t i = 0; i < leftBytes; ++i) {
            if (ToUpperAscii(left[i]) != ToUpperAscii(right[i])) return false;
        }
        return true;
    }
}
