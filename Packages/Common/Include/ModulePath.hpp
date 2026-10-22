/* ModulePath.hpp
    模块集内路径的解析与模块标识派生：Baleen 与内核共用同一份规则

    路径形态唯一：Modules/<逻辑名>.<major>.<minor>.<patch>.osm
    - Modules/ 前缀、.osm 后缀固定；逻辑名是 [A-Z][A-Za-z0-9]*，最多 255 字节
    - 逻辑名之后恰好是三个十进制版本分量，各不超过 0xFFFF
    - 逻辑名之后允许出现子目录（如 Modules/Optional/<逻辑名>...），此时取不出逻辑名，
      按"不是引导闭包成员"处理：引导闭包成员必须平铺在 Modules/ 下

    ModuleId = SHA-256("LPMOD01" || 逻辑名 ASCII)[0:16)，与 MakeContract 的生成规则同源
    派生函数以摘要实现类型为模板参数：Baleen 用 Baleen::Digest::Sha256（32 位计数兼容视图），
    内核用 Common::Digest::Sha256（64 位计数），两者算法一致、计数宽度不同

    本头只做纯语法解析与派生，不做文件名拼接、不读写文件、不依赖平台设施
*/

#pragma once
#include <stdint.h>

#include <ModuleId.hpp>

namespace Common {
    namespace ModulePath {
        namespace Detail {
            // 行是否以给定字面量起头：字面量用长度给定时不依赖结尾零
            inline bool StartsWith(const uint8_t* bytes, uint32_t bytesLength, const char* literal, uint32_t literalBytes) {
                if (bytes == nullptr || bytesLength < literalBytes) return false;
                for (uint32_t index = 0; index < literalBytes; ++index) if (bytes[index] != static_cast<uint8_t>(literal[index])) return false;
                return true;
            }

            // 解析十进制定点数并推进游标：零位、非数字、超过 kMaxValue 都返回 false
            template <uint64_t kMaxValue>
            bool ParseDecimal(const uint8_t*& text, uint32_t& remaining, uint64_t& value) {
                constexpr uint64_t kLimit[10] = {
                    (kMaxValue - 0) / 10, (kMaxValue - 1) / 10, (kMaxValue - 2) / 10, (kMaxValue - 3) / 10, (kMaxValue - 4) / 10,
                    (kMaxValue - 5) / 10, (kMaxValue - 6) / 10, (kMaxValue - 7) / 10, (kMaxValue - 8) / 10, (kMaxValue - 9) / 10,
                };
                uint64_t result = 0;
                uint32_t digits = 0;
                while (remaining > 0 && text[0] >= '0' && text[0] <= '9') {
                    const uint32_t digit = static_cast<uint32_t>(text[0] - '0');
                    if (result > kLimit[digit]) return false;
                    result = result * 10 + digit;
                    ++digits;
                    ++text;
                    --remaining;
                }
                if (digits == 0) return false;
                value = result;
                return true;
            }
        }

        /* 从集内相对路径里取出模块逻辑名
            路径必须是 Modules/<逻辑名>.<major>.<minor>.<patch>.osm；成功时 name 指向路径内部，
            nameBytes 为逻辑名字节数（不以空字符结尾）；形态不符（含子目录、版本分量非法等）返回 false
        */
        inline bool ExtractName(const uint8_t* path, uint32_t pathBytes, const uint8_t*& name, uint32_t& nameBytes) {
            const char kModulesPrefix[] = "Modules/";
            const char kSuffix[] = ".osm";
            const uint32_t prefixBytes = static_cast<uint32_t>(sizeof(kModulesPrefix) - 1);
            const uint32_t suffixBytes = static_cast<uint32_t>(sizeof(kSuffix) - 1);
            if (path == nullptr) return false;
            if (pathBytes <= prefixBytes + suffixBytes) return false;
            if (!Detail::StartsWith(path, pathBytes, kModulesPrefix, prefixBytes)) return false;
            const uint8_t* const rest = path + prefixBytes;
            const uint32_t restBytes = pathBytes - prefixBytes;
            if (!Detail::StartsWith(rest + restBytes - suffixBytes, suffixBytes, kSuffix, suffixBytes)) return false;
            const uint32_t stemBytes = restBytes - suffixBytes;
            uint32_t nameLength = 0;
            while (nameLength < stemBytes && rest[nameLength] != '.') ++nameLength;
            if (nameLength == 0 || nameLength == stemBytes) return false;
            if (rest[0] < 'A' || rest[0] > 'Z') return false;
            for (uint32_t index = 1; index < nameLength; ++index) {
                const uint8_t c = rest[index];
                const bool digit = c >= '0' && c <= '9';
                const bool lower = c >= 'a' && c <= 'z';
                const bool upper = c >= 'A' && c <= 'Z';
                if (!digit && !lower && !upper) return false;
            }
            // 逻辑名之后必须恰好是三个十进制版本号，各不超过 0xFFFF
            uint32_t position = nameLength;
            for (uint32_t component = 0; component < 3; ++component) {
                if (position >= stemBytes || rest[position] != '.') return false;
                ++position;
                const uint8_t* text = rest + position;
                uint32_t remaining = stemBytes - position;
                uint64_t value = 0;
                if (!Detail::ParseDecimal<0xFFFF>(text, remaining, value)) return false;
                position = stemBytes - remaining;
            }
            if (position != stemBytes) return false;
            name = rest;
            nameBytes = nameLength;
            return true;
        }

        /* 由逻辑名派生模块标识：SHA-256("LPMOD01" || 逻辑名 ASCII) 的前 16 字节
            Sha256 是摘要实现类型（Begin / Update / Finish 三个方法）；失败返回 false
            与 MakeContract 的生成规则同源，Baleen 与内核不维护第二份成员表
        */
        template <typename Sha256>
        inline bool DeriveModuleId(const uint8_t* name, uint32_t nameBytes, ModuleId& id) {
            constexpr char kPrefix[] = "LPMOD01";
            const uint32_t prefixBytes = static_cast<uint32_t>(sizeof(kPrefix) - 1);
            if (name == nullptr || nameBytes == 0 || nameBytes > 255) return false;
            uint8_t buffer[prefixBytes + 255];
            for (uint32_t index = 0; index < prefixBytes; ++index) buffer[index] = static_cast<uint8_t>(kPrefix[index]);
            for (uint32_t index = 0; index < nameBytes; ++index) buffer[prefixBytes + index] = name[index];
            uint8_t digest[32] = {};
            Sha256 sha;
            sha.Begin();
            sha.Update(buffer, prefixBytes + nameBytes);
            sha.Finish(digest);
            for (uint32_t index = 0; index < sizeof(id.bytes); ++index) id.bytes[index] = digest[index];
            return true;
        }
    }
}
