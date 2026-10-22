/* Elf.hpp
    ELF64 模块镜像的读取与到内核模块容器的翻译

    PackModule 读入 ELF64 ET_DYN 模块（不 strip），按节表取出可分配节、按符号表取入口与
    契约表符号、按 .rela.dyn 取动态重定位，翻译成 Osm.hpp 的容器写出输入
    构建侧首批只允许 R_X86_64_RELATIVE，出现其他重定位类型即失败
*/

#pragma once
#include <Osm.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace elf {
    // ELF 识别常量：只接受 64 位、小端、ET_DYN、x86-64
    constexpr std::uint8_t kClass64 = 2;
    constexpr std::uint8_t kDataLittleEndian = 1;
    constexpr std::uint16_t kTypeDyn = 3;
    constexpr std::uint16_t kMachineX86_64 = 62;
    // 节类型
    constexpr std::uint32_t kSectionProgbits = 1;
    constexpr std::uint32_t kSectionSymtab = 2;
    constexpr std::uint32_t kSectionStrtab = 3;
    constexpr std::uint32_t kSectionRela = 4;
    constexpr std::uint32_t kSectionNobits = 8;
    constexpr std::uint32_t kSectionDynsym = 11;
    constexpr std::uint32_t kSectionInitArray = 14;
    constexpr std::uint32_t kSectionFiniArray = 15;
    constexpr std::uint32_t kSectionPreinitArray = 16;
    // 节标志
    constexpr std::uint64_t kSectionFlagWrite = 0x1;
    constexpr std::uint64_t kSectionFlagAlloc = 0x2;
    constexpr std::uint64_t kSectionFlagExec = 0x4;
    constexpr std::uint64_t kSectionFlagTls = 0x400;
    // 符号类型
    constexpr std::uint8_t kSymbolNoType = 0;
    constexpr std::uint8_t kSymbolFunc = 2;
    constexpr std::uint8_t kSymbolFile = 4;
    // 重定位类型：首批只允许 R_X86_64_RELATIVE；32/32S 是绝对地址重定位，按构建错误拒绝
    constexpr std::uint32_t kRelocationRelative = 8;
    constexpr std::uint32_t kRelocation32 = 10;
    constexpr std::uint32_t kRelocation32S = 11;
    // 入口符号名
    constexpr const char* kEntryInitSymbol = "_Module_Init";
    constexpr const char* kEntryActivateSymbol = "_Module_Activate";
    constexpr const char* kEntryAbortSymbol = "_Module_Abort";
    // 契约表符号前缀：完整名为 _Contract_Export_<稳定名去点>_v<版本>
    constexpr const char* kExportSymbolPrefix = "_Contract_Export_";
    constexpr const char* kImportSymbolPrefix = "_Contract_Import_";

    // 一个节头
    struct Section {
        std::string name;                 // 节名
        std::uint32_t type = 0;           // 节类型
        std::uint64_t flags = 0;          // 节标志
        std::uint64_t address = 0;        // 装载地址（模块基址为 0 时的内存偏移）
        std::uint64_t offset = 0;         // 文件偏移
        std::uint64_t size = 0;           // 字节数
        std::uint32_t link = 0;           // 关联节下标
        std::uint64_t addralign = 0;      // 地址对齐
    };

    // 一个符号表条目
    struct Symbol {
        std::string name;                 // 符号名
        std::uint8_t info = 0;            // 类型与绑定
        std::uint16_t sectionIndex = 0;   // 定义所在节下标，0 表示未定义
        std::uint64_t value = 0;          // 地址（模块基址为 0 时的内存偏移）
        std::uint64_t size = 0;           // 符号字节数
    };

    // 一条动态重定位
    struct Relocation {
        std::uint32_t type = 0;           // 重定位类型
        std::uint64_t offset = 0;         // 被修正的内存偏移
        std::uint64_t addend = 0;         // 加数
    };

    // 已解析的 ELF64 模块
    class File {
    public:
        // 解析 ELF64 ET_DYN；失败抛出 std::runtime_error
        static File Parse(std::span<const std::uint8_t> bytes);
        // 按名字查找节，找不到返回 nullptr
        const Section* FindSection(std::string_view name) const;
        // 按名字查找符号（先 .symtab 后 .dynsym），找不到返回 nullptr
        const Symbol* FindSymbol(std::string_view name) const;
        // 全部节
        const std::vector<Section>& Sections() const;
        // 动态重定位
        const std::vector<Relocation>& Relocations() const;
        // 未定义外部符号的名字，空表示 nm -u 为空
        const std::vector<std::string>& UndefinedSymbols() const;
        // 读取一段可分配内存的初值；地址不落在任何有文件字节的可分配节时返回 false
        bool ReadMemory(std::uint64_t address, std::span<std::uint8_t> output) const;
    private:
        std::span<const std::uint8_t> m_bytes;   // 调用方持有的文件字节
        std::vector<Section> m_sections;         // 节表
        std::vector<Symbol> m_symbols;           // .symtab 与 .dynsym 的符号
        std::vector<Relocation> m_relocations;   // 动态重定位
        std::vector<std::string> m_undefined;    // 未定义外部符号
    };

    // 命令行声明的一条导出接口条目
    struct ExportDecl {
        std::string stableName;    // 接口稳定名
        std::uint16_t version = 0; // 接口版本
        std::uint16_t slotCount = 0; // 槽位数
    };

    // 命令行声明的一条导入接口条目
    struct ImportDecl {
        std::string stableName;          // 接口稳定名
        std::uint16_t minVersion = 0;    // 可接受的最低版本
        std::uint16_t maxVersion = 0;    // 可接受的最高版本；0xFFFF 表示不设上限
        std::uint16_t slotCount = 0;     // 使用槽位数
        std::uint16_t symbolVersion = 0; // 回填区符号里的版本；0 表示先试 MaxVersion 再试 MinVersion
    };

    // 命令行声明的一条模块依赖条目
    struct DependencyDecl {
        std::string logicalName;    // 被依赖模块的逻辑名
        std::uint16_t minMajor = 0; // 可接受的最低 major
        std::uint16_t maxMajor = 0; // 可接受的最高 major；0 表示不设上限
    };

    // 打包输入：模块身份、版本、ABI 与三张命令行声明的表
    struct PackInput {
        std::string moduleName;          // 模块逻辑名，派生 ModuleId
        std::uint16_t versionMajor = 0;  // 模块版本 major
        std::uint16_t versionMinor = 0;  // 模块版本 minor
        std::uint16_t versionPatch = 0;  // 模块版本 patch
        std::uint16_t abiMin = 1;        // 内核 ABI 下限
        std::uint16_t abiMax = 0xFFFF;   // 内核 ABI 上限
        std::vector<ExportDecl> exports; // 导出接口声明
        std::vector<ImportDecl> imports; // 导入接口声明
        std::vector<DependencyDecl> dependencies; // 模块依赖声明
    };

    // 把 ELF 模块与打包输入翻译成容器写出输入；失败抛出 std::runtime_error
    osm::Spec MakeSpec(const File& file, const PackInput& input);
}
