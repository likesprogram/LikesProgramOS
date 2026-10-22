/* main.cpp
    MakeContract：模块契约层生成器，读 Interfaces.def 与 BootClosure.def 生成契约头与常量表

    生成 10 份契约头，默认写到 Packages/Common/Out/Contract，目录可被 --out 覆盖；
    生成头不提交，输入 .def 与冻结表提交
    --check 只校验不写文件：.def 语法与排序、标识唯一、提供者固定、闭包表、冻结比对
    --freeze 把当前定义的每个接口版本追加进冻结表，已冻结行只读比对
    输出确定性：不写时间戳、构建路径、用户名，同一输入两次生成的字节一致
    退出码：0 成功；1 输入或校验失败（stderr 带文件名与行号）；2 用法错误
*/

#include "Contract.h"
#include "Generate.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
    // 默认契约输入目录：相对仓库根
    constexpr const char* kDefaultContractDir = "Packages/Common/Contract";
    // 默认生成目录：随 Common 的构建约定，可被 --out 覆盖
    constexpr const char* kDefaultOutDir = "Packages/Common/Out/Contract";
    // 冻结表的默认文件名
    constexpr const char* kFrozenName = "Frozen.tsv";

    // 命令行选项
    struct Options {
        std::string contractDir = kDefaultContractDir;                  // 契约输入目录
        std::string outDir = kDefaultOutDir;                            // 生成目录
        std::string frozenPath;                                         // 冻结表输出路径，空表示用输入目录下的默认名
        bool check = false;                                             // 只校验不写文件
        bool freeze = false;                                            // 只写冻结表
        bool moduleCheck = false;                                       // 模块契约校验：读 Module.mk 的导出/导入声明
        std::string logicalName;                                        // 模块逻辑名：闭包单例提供者按它核对
        std::vector<contract::ModuleExportDecl> exportDecls;            // CONTRACT_EXPORTS 解析结果
        std::vector<contract::ModuleImportDecl> importDecls;            // CONTRACT_IMPORTS 解析结果
    };

    // 解析模块契约声明里的十进制字段：无前导零，取值在范围内
    uint32_t ParseContractNumber(const std::string& text, const std::string& what, uint32_t minimum, uint32_t maximum) {
        if (text.empty()) throw contract::InputError("Module.mk", 0, what + "不能为空");
        if (text.size() > 1 && text[0] == '0') throw contract::InputError("Module.mk", 0, what + "不允许前导零：" + text);
        uint64_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') throw contract::InputError("Module.mk", 0, what + "必须是十进制数字：" + text);
            value = value * 10 + static_cast<uint64_t>(c - '0');
            if (value > maximum) throw contract::InputError("Module.mk", 0, what + "超出范围 " + std::to_string(minimum) + ".." + std::to_string(maximum) + "：" + text);
        }
        if (value < minimum) throw contract::InputError("Module.mk", 0, what + "超出范围 " + std::to_string(minimum) + ".." + std::to_string(maximum) + "：" + text);
        return static_cast<uint32_t>(value);
    }

    // 按冒号切分模块契约声明；字段数与预期不符时按 Module.mk 配置错误报出
    std::vector<std::string> SplitContractFields(const std::string& text, std::size_t expected, const std::string& what) {
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (true) {
            const std::size_t colon = text.find(':', begin);
            fields.push_back(text.substr(begin, colon == std::string::npos ? std::string::npos : colon - begin));
            if (colon == std::string::npos) break;
            begin = colon + 1;
        }
        if (fields.size() != expected) throw contract::InputError("Module.mk", 0, what + "：期望 " + std::to_string(expected) + " 段：" + text);
        return fields;
    }

    // 解析 "--export <稳定名>:<版本>"：槽数由接口定义推出，模块配置不写
    contract::ModuleExportDecl ParseExportDecl(const std::string& text) {
        const std::vector<std::string> fields = SplitContractFields(text, 2, "CONTRACT_EXPORTS 项必须是 <稳定名>:<版本>");
        contract::ModuleExportDecl declaration;
        declaration.name = fields[0];
        declaration.version = static_cast<uint16_t>(ParseContractNumber(fields[1], "导出项版本", 1, 65534));
        return declaration;
    }

    // 解析 "--import <稳定名>:<Min>:<Max>:<使用槽数>"：区间上限允许 65535（不设上限）
    contract::ModuleImportDecl ParseImportDecl(const std::string& text) {
        const std::vector<std::string> fields = SplitContractFields(text, 4, "CONTRACT_IMPORTS 项必须是 <稳定名>:<Min>:<Max>:<使用槽数>");
        contract::ModuleImportDecl declaration;
        declaration.name = fields[0];
        declaration.minVersion = static_cast<uint16_t>(ParseContractNumber(fields[1], "导入项下限", 1, 65534));
        declaration.maxVersion = static_cast<uint16_t>(ParseContractNumber(fields[2], "导入项上限", 1, 65535));
        declaration.slotCount = static_cast<uint16_t>(ParseContractNumber(fields[3], "导入项使用槽数", 1, 64));
        return declaration;
    }

    // 用法错误：退出码 2
    struct UsageError final : std::runtime_error {
        // 构造一条用法错误
        UsageError(const std::string& message) : std::runtime_error(message) {}
    };

    // 打印用法
    void Usage() {
        std::cout <<
            "MakeContract — 模块契约层生成器：读 Interfaces.def 与 BootClosure.def 生成契约头与常量表\n"
            "\n"
            "生成：MakeContract [--contract-dir <目录>] [--out <目录>]\n"
            "校验：MakeContract --check [--contract-dir <目录>]\n"
            "冻结：MakeContract --freeze [--contract-dir <目录>] [--frozen <文件>]\n"
            "模块：MakeContract --module-check --logical-name <逻辑名> [--export <稳定名>:<版本> ...] [--import <稳定名>:<Min>:<Max>:<使用槽数> ...] [--contract-dir <目录>]\n"
            "\n"
            "  --contract-dir  契约输入目录，默认 " + std::string(kDefaultContractDir) + "\n"
            "  --out           生成目录，默认 " + std::string(kDefaultOutDir) + "\n"
            "  --check         只校验不写文件：.def 语法与排序、标识唯一、提供者固定、闭包表、冻结比对\n"
            "  --freeze        把当前定义的每个接口版本追加进冻结表，已冻结行只读比对\n"
            "  --frozen        --freeze 的输出路径，默认 <contract-dir>/" + std::string(kFrozenName) + "\n"
            "  --module-check  模块契约校验：检查导出/导入声明并输出 PackModule 的 --export/--import 参数\n"
            "  --logical-name  模块逻辑名：闭包单例提供者按它与固定表核对\n"
            "  --export        模块导出声明 <稳定名>:<版本>，槽数由接口定义推出，可多次\n"
            "  --import        模块导入声明 <稳定名>:<Min>:<Max>:<使用槽数>，可多次\n"
            "  -h, --help      打印本用法\n"
            "\n"
            "退出码：0 成功；1 输入或校验失败；2 用法错误\n";
    }

    // 拼接目录与文件名：目录为空时返回文件名本身
    std::string JoinPath(const std::string& dir, const std::string& name) {
        if (dir.empty()) return name;
        if (dir.back() == '/') return dir + name;
        return dir + "/" + name;
    }

    // 写文本文件：按需创建父目录后整份覆盖
    void WriteFile(const std::string& path, const std::string& text) {
        const std::filesystem::path target(path);
        if (target.has_parent_path()) {
            std::error_code error;
            std::filesystem::create_directories(target.parent_path(), error);
            if (error) throw std::runtime_error("无法创建目录：" + target.parent_path().string());
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入文件：" + path);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) throw std::runtime_error("写入失败：" + path);
    }

    // 接口个数：同稳定名的多个版本只算一个
    std::size_t InterfaceCount(const contract::Contract& contract) {
        std::size_t count = 0;
        for (std::size_t index = 0; index < contract.interfaces.size(); ++index) {
            if (index == 0 || contract.interfaces[index - 1].name != contract.interfaces[index].name) ++count;
        }
        return count;
    }

    // 槽位总数：全部接口版本的槽行之和
    std::size_t TotalSlots(const contract::Contract& contract) {
        std::size_t count = 0;
        for (const contract::InterfaceVersion& block : contract.interfaces) count += block.slots.size();
        return count;
    }

    // 生成模式：生成全部生成物并写出，不改冻结表
    void RunGenerate(const contract::Contract& contract, const std::string& outDir) {
        const std::vector<contract::OutputFile> files = contract::Generate(contract);
        for (const contract::OutputFile& file : files) WriteFile(JoinPath(outDir, file.name), file.text);
        std::cout << "MakeContract: 已生成 " << files.size() << " 个文件到 " << outDir
                  << "（接口 " << InterfaceCount(contract) << "、版本块 " << contract.interfaces.size()
                  << "、槽 " << TotalSlots(contract) << "、闭包成员 " << contract.closure.size() << "）\n";
    }

    // 校验模式：全部检查已由 Load 完成，输出摘要
    void RunCheck(const contract::Contract& contract) {
        std::cout << "MakeContract: 校验通过（接口 " << InterfaceCount(contract) << "、版本块 " << contract.interfaces.size()
                  << "、槽 " << TotalSlots(contract) << "、闭包成员 " << contract.closure.size()
                  << "、冻结记录 " << contract.frozen.size() << "）\n";
    }

    // 冻结模式：把当前定义中尚未冻结的版本块追加进冻结表，已冻结行只读比对
    void RunFreeze(const contract::Contract& contract, const std::string& frozenPath) {
        std::map<std::pair<std::string, uint16_t>, contract::FrozenEntry> merged;
        for (const contract::FrozenEntry& entry : contract.frozen) merged.emplace(std::make_pair(entry.name, entry.version), entry);
        std::size_t added = 0;
        for (const contract::InterfaceVersion& block : contract.interfaces) {
            const std::pair<std::string, uint16_t> key(block.name, block.version);
            if (merged.count(key) != 0) continue;
            contract::FrozenEntry entry;
            entry.name = block.name;
            entry.version = block.version;
            entry.slotCount = static_cast<uint16_t>(block.slots.size());
            entry.digest = contract::SlotDigest(block);
            merged.emplace(key, entry);
            ++added;
        }
        std::string text =
            "# Frozen.tsv\n"
            "#   已发布接口版本的槽位冻结基线：稳定名、版本、槽数与槽位定义摘要\n"
            "#   摘要 = 该版本槽位定义（每槽 \"<槽号>\\t<槽名>\\t<签名>\" 以 LF 连接）的 SHA-256 前 16 字节\n"
            "#   由 MakeContract --freeze 追加，此后只增行、不改行；首次写入发生在 T5.1 的 v1 冻结\n";
        for (const auto& item : merged) {
            const contract::FrozenEntry& entry = item.second;
            text += entry.name + "\t" + std::to_string(entry.version) + "\t" + std::to_string(entry.slotCount) + "\t" + entry.digest + "\n";
        }
        WriteFile(frozenPath, text);
        std::cout << "MakeContract: 已写入冻结表 " << frozenPath << "（新增 " << added << " 行，共 " << merged.size() << " 行）\n";
    }
}

