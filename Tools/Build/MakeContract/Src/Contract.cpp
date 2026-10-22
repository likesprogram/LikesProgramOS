/* Contract.cpp
    契约输入实现：解析 Interfaces.def、BootClosure.def 与冻结表

    字节级规则：纯 ASCII、LF 行尾、字段间单个空格、非注释行不许空行与首尾空白
    Interfaces.def 的全文件排序、同接口版本升序与版本块连续在解析时逐行检查；
    重复的 (稳定名, 版本) 按"重复提供者"直接拒绝
    标识派生只有这一处：SHA-256("LPIF01" || 稳定名) 与 SHA-256("LPMOD01" || 逻辑名)
    摘要取前 16 字节作标识，文本形式为 32 位小写十六进制；生成器与 CheckRegistry 都消费生成常量
*/

#include "Contract.h"

#include <HostIo.h>
#include <Sha256.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace contract {
    namespace {
        // 接口定义输入的文件名
        constexpr const char* kInterfacesFile = "Interfaces.def";
        // 闭包成员输入的文件名
        constexpr const char* kBootClosureFile = "BootClosure.def";
        // 冻结表的文件名
        constexpr const char* kFrozenFile = "Frozen.tsv";
        // 接口稳定名与模块逻辑名的最大字节数
        constexpr std::size_t kNameMaxBytes = 64;
        // 槽名的最大字节数
        constexpr std::size_t kSlotNameMaxBytes = 32;
        // 单个版本块的槽数范围
        constexpr std::size_t kSlotMinCount = 1;
        constexpr std::size_t kSlotMaxCount = 64;
        // 闭包成员固定数量
        constexpr std::size_t kClosureCount = 13;
        // 槽位定义摘要的十六进制字符数（SHA-256 前 16 字节）
        constexpr std::size_t kDigestHexChars = 32;

        // 拼接目录与文件名：目录为空时返回文件名本身
        std::string JoinPath(const std::string& dir, const char* name) {
            if (dir.empty()) return name;
            if (dir.back() == '/') return dir + name;
            return dir + "/" + name;
        }

        // 读整个文本文件；失败抛文件级输入错误
        std::string ReadText(const std::string& dir, const char* name) {
            const std::string path = JoinPath(dir, name);
            std::vector<uint8_t> bytes;
            try {
                bytes = hostbuild::ReadFile(path);
            } catch (const std::exception&) {
                throw InputError(name, 0, "无法读取文件");
            }
            return std::string(bytes.begin(), bytes.end());
        }

        // 按 LF 切行并做字节级检查：拒绝 BOM、CR 与 NUL，空文件报错
        std::vector<std::string> SplitLines(const std::string& file, const std::string& text) {
            if (text.empty()) throw InputError(file, 0, "文件为空");
            if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xef && static_cast<uint8_t>(text[1]) == 0xbb && static_cast<uint8_t>(text[2]) == 0xbf) throw InputError(file, 1, "不允许 UTF-8 BOM");
            std::vector<std::string> lines;
            std::size_t begin = 0;
            std::size_t line = 1;
            for (std::size_t index = 0; index <= text.size(); ++index) {
                const bool end = index == text.size();
                if (!end && text[index] != '\n') {
                    const uint8_t byte = static_cast<uint8_t>(text[index]);
                    if (byte == '\r') throw InputError(file, line, "不允许 CR 行尾，行尾必须是 LF");
                    if (byte == 0x00) throw InputError(file, line, "不允许 NUL 字节");
                    continue;
                }
                lines.push_back(text.substr(begin, index - begin));
                begin = index + 1;
                ++line;
            }
            // 末行带 LF 时上面会多切出一个空段，丢弃
            if (!lines.empty() && lines.back().empty()) lines.pop_back();
            return lines;
        }

        // 判断一行是否为注释行：以 '#' 开头整行跳过
        bool IsComment(const std::string& row) { return !row.empty() && row[0] == '#'; }

        // 检查非注释行不允许空行与首尾空白
        void CheckNoEdgeSpace(const std::string& file, const std::string& row, std::size_t line) {
            if (row.empty()) throw InputError(file, line, "不允许空行");
            if (row.front() == ' ' || row.front() == '\t') throw InputError(file, line, "行首不允许空白");
            if (row.back() == ' ' || row.back() == '\t') throw InputError(file, line, "行尾不允许空白");
        }

        // 判断字符是否为 ASCII 数字
        bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        // 判断字符是否为 ASCII 大写字母
        bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }

        // 判断字符是否为 ASCII 字母或数字
        bool IsAlnum(char c) { return IsUpper(c) || (c >= 'a' && c <= 'z') || IsDigit(c); }

        // 解析十进制整数：无前导零，取值在给定范围内
        uint32_t ParseDecimal(const std::string& file, const std::string& text, std::size_t line, const char* what, uint32_t minimum, uint32_t maximum) {
            if (text.empty()) throw InputError(file, line, std::string(what) + "不能为空");
            if (text.size() > 1 && text[0] == '0') throw InputError(file, line, std::string(what) + "不允许前导零");
            uint64_t value = 0;
            for (const char c : text) {
                if (!IsDigit(c)) throw InputError(file, line, std::string(what) + "必须是十进制数字");
                value = value * 10 + static_cast<uint64_t>(c - '0');
                if (value > maximum) throw InputError(file, line, std::string(what) + "超出范围 " + std::to_string(minimum) + ".." + std::to_string(maximum));
            }
            if (value < minimum) throw InputError(file, line, std::string(what) + "必须在 " + std::to_string(minimum) + ".." + std::to_string(maximum) + " 内");
            return static_cast<uint32_t>(value);
        }

        // 检查模块逻辑名：[A-Z][A-Za-z0-9]*，单分量，不超过 64 字节
        void CheckModuleName(const std::string& file, const std::string& name, std::size_t line, const char* what) {
            if (name.empty()) throw InputError(file, line, std::string(what) + "不能为空");
            if (name.size() > kNameMaxBytes) throw InputError(file, line, std::string(what) + "超过 64 字节");
            if (!IsUpper(name[0])) throw InputError(file, line, std::string(what) + "必须以大写字母开头");
            for (const char c : name) if (!IsAlnum(c)) throw InputError(file, line, std::string(what) + "含非法字符，只允许 ASCII 字母数字");
        }

        // 检查接口稳定名：[A-Z][A-Za-z0-9]*(\.[A-Z][A-Za-z0-9]*)+，不超过 64 字节
        void CheckInterfaceName(const std::string& file, const std::string& name, std::size_t line) {
            if (name.empty()) throw InputError(file, line, "稳定名不能为空");
            if (name.size() > kNameMaxBytes) throw InputError(file, line, "稳定名超过 64 字节");
            std::size_t componentBegin = 0;
            uint32_t components = 0;
            for (std::size_t index = 0; index <= name.size(); ++index) {
                const bool end = index == name.size() || name[index] == '.';
                if (end) {
                    if (index == componentBegin) throw InputError(file, line, "稳定名不允许空分量");
                    ++components;
                    componentBegin = index + 1;
                    continue;
                }
                const char c = name[index];
                const bool firstByte = index == componentBegin;
                if (firstByte ? !IsUpper(c) : !IsAlnum(c)) throw InputError(file, line, "稳定名分量必须以大写字母开头且只含 ASCII 字母数字");
            }
            if (components < 2) throw InputError(file, line, "稳定名至少要有两个分量");
        }

        // 检查槽名：[A-Z][A-Za-z0-9]*，不超过 32 字节
        void CheckSlotName(const std::string& file, const std::string& name, std::size_t line) {
            if (name.empty()) throw InputError(file, line, "槽名不能为空");
            if (name.size() > kSlotNameMaxBytes) throw InputError(file, line, "槽名超过 32 字节");
            if (!IsUpper(name[0])) throw InputError(file, line, "槽名必须以大写字母开头");
            for (const char c : name) if (!IsAlnum(c)) throw InputError(file, line, "槽名含非法字符，只允许 ASCII 字母数字");
        }

        // 签名允许的标量类型：只能按值或按指针出现，char 只允许指针
        bool IsScalarType(const std::string& type) {
            static const char* const kTypes[] = {
                "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int32_t", "int64_t", "void", "char",
                "KernelError", "Storage::Error", "FileReference",
                "DeviceHandle", "ClaimHandle", "BindHandle", "FileSystemId", "ObjectHandle",
                "ControllerHandle", "UsbDeviceHandle", "ClassHandle",
            };
            for (const char* candidate : kTypes) if (type == candidate) return true;
            return false;
        }

        // 签名允许的结构类型：只能以指针出现
        bool IsStructureType(const std::string& type) {
            static const char* const kTypes[] = {
                "ModuleId", "Boot::Info", "Storage::FileInfo", "Storage::FileSystemCaps",
                "DeviceFacts", "DeviceResources", "BindingFacts", "SystemVolumeFacts",
                "DriverDesc", "BlockDeviceOps", "FileSystemOps",
                "UsbSetupPacket", "UsbDeviceFacts", "UsbControllerOps", "UsbClassOps",
            };
            for (const char* candidate : kTypes) if (type == candidate) return true;
            return false;
        }

        // 校验一个返回类型或参数类型：const 前缀 + 基础类型 + 零到多个指针
        void CheckType(const std::string& file, const std::string& text, std::size_t line) {
            if (text.empty()) throw InputError(file, line, "签名类型不能为空");
            std::string type = text;
            bool isConst = false;
            if (type.rfind("const ", 0) == 0) {
                isConst = true;
                type = type.substr(6);
            }
            std::size_t stars = 0;
            while (!type.empty() && type.back() == '*') {
                type.pop_back();
                ++stars;
            }
            if (type.empty()) throw InputError(file, line, "签名类型格式非法：" + text);
            if (isConst && stars == 0) throw InputError(file, line, "const 只能修饰指针指向的类型：" + text);
            if (IsScalarType(type)) {
                if (type == "char" && stars == 0) throw InputError(file, line, "char 必须是指针（路径字符串）：" + text);
                return;
            }
            if (IsStructureType(type)) {
                if (stars == 0) throw InputError(file, line, "结构类型必须按指针传递：" + text);
                return;
            }
            throw InputError(file, line, "签名类型不在允许集合内：" + text);
        }

        // 校验一个完整 C 签名：<返回类型> (<参数列表>)，空参数必须写成 (void)
        void CheckSignature(const std::string& file, const std::string& signature, std::size_t line) {
            if (signature.empty()) throw InputError(file, line, "缺少 C 签名");
            if (signature.back() != ')') throw InputError(file, line, "签名必须以 ) 结尾");
            const std::size_t open = signature.rfind('(');
            if (open == std::string::npos || open < 2 || signature[open - 1] != ' ') throw InputError(file, line, "签名必须以 \"<返回类型> (\" 形式给出");
            const std::string returnType = signature.substr(0, open - 1);
            const std::string params = signature.substr(open + 1, signature.size() - open - 2);
            if (params.find('(') != std::string::npos || params.find(')') != std::string::npos) throw InputError(file, line, "签名参数列表不允许嵌套括号");
            CheckType(file, returnType, line);
            if (params == "void") return;
            if (params.empty()) throw InputError(file, line, "空参数列表必须写成 (void)");
            std::size_t begin = 0;
            while (true) {
                const std::size_t comma = params.find(',', begin);
                const std::size_t end = comma == std::string::npos ? params.size() : comma;
                const std::string item = params.substr(begin, end - begin);
                if (item.empty()) throw InputError(file, line, "参数列表含空项");
                if (comma != std::string::npos) {
                    if (end + 2 >= params.size()) throw InputError(file, line, "参数之间必须用 \", \" 分隔");
                    if (params[end + 1] != ' ' || params[end + 2] == ' ' || params[end + 2] == ',') throw InputError(file, line, "参数之间必须用单个 \", \" 分隔");
                }
                CheckType(file, item, line);
                if (comma == std::string::npos) break;
                begin = comma + 2;
            }
        }

        // 取下一个空格分隔字段：不允许空字段，缺字段按名称报错
        std::string TakeField(const std::string& file, const std::string& text, std::size_t& begin, std::size_t line, const char* what) {
            const std::size_t space = text.find(' ', begin);
            if (space == std::string::npos) throw InputError(file, line, std::string("缺少字段：") + what);
            const std::string field = text.substr(begin, space - begin);
            if (field.empty()) throw InputError(file, line, "字段之间必须用单个空格分隔");
            begin = space + 1;
            return field;
        }

        // 解析 Interfaces.def：逐行读入版本块，同时完成语法、排序与重复检查
        std::vector<InterfaceVersion> ParseInterfaces(const std::string& file, const std::string& text) {
            const std::vector<std::string> lines = SplitLines(file, text);
            std::vector<InterfaceVersion> blocks;
            std::map<std::string, std::set<uint16_t>> seenVersions;
            std::set<std::string> slotNames;
            std::string currentName;
            uint16_t currentVersion = 0;
            bool hasCurrent = false;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                const std::size_t line = index + 1;
                const std::string& row = lines[index];
                if (IsComment(row)) continue;
                CheckNoEdgeSpace(file, row, line);
                if (row.rfind("interface ", 0) != 0) throw InputError(file, line, "行必须以 \"interface \" 开头");
                const std::string rest = row.substr(10);
                std::size_t begin = 0;
                const std::string name = TakeField(file, rest, begin, line, "稳定名");
                const std::string versionText = TakeField(file, rest, begin, line, "版本");
                const std::string slotName = TakeField(file, rest, begin, line, "槽名");
                const std::string signature = rest.substr(begin);
                CheckInterfaceName(file, name, line);
                const uint16_t version = static_cast<uint16_t>(ParseDecimal(file, versionText, line, "接口版本", 1, 65534));
                CheckSlotName(file, slotName, line);
                CheckSignature(file, signature, line);
                if (hasCurrent && name == currentName && version == currentVersion) {
                    if (!slotNames.insert(slotName).second) throw InputError(file, line, "槽名在版本块内重复：" + slotName);
                    InterfaceVersion& block = blocks.back();
                    if (block.slots.size() >= kSlotMaxCount) throw InputError(file, line, "版本块的槽数超过 64");
                    block.slots.push_back(Slot{ static_cast<uint16_t>(block.slots.size()), slotName, signature, line });
                    continue;
                }
                auto& versions = seenVersions[name];
                if (versions.count(version) != 0) throw InputError(file, line, "重复的接口版本块（同一接口标识与版本）：" + name + " v" + versionText);
                if (!blocks.empty()) {
                    const InterfaceVersion& previous = blocks.back();
                    if (name < previous.name) throw InputError(file, line, "接口必须按稳定名 ASCII 升序排列：" + name + " 不能出现在 " + previous.name + " 之后");
                    if (name == previous.name && version < previous.version) throw InputError(file, line, "同一稳定名的版本必须升序排列");
                }
                versions.insert(version);
                blocks.push_back(InterfaceVersion{ name, version, line, {} });
                blocks.back().slots.push_back(Slot{ 0, slotName, signature, line });
                currentName = name;
                currentVersion = version;
                hasCurrent = true;
                slotNames.clear();
                slotNames.insert(slotName);
            }
            if (blocks.empty()) throw InputError(file, 0, "没有接口定义");
            return blocks;
        }

        // 解析 BootClosure.def：13 行闭包成员，顺序 1..13 连续升序、逻辑名唯一
        std::vector<ClosureMember> ParseClosure(const std::string& file, const std::string& text) {
            const std::vector<std::string> lines = SplitLines(file, text);
            std::vector<ClosureMember> members;
            std::set<std::string> names;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                const std::size_t line = index + 1;
                const std::string& row = lines[index];
                // 闭包表带说明块，注释行与空行都跳过
                if (row.empty() || IsComment(row)) continue;
                CheckNoEdgeSpace(file, row, line);
                if (row.rfind("closure ", 0) != 0) throw InputError(file, line, "行必须以 \"closure \" 开头");
                const std::string rest = row.substr(8);
                std::size_t begin = 0;
                const std::string orderText = TakeField(file, rest, begin, line, "顺序");
                const std::string name = rest.substr(begin);
                if (name.find(' ') != std::string::npos) throw InputError(file, line, "闭包行只能有顺序与逻辑名两个字段");
                const uint32_t order = ParseDecimal(file, orderText, line, "闭包顺序", 1, static_cast<uint32_t>(kClosureCount));
                if (order != members.size() + 1) throw InputError(file, line, "闭包顺序必须从 1 连续升序");
                CheckModuleName(file, name, line, "逻辑名");
                if (!names.insert(name).second) throw InputError(file, line, "闭包逻辑名重复：" + name);
                members.push_back(ClosureMember{ static_cast<uint16_t>(order), name, line });
            }
            if (members.size() != kClosureCount) throw InputError(file, 0, "闭包成员必须恰好 13 个，实际 " + std::to_string(members.size()) + " 个");
            return members;
        }

        // 判断是否为 32 位小写十六进制摘要
        bool IsLowerHexDigest(const std::string& text) {
            if (text.size() != kDigestHexChars) return false;
            for (const char c : text) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            return true;
        }

        // 解析冻结表：每行 4 个制表符分隔字段，注释行与空行跳过
        std::vector<FrozenEntry> ParseFrozen(const std::string& dir, bool& hasFrozen) {
            const std::string path = JoinPath(dir, kFrozenFile);
            std::error_code error;
            hasFrozen = std::filesystem::exists(path, error);
            if (error) throw InputError(kFrozenFile, 0, "无法确认冻结表是否存在");
            if (!hasFrozen) return {};
            const std::vector<std::string> lines = SplitLines(kFrozenFile, ReadText(dir, kFrozenFile));
            std::vector<FrozenEntry> entries;
            std::set<std::pair<std::string, uint16_t>> seen;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                const std::size_t line = index + 1;
                const std::string& row = lines[index];
                if (row.empty() || IsComment(row)) continue;
                std::vector<std::string> fields;
                std::size_t begin = 0;
                while (true) {
                    const std::size_t tab = row.find('\t', begin);
                    fields.push_back(row.substr(begin, tab == std::string::npos ? std::string::npos : tab - begin));
                    if (tab == std::string::npos) break;
                    begin = tab + 1;
                }
                if (fields.size() != 4) throw InputError(kFrozenFile, line, "冻结表每行必须是 4 个制表符分隔字段");
                CheckInterfaceName(kFrozenFile, fields[0], line);
                const uint16_t version = static_cast<uint16_t>(ParseDecimal(kFrozenFile, fields[1], line, "冻结版本", 1, 65534));
                const uint16_t slotCount = static_cast<uint16_t>(ParseDecimal(kFrozenFile, fields[2], line, "冻结槽数", static_cast<uint32_t>(kSlotMinCount), static_cast<uint32_t>(kSlotMaxCount)));
                if (!IsLowerHexDigest(fields[3])) throw InputError(kFrozenFile, line, "冻结摘要必须是 32 位小写十六进制");
                if (!seen.insert({ fields[0], version }).second) throw InputError(kFrozenFile, line, "冻结记录重复：" + fields[0] + " v" + fields[1]);
                entries.push_back(FrozenEntry{ fields[0], version, slotCount, fields[3], line });
            }
            return entries;
        }

        // 在版本块表里查找 (稳定名, 版本)
        const InterfaceVersion* FindVersionInBlocks(const std::vector<InterfaceVersion>& blocks, const std::string& name, uint16_t version) {
            for (const InterfaceVersion& block : blocks) if (block.name == name && block.version == version) return &block;
            return nullptr;
        }

        // 追加兼容：同接口的相邻版本，新版本前 N 槽必须与旧版本逐槽同名同签名
        // 破坏性变更不允许用版本表达；改既有槽必须新建接口稳定名
        void CheckAppendCompatibility(const std::vector<InterfaceVersion>& blocks) {
            std::map<std::string, std::vector<const InterfaceVersion*>> versionsByName;
            for (const InterfaceVersion& block : blocks) versionsByName[block.name].push_back(&block);
            for (const auto& item : versionsByName) {
                const std::vector<const InterfaceVersion*>& versions = item.second;
                for (std::size_t index = 1; index < versions.size(); ++index) {
                    const InterfaceVersion& previous = *versions[index - 1];
                    const InterfaceVersion& current = *versions[index];
                    const std::string where = current.name + " v" + std::to_string(current.version) + " 与 v" + std::to_string(previous.version);
                    if (current.slots.size() < previous.slots.size()) throw InputError(kInterfacesFile, current.line, "新版本槽数少于旧版本，只允许尾部追加：" + where);
                    for (std::size_t slot = 0; slot < previous.slots.size(); ++slot) {
                        const Slot& oldSlot = previous.slots[slot];
                        const Slot& newSlot = current.slots[slot];
                        if (newSlot.name != oldSlot.name) throw InputError(kInterfacesFile, newSlot.line, "追加兼容：槽 " + std::to_string(slot) + " 的槽名必须与旧版本相同（" + oldSlot.name + "）：" + where);
                        if (newSlot.signature != oldSlot.signature) throw InputError(kInterfacesFile, newSlot.line, "追加兼容：槽 " + std::to_string(slot) + " 的签名必须与旧版本相同（" + oldSlot.signature + "）：" + where);
                    }
                }
            }
        }

        // 构建期硬检查：必需接口存在、Core.Abi 槽位签名固定
        void CheckProviderTables(const std::vector<InterfaceVersion>& blocks) {
            for (const char* name : kRequiredCoreInterfaces) {
                if (FindVersionInBlocks(blocks, name, 1) == nullptr) throw InputError(kInterfacesFile, 0, std::string("内核核心服务接口必须定义 v1：") + name);
            }
            for (const SingletonProvider& provider : kSingletonProviders) {
                if (FindVersionInBlocks(blocks, provider.interfaceName, 1) == nullptr) throw InputError(kInterfacesFile, 0, std::string("闭包单例接口必须定义 v1：") + provider.interfaceName);
            }
            const InterfaceVersion* abi = FindVersionInBlocks(blocks, "Core.Abi", 1);
            for (const AbiSlotBinding& binding : kCoreAbiBindings) {
                if (binding.number >= abi->slots.size()) throw InputError(kInterfacesFile, abi->line, std::string("Core.Abi v1 缺少槽 ") + std::to_string(binding.number));
                const Slot& slot = abi->slots[binding.number];
                if (slot.name != binding.name) throw InputError(kInterfacesFile, slot.line, std::string("Core.Abi v1 槽 ") + std::to_string(binding.number) + " 的槽名必须是 " + binding.name);
                if (slot.signature != binding.signature) throw InputError(kInterfacesFile, slot.line, std::string("Core.Abi v1 槽 ") + std::to_string(binding.number) + " 的签名固定为 " + binding.signature);
            }
        }

        // 冻结比对：冻结表里的每个已发布版本都必须与当前定义一致
        void CheckFrozen(const std::vector<InterfaceVersion>& blocks, const std::vector<FrozenEntry>& frozen) {
            for (const FrozenEntry& entry : frozen) {
                const InterfaceVersion* block = FindVersionInBlocks(blocks, entry.name, entry.version);
                const std::string where = entry.name + " v" + std::to_string(entry.version);
                if (block == nullptr) throw InputError(kFrozenFile, entry.line, "已发布版本不在当前 Interfaces.def 中：" + where);
                if (block->slots.size() != entry.slotCount) throw InputError(kFrozenFile, entry.line, "槽数与冻结记录不符：" + where + "，冻结 " + std::to_string(entry.slotCount) + "，当前 " + std::to_string(block->slots.size()));
                if (SlotDigest(*block) != entry.digest) throw InputError(kFrozenFile, entry.line, "槽位定义与冻结记录不符：" + where);
            }
        }

        // 计算一段字节的 SHA-256
        std::array<uint8_t, hostbuild::kSha256Bytes> Sha256Of(const std::string& text) {
            hostbuild::Sha256 hash;
            hash.Begin();
            hash.Update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
            return hash.Finish();
        }

        // 把字节写成小写十六进制文本
        std::string HexText(const uint8_t* bytes, std::size_t count) {
            const char digits[] = "0123456789abcdef";
            std::string text;
            text.reserve(count * 2);
            for (std::size_t index = 0; index < count; ++index) {
                text.push_back(digits[bytes[index] >> 4]);
                text.push_back(digits[bytes[index] & 0x0f]);
            }
            return text;
        }

        // 派生标识：域标记与名字 ASCII 拼接后取 SHA-256 的前 16 字节
        IdBytes DeriveId(const char* domain, const std::string& name) {
            const std::array<uint8_t, hostbuild::kSha256Bytes> digest = Sha256Of(std::string(domain) + name);
            IdBytes id;
            std::copy(digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(kIdBytes), id.bytes.begin());
            return id;
        }
    }

    InputError::InputError(const std::string& fileText, std::size_t lineNumber, const std::string& message) : std::runtime_error(message), file(fileText), line(lineNumber) {}

    Contract Load(const std::string& contractDir) {
        Contract result;
        result.interfaces = ParseInterfaces(kInterfacesFile, ReadText(contractDir, kInterfacesFile));
        CheckAppendCompatibility(result.interfaces);
        // 接口标识与生成常量名都必须唯一：不同稳定名的标识碰撞与常量名折叠都直接拒绝
        std::map<std::string, std::string> interfaceIds;
        std::map<std::string, std::string> constantNames;
        std::vector<std::string> names;
        for (const InterfaceVersion& block : result.interfaces) {
            if (std::find(names.begin(), names.end(), block.name) != names.end()) continue;
            names.push_back(block.name);
            const std::string idText = IdText(DeriveInterfaceId(block.name));
            const auto idInserted = interfaceIds.emplace(idText, block.name);
            if (!idInserted.second) throw InputError(kInterfacesFile, block.line, "接口标识冲突：" + block.name + " 与 " + idInserted.first->second);
            const std::string constantName = InterfaceConstantName(block.name);
            const auto nameInserted = constantNames.emplace(constantName, block.name);
            if (!nameInserted.second) throw InputError(kInterfacesFile, block.line, "接口常量名冲突：" + block.name + " 与 " + nameInserted.first->second);
        }
        CheckProviderTables(result.interfaces);
        result.closure = ParseClosure(kBootClosureFile, ReadText(contractDir, kBootClosureFile));
        std::map<std::string, std::string> moduleIds;
        for (const ClosureMember& member : result.closure) {
            const std::string idText = IdText(DeriveModuleId(member.name));
            const auto inserted = moduleIds.emplace(idText, member.name);
            if (!inserted.second) throw InputError(kBootClosureFile, member.line, "模块标识冲突：" + member.name + " 与 " + inserted.first->second);
        }
        result.frozen = ParseFrozen(contractDir, result.hasFrozen);
        if (result.hasFrozen) CheckFrozen(result.interfaces, result.frozen);
        return result;
    }

    IdBytes DeriveInterfaceId(const std::string& name) { return DeriveId("LPIF01", name); }

    IdBytes DeriveModuleId(const std::string& name) { return DeriveId("LPMOD01", name); }

    std::string IdText(const IdBytes& id) { return HexText(id.bytes.data(), id.bytes.size()); }

    std::string SlotDigest(const InterfaceVersion& interface) {
        std::string normalized;
        for (const Slot& slot : interface.slots) {
            normalized += std::to_string(slot.number);
            normalized += '\t';
            normalized += slot.name;
            normalized += '\t';
            normalized += slot.signature;
            normalized += '\n';
        }
        // 每槽一行以 LF 连接、末尾不带多余换行
        if (!normalized.empty()) normalized.pop_back();
        const std::array<uint8_t, hostbuild::kSha256Bytes> digest = Sha256Of(normalized);
        return HexText(digest.data(), 16);
    }

    std::string PascalName(const std::string& name) {
        std::string result;
        result.reserve(name.size());
        for (const char c : name) if (c != '.') result.push_back(c);
        return result;
    }

    std::string InterfaceConstantName(const std::string& name) { return "kIf" + PascalName(name); }

    std::string ModuleConstantName(const std::string& name) { return "kMod" + PascalName(name); }

    const InterfaceVersion* FindVersion(const Contract& contract, const std::string& name, uint16_t version) { return FindVersionInBlocks(contract.interfaces, name, version); }

    const InterfaceVersion* HighestVersion(const Contract& contract, const std::string& name) {
        const InterfaceVersion* highest = nullptr;
        for (const InterfaceVersion& block : contract.interfaces) {
            if (block.name != name) continue;
            if (highest == nullptr || block.version > highest->version) highest = &block;
        }
        return highest;
    }

    ModuleContractResult ValidateModuleContract(const Contract& contract, const std::string& logicalName, const std::vector<ModuleExportDecl>& exports, const std::vector<ModuleImportDecl>& imports) {
        const std::string file = "Module.mk";
        CheckModuleName(file, logicalName, 0, "模块逻辑名");
        ModuleContractResult result;
        // 导出：接口与版本必须存在、槽数由版本块推出；Core.* 与闭包单例的提供者固定
        std::set<std::pair<std::string, uint16_t>> exported;
        for (const ModuleExportDecl& declaration : exports) {
            CheckInterfaceName(file, declaration.name, 0);
            if (declaration.name.rfind("Core.", 0) == 0) throw InputError(file, 0, "Core.* 由内核核心唯一提供，模块不得导出：" + declaration.name);
            for (const SingletonProvider& provider : kSingletonProviders) {
                if (declaration.name != provider.interfaceName) continue;
                if (logicalName != provider.moduleName) throw InputError(file, 0, std::string("闭包单例的提供者必须是 ") + provider.moduleName + "，本模块是 " + logicalName + "：" + declaration.name);
            }
            const InterfaceVersion* block = FindVersion(contract, declaration.name, declaration.version);
            if (block == nullptr) {
                if (HighestVersion(contract, declaration.name) == nullptr) throw InputError(file, 0, "接口不在 Interfaces.def 中：" + declaration.name);
                throw InputError(file, 0, "接口版本不在 Interfaces.def 中：" + declaration.name + " v" + std::to_string(declaration.version));
            }
            if (!exported.insert({ declaration.name, declaration.version }).second) throw InputError(file, 0, "导出项重复：" + declaration.name + " v" + std::to_string(declaration.version));
            result.exportArgs.push_back("--export " + declaration.name + ":" + std::to_string(declaration.version) + ":" + std::to_string(block->slots.size()));
        }
        // 导入：区间合法、接口与区间内版本存在、使用槽数不超过区间内最高版本的槽数、同一接口只有一条导入
        std::set<std::string> imported;
        for (const ModuleImportDecl& declaration : imports) {
            CheckInterfaceName(file, declaration.name, 0);
            if (declaration.minVersion == 0 || declaration.minVersion > declaration.maxVersion) throw InputError(file, 0, "导入版本区间非法：" + declaration.name);
            if (declaration.slotCount < kSlotMinCount || declaration.slotCount > kSlotMaxCount) throw InputError(file, 0, "导入使用槽数必须在 1..64：" + declaration.name);
            if (!imported.insert(declaration.name).second) throw InputError(file, 0, "同一接口只能有一条导入：" + declaration.name);
            const InterfaceVersion* best = nullptr;
            bool interfaceExists = false;
            for (const InterfaceVersion& block : contract.interfaces) {
                if (block.name != declaration.name) continue;
                interfaceExists = true;
                if (block.version < declaration.minVersion || block.version > declaration.maxVersion) continue;
                if (best == nullptr || block.version > best->version) best = &block;
            }
            if (!interfaceExists) throw InputError(file, 0, "接口不在 Interfaces.def 中：" + declaration.name);
            if (best == nullptr) throw InputError(file, 0, "导入区间内没有已定义版本：" + declaration.name);
            if (declaration.slotCount > best->slots.size()) throw InputError(file, 0, "使用槽数超过区间内最高版本的槽数：" + declaration.name);
            result.importArgs.push_back("--import " + declaration.name + ":" + std::to_string(declaration.minVersion) + ":" + std::to_string(declaration.maxVersion) + ":" + std::to_string(declaration.slotCount));
        }
        return result;
    }
}
