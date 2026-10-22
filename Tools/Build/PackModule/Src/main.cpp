/* main.cpp
    PackModule：把 ELF64 ET_DYN 模块打包成内核模块容器（.osm），或用同一份解析器核对容器

    打包：读未 strip 的 ELF64 模块，取三个入口符号、可分配节与动态重定位，按命令行声明的
    导出/导入/依赖表写出容器；写出后立即回读并走 Parse，保证写出器与校验器同源
    核对：--verify 用同一份 Osm.cpp 解析器复算全部容器约束，失败时打印英文原因
    退出码 0 成功、1 输入或校验失败、2 用法错误
*/

#include <Elf.hpp>
#include <HostIo.h>
#include <Osm.hpp>
#include <Sha256.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    // 用法错误：参数缺失、未知选项或模式冲突，对应退出码 2
    struct UsageError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // 打印用法
    void Usage() {
        std::cout <<
            "PackModule - pack an ELF64 ET_DYN module into an .osm container, or verify one\n"
            "\n"
            "Pack:   PackModule --elf <module.elf> --out <module.osm> --name <LogicalName>\n"
            "                   [--version <major>.<minor>.<patch>]\n"
            "                   [--abi-min <n>] [--abi-max <n>]\n"
            "                   [--export <Stable.Name>:<version>:<slots> ...]\n"
            "                   [--import <Stable.Name>:<min>:<max>:<slots>[:<symbolVersion>] ...]\n"
            "                   [--depends <LogicalName>:<minMajor>:<maxMajor> ...]\n"
            "Verify: PackModule --verify <module.osm>\n"
            "\n"
            "  --elf       unstripped ELF64 ET_DYN module; symbols _Module_Init,\n"
            "              _Module_Activate and _Module_Abort are read from its symbol table\n"
            "  --out       output .osm path\n"
            "  --name      module logical name (derives ModuleId, also the human-readable name)\n"
            "  --version   module version, default 1.0.0; major must be nonzero\n"
            "  --abi-min   minimum kernel ABI version, default 1\n"
            "  --abi-max   maximum kernel ABI version, default 0xFFFF (no upper limit)\n"
            "  --export    export table entry; the slot table is the ELF symbol\n"
            "              _Contract_Export_<stable name without dots>_v<version>\n"
            "  --import    import table entry; the patch area is the ELF symbol\n"
            "              _Contract_Import_<stable name without dots>_v<symbolVersion>\n"
            "  --depends   dependency table entry; maxMajor 0 means no upper limit\n"
            "  --verify    parse an .osm and recompute every container constraint; writes nothing\n"
            "\n"
            "Layout, limits and field semantics are fixed by the container format\n"
            "Interface/Module id derivation is fixed by the contract format\n"
            "Exit codes: 0 success, 1 input or verification failure, 2 usage error\n";
    }

    // 解析 16 位无符号十进制数或 0x 前缀十六进制数
    std::uint16_t ParseU16(const std::string& text, const char* what) {
        std::uint64_t value = 0;
        try {
            value = hostbuild::ParseNumber(text, what);
        } catch (const std::exception&) {
            throw std::runtime_error(std::string("cannot parse ") + what + ": " + text);
        }
        if (value > 0xFFFF) throw std::runtime_error(std::string(what) + " is beyond 16 bits: " + text);
        return static_cast<std::uint16_t>(value);
    }

    // 把命令行值按给定分隔符拆成固定字段数；字段为空即报错
    std::vector<std::string> SplitFields(const std::string& text, char separator, std::size_t expected, const char* what) {
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (true) {
            const std::size_t end = text.find(separator, begin);
            fields.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (fields.size() != expected) throw std::runtime_error(std::string(what) + " expects " + std::to_string(expected) + " separated fields: " + text);
        for (const std::string& field : fields) {
            if (field.empty()) throw std::runtime_error(std::string(what) + " has an empty field: " + text);
        }
        return fields;
    }

    // 解析 "--version major.minor.patch"
    void ParseVersion(const std::string& text, elf::PackInput& input) {
        const std::vector<std::string> fields = SplitFields(text, '.', 3, "--version");
        input.versionMajor = ParseU16(fields[0], "module version major");
        input.versionMinor = ParseU16(fields[1], "module version minor");
        input.versionPatch = ParseU16(fields[2], "module version patch");
        if (input.versionMajor == 0) throw std::runtime_error("module version major must be nonzero");
    }

    // 解析 "--export <稳定名>:<版本>:<槽数>"
    elf::ExportDecl ParseExport(const std::string& text) {
        const std::vector<std::string> fields = SplitFields(text, ':', 3, "--export");
        elf::ExportDecl declaration;
        declaration.stableName = fields[0];
        if (!osm::IsValidInterfaceName(declaration.stableName)) {
            throw std::runtime_error("invalid interface stable name in --export: " + declaration.stableName);
        }
        declaration.version = ParseU16(fields[1], "export version");
        declaration.slotCount = ParseU16(fields[2], "export slot count");
        if (declaration.version == 0) throw std::runtime_error("export version must be nonzero: " + text);
        if (declaration.slotCount < 1 || declaration.slotCount > 64) throw std::runtime_error("export slot count must be in 1..64: " + text);
        return declaration;
    }

    // 解析 "--import <稳定名>:<最低版本>:<最高版本>:<槽数>[:<符号版本>]"
    elf::ImportDecl ParseImport(const std::string& text) {
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (true) {
            const std::size_t end = text.find(':', begin);
            fields.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (fields.size() != 4 && fields.size() != 5) {
            throw std::runtime_error("--import expects <Stable.Name>:<min>:<max>:<slots>[:<symbolVersion>]: " + text);
        }
        for (const std::string& field : fields) {
            if (field.empty()) throw std::runtime_error("--import has an empty field: " + text);
        }
        elf::ImportDecl declaration;
        declaration.stableName = fields[0];
        if (!osm::IsValidInterfaceName(declaration.stableName)) {
            throw std::runtime_error("invalid interface stable name in --import: " + declaration.stableName);
        }
        declaration.minVersion = ParseU16(fields[1], "import min version");
        declaration.maxVersion = ParseU16(fields[2], "import max version");
        declaration.slotCount = ParseU16(fields[3], "import slot count");
        if (fields.size() == 5) declaration.symbolVersion = ParseU16(fields[4], "import symbol version");
        if (declaration.minVersion == 0) throw std::runtime_error("import min version must be nonzero: " + text);
        if (declaration.maxVersion != 0xFFFF && declaration.maxVersion < declaration.minVersion) {
            throw std::runtime_error("import version range is reversed: " + text);
        }
        if (declaration.slotCount < 1 || declaration.slotCount > 64) throw std::runtime_error("import slot count must be in 1..64: " + text);
        return declaration;
    }

    // 解析 "--depends <逻辑名>:<最低 major>:<最高 major>"
    elf::DependencyDecl ParseDependency(const std::string& text) {
        const std::vector<std::string> fields = SplitFields(text, ':', 3, "--depends");
        elf::DependencyDecl declaration;
        declaration.logicalName = fields[0];
        if (!osm::IsValidModuleName(declaration.logicalName)) {
            throw std::runtime_error("invalid module logical name in --depends: " + declaration.logicalName);
        }
        declaration.minMajor = ParseU16(fields[1], "dependency min major");
        declaration.maxMajor = ParseU16(fields[2], "dependency max major");
        if (declaration.minMajor == 0) throw std::runtime_error("dependency min major must be nonzero: " + text);
        if (declaration.maxMajor != 0 && declaration.maxMajor < declaration.minMajor) {
            throw std::runtime_error("dependency max major is below min major: " + text);
        }
        return declaration;
    }

    // 整份写出一段字节，失败即报错
    void WriteFile(const std::string& path, std::span<const std::uint8_t> bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open for writing: " + path);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) throw std::runtime_error("write failed: " + path);
    }

    // 计算并打印时的十六进制摘要
    std::string Sha256Text(std::span<const std::uint8_t> bytes) {
        hostbuild::Sha256 sha;
        sha.Begin();
        sha.Update(bytes);
        return osm::HexText(sha.Finish());
    }

    // 打印解析结果的摘要
    void PrintSummary(const char* prefix, const osm::Image& image) {
        std::cout << prefix << ": " << image.totalBytes << " bytes, " << image.sections.size() << " sections, "
                  << image.exports.size() << " exports, " << image.imports.size() << " imports, "
                  << image.dependencies.size() << " dependencies, " << image.relocations.size() << " relocations, "
                  << "ImageBytes 0x" << std::hex << image.imageBytes << std::dec
                  << ", ModuleId " << osm::HexText(image.moduleId) << "\n";
    }

    // 打包：读 ELF、翻译成容器输入、写出并回读自检
    void Pack(const std::string& elfPath, const std::string& outPath, const elf::PackInput& input) {
        const std::vector<std::uint8_t> elfBytes = hostbuild::ReadFile(elfPath);
        if (elfBytes.empty()) throw std::runtime_error("ELF input is empty: " + elfPath);
        const elf::File file = elf::File::Parse(elfBytes);
        const osm::Spec spec = elf::MakeSpec(file, input);
        const osm::Bytes image = osm::Build(spec);
        WriteFile(outPath, image);
        // 回读并走同一份解析器：写出器与校验器同源，写出的字节必须自己先通过校验
        const std::vector<std::uint8_t> written = hostbuild::ReadFile(outPath);
        const osm::Image parsed = osm::Parse(written);
        PrintSummary("PackModule: verify passed for the written file", parsed);
        std::cout << "PackModule: packed " << elfPath << " -> " << outPath << ", sha256=" << Sha256Text(image) << "\n";
    }

    // 核对：读入容器并复算全部约束
    void Verify(const std::string& path) {
        const std::vector<std::uint8_t> image = hostbuild::ReadFile(path);
        const osm::Image parsed = osm::Parse(image);
        PrintSummary("PackModule: verify passed", parsed);
        std::cout << "PackModule: verify OK " << path << ", sha256=" << Sha256Text(image) << "\n";
    }
}

// 入口：解析参数，按 --verify 选择打包或核对
int main(int argc, char** argv) {
    try {
        bool verify = false;
        std::string elfPath;
        std::string outPath;
        std::string verifyPath;
        bool hasVersion = false;
        bool hasAbiMin = false;
        bool hasAbiMax = false;
        elf::PackInput input;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            auto next = [&](const char* what) -> std::string {
                if (i + 1 >= argc) throw UsageError(std::string("option ") + what + " expects a value");
                return argv[++i];
            };
            if (argument == "-h" || argument == "--help") {
                Usage();
                return 0;
            } else if (argument == "--verify") {
                verify = true;
                verifyPath = next("--verify");
            } else if (argument == "--elf") elfPath = next("--elf");
            else if (argument == "--out") outPath = next("--out");
            else if (argument == "--name") input.moduleName = next("--name");
            else if (argument == "--version") {
                ParseVersion(next("--version"), input);
                hasVersion = true;
            } else if (argument == "--abi-min") {
                input.abiMin = ParseU16(next("--abi-min"), "kernel ABI minimum");
                hasAbiMin = true;
            } else if (argument == "--abi-max") {
                input.abiMax = ParseU16(next("--abi-max"), "kernel ABI maximum");
                hasAbiMax = true;
            } else if (argument == "--export") input.exports.push_back(ParseExport(next("--export")));
            else if (argument == "--import") input.imports.push_back(ParseImport(next("--import")));
            else if (argument == "--depends") input.dependencies.push_back(ParseDependency(next("--depends")));
            else throw UsageError("unknown option: " + argument);
        }
        if (verify) {
            if (!elfPath.empty() || !outPath.empty() || !input.moduleName.empty() || hasVersion || hasAbiMin || hasAbiMax
                || !input.exports.empty() || !input.imports.empty() || !input.dependencies.empty()) {
                throw UsageError("--verify does not accept packing options");
            }
            Verify(verifyPath);
            return 0;
        }
        if (elfPath.empty() || outPath.empty() || input.moduleName.empty()) {
            throw UsageError("packing needs --elf, --out and --name");
        }
        if (!osm::IsValidModuleName(input.moduleName)) throw std::runtime_error("invalid module logical name: " + input.moduleName);
        if (input.abiMin == 0) throw std::runtime_error("--abi-min must be nonzero");
        if (input.abiMax != 0xFFFF && input.abiMax < input.abiMin) throw std::runtime_error("--abi-max is below --abi-min");
        if (!hasVersion) {
            input.versionMajor = 1;
            input.versionMinor = 0;
            input.versionPatch = 0;
        }
        Pack(elfPath, outPath, input);
        return 0;
    } catch (const UsageError& error) {
        std::cerr << "PackModule: " << error.what() << "\n";
        Usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "PackModule: " << error.what() << "\n";
        return 1;
    }
}
