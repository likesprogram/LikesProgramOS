/* Path.hpp
    统一路径索引：所有文件系统共用同一种路径形式与同一套分量切分，卷内查找各实现自理

    统一的是**路径的写法和切分**，不是名字的比较规则：Ext4 区分大小写，ISO9660 与
    FAT32 不区分，把大小写折叠写进共用层会让其中一侧出现读不到的路径。因此本文件
    只做语法校验与切分，比较语义由实现按卷格式选定，并经 FileSystemCaps::nameCase 报告
    共用的三种比较（区分大小写、ASCII 折叠、逐字节）供实现直接调用，避免各写一份

    路径形式：以 '/' 起头的绝对路径，'/' 分隔分量，根路径为 "/"；分量非空、不含 "."
    与 ".."，不以 '/' 结尾；非 ASCII 字节不做字符集变换与换行翻译，按原字节使用
*/

#pragma once
#include <stdint.h>

#include <Storage/Error.hpp>

namespace Storage {
    // 统一路径的语法边界
    struct PathRules {
        static constexpr char kSeparator = '/';          // 分量分隔符
        static constexpr uint32_t kMaxBytes = 1024;      // 整条路径的字节数上限，含起头分隔符，不含结尾零
        static constexpr uint32_t kMaxNameBytes = 255;   // 单个分量的字节数上限
    };

    // 卷内名字的比较语义：由实现按卷格式选定，调用方从 FileSystemCaps::nameCase 得知
    enum class NameCase : uint32_t {
        Sensitive,     // 逐字节比较：Ext4 等大小写敏感的格式，卷内名字原样保存
        Insensitive,   // ASCII 大小写折叠后比较：ISO9660 与 FAT32，卷内名字通常以大写保存
    };

    // 校验统一路径的语法：指针为空、不是绝对路径、空分量、"." 与 ".." 返回 InvalidArgument
    // 超出长度上限返回 NameTooLong；只做语法判定，不访问卷，也不折叠大小写
    Error CheckPath(const char* path);

    // 路径遍历器：按 '/' 切出分量，供实现逐段完成自己的查找
    // 只切分与计数：不折叠大小写、不解析符号链接、不拼接字符串
    class PathWalker {
    public:
        // 绑定一条路径；语法不通过时 Check() 给出原因，此时没有分量
        explicit PathWalker(const char* path);
        // 绑定时语法检查的结果；不是 Ok 时其余查询都不可用
        Error Check() const { return m_check; }
        // 是否还有待处理分量；根路径下为 false
        bool HasName() const { return m_name != nullptr; }
        // 当前分量起址；没有分量时为空
        const char* Name() const { return m_name; }
        // 当前分量字节数；没有分量时为 0
        uint32_t NameBytes() const { return m_nameBytes; }
        // 前进到下一个分量；已到末尾时保持没有分量
        void Advance();
        // 路径里的分量总数；根路径为 0
        uint32_t Count() const { return m_count; }

    private:
        // 从名字起址到下一个分隔符或串尾的字节数
        static uint32_t SegmentBytes(const char* name);

        Error m_check = Error::InvalidArgument;   // 绑定时语法检查的结果
        const char* m_name = nullptr;             // 当前分量起址；空表示没有分量
        uint32_t m_nameBytes = 0;                 // 当前分量字节数
        uint32_t m_count = 0;                     // 分量总数
    };

    // 名字比较：逐字节相等，区分大小写
    bool NameEquals(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes);

    // 名字比较：ASCII 大小写折叠后相等；非 ASCII 字节按原值比较
    bool NameEqualsIgnoreCase(const char* left, uint32_t leftBytes, const char* right, uint32_t rightBytes);

    // ASCII 大写折叠单个字节；非 ASCII 与已是大写的字节原样返回
    constexpr char ToUpperAscii(char byte) {
        return (byte >= 'a' && byte <= 'z') ? static_cast<char>(byte - 'a' + 'A') : byte;
    }

    // ASCII 小写折叠单个字节；非 ASCII 与已是大写的字节原样返回
    constexpr char ToLowerAscii(char byte) {
        return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte - 'A' + 'a') : byte;
    }
}