// 入口：解析选项后按模式生成、校验或冻结
int main(int argc, char** argv) {
    try {
        Options options;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            auto next = [&](const char* what) -> std::string {
                if (index + 1 >= argc) throw UsageError(std::string("选项 ") + what + " 缺少参数");
                return argv[++index];
            };
            if (argument == "-h" || argument == "--help") {
                Usage();
                return 0;
            } else if (argument == "--check") options.check = true;
            else if (argument == "--freeze") options.freeze = true;
            else if (argument == "--module-check") options.moduleCheck = true;
            else if (argument == "--contract-dir") options.contractDir = next("--contract-dir");
            else if (argument == "--out") options.outDir = next("--out");
            else if (argument == "--frozen") options.frozenPath = next("--frozen");
            else if (argument == "--logical-name") options.logicalName = next("--logical-name");
            else if (argument == "--export") options.exportDecls.push_back(ParseExportDecl(next("--export")));
            else if (argument == "--import") options.importDecls.push_back(ParseImportDecl(next("--import")));
            else throw UsageError("无法识别的参数：" + argument);
        }
        if (options.check && options.freeze) throw UsageError("--check 与 --freeze 不能同时使用");
        if (options.moduleCheck && (options.check || options.freeze)) throw UsageError("--module-check 不能与 --check 或 --freeze 同时使用");
        if (options.moduleCheck && options.logicalName.empty()) throw UsageError("--module-check 需要 --logical-name");
        if (!options.moduleCheck && (!options.logicalName.empty() || !options.exportDecls.empty() || !options.importDecls.empty())) throw UsageError("--logical-name / --export / --import 只与 --module-check 一起使用");
        if (options.contractDir.empty()) throw UsageError("--contract-dir 不能为空");
        if (options.outDir.empty()) throw UsageError("--out 不能为空");
        if (!options.freeze && !options.frozenPath.empty()) throw UsageError("--frozen 只与 --freeze 一起使用");

        const contract::Contract data = contract::Load(options.contractDir);
        if (options.moduleCheck) {
            const contract::ModuleContractResult result = contract::ValidateModuleContract(data, options.logicalName, options.exportDecls, options.importDecls);
            for (const std::string& argument : result.exportArgs) std::cout << argument << '\n';
            for (const std::string& argument : result.importArgs) std::cout << argument << '\n';
            return 0;
        }
        if (options.check) {
            RunCheck(data);
            return 0;
        }
        if (options.freeze) {
            const std::string frozenPath = options.frozenPath.empty() ? JoinPath(options.contractDir, kFrozenName) : options.frozenPath;
            RunFreeze(data, frozenPath);
            return 0;
        }
        RunGenerate(data, options.outDir);
        return 0;
    } catch (const UsageError& error) {
        std::cerr << "MakeContract: " << error.what() << "\n";
        Usage();
        return 2;
    } catch (const contract::InputError& error) {
        if (error.line == 0) std::cerr << "MakeContract: " << error.file << ": " << error.what() << "\n";
        else std::cerr << "MakeContract: " << error.file << ":" << error.line << ": " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "MakeContract: " << error.what() << "\n";
        return 1;
    }
}
