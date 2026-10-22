/* Contract.h
    契约输入模型：Interfaces.def、BootClosure.def 与冻结表的解析、校验与标识派生

    解析规则与字段语义见 Docs/Specs/模块契约层.md 第二、三、四节；本头只声明模型与
    校验入口，所有格式判定在 Contract.cpp 内完成，失败抛 InputError 并带文件名与行号
    MakeContract 的生成侧不重复校验：Generate 只接受已通过 Load 的输入
*/

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace contract {
    // 标识原始字节长度：InterfaceId 与 ModuleId 都是 SHA-256 的前 16 字节
    constexpr std::size_t kIdBytes = 16;

    // 派生出的 16 字节标识
    struct IdBytes {
        std::array<uint8_t, kIdBytes> bytes{};    // 原始标识字节
    };

    // 一个槽位：槽号、槽名与 C 签名
    struct Slot {
        uint16_t number = 0;        // 槽号，从 0 起
        std::string name;           // 槽名，生成结构体的成员名
        std::string signature;      // C 签名，如 "uint64_t (uint32_t)"
        std::size_t line = 0;       // 槽行行号，用于错误定位
    };

    // 一个接口版本块：稳定名、版本与槽位表
    struct InterfaceVersion {
        std::string name;              // 稳定名，如 Core.Page
        uint16_t version = 0;          // 接口版本，取值 1..65534
        std::size_t line = 0;          // 版本块首行的行号
        std::vector<Slot> slots;       // 槽位，文件顺序即槽位顺序
    };

    // 一个引导闭包成员：预装顺序与逻辑名
    struct ClosureMember {
        uint16_t order = 0;    // 预装顺序，1..13 连续升序
        std::string name;      // 逻辑名，如 DevModel
        std::size_t line = 0;  // 行号
    };

    // 一条冻结记录：稳定名、版本、槽数与槽位定义摘要
    struct FrozenEntry {
        std::string name;          // 稳定名
        uint16_t version = 0;      // 接口版本
        uint16_t slotCount = 0;    // 冻结的槽数
        std::string digest;        // 槽位定义摘要，32 位小写十六进制
        std::size_t line = 0;      // 冻结表行号
    };

    // 契约输入：接口版本块、闭包成员与可选的冻结记录
    struct Contract {
        std::vector<InterfaceVersion> interfaces;    // 接口版本块，按文件顺序
        std::vector<ClosureMember> closure;          // 闭包成员，按文件顺序
        std::vector<FrozenEntry> frozen;             // 冻结记录，按文件顺序
        bool hasFrozen = false;                      // 冻结表是否给出
    };

    // 输入错误：文件名基本名、行号（0 表示文件级）与原因
    struct InputError final : std::runtime_error {
        std::string file;     // 出错的输入文件名，不含目录
        std::size_t line;     // 出错行号，0 表示文件级
        // 构造一条带位置的输入错误
        InputError(const std::string& fileText, std::size_t lineNumber, const std::string& message);
    };

    // 闭包单例的固定提供者：接口稳定名与提供者逻辑名
    struct SingletonProvider {
        const char* interfaceName;    // 接口稳定名
        const char* moduleName;       // 唯一提供者的逻辑名
    };

    // 规格 4.3 固定的闭包单例提供者，按接口稳定名升序（M6 增补 Usb.Host → UsbCore）
    inline constexpr SingletonProvider kSingletonProviders[] = {
        { "Block.Registry", "Vfs" },
        { "Boot.Volume", "Vfs" },
        { "Dev.Model", "DevModel" },
        { "Drv.Loader", "DriverLoader" },
        { "Fs.Vfs", "Vfs" },
        { "Usb.Host", "UsbCore" },
    };

    // Core.Abi 版本块的固定槽位：槽号、槽名与签名
    struct AbiSlotBinding {
        uint16_t number;        // 槽号
        const char* name;       // 槽名
        const char* signature;  // 固定 C 签名
    };

    // 规格 5.2 与 5.3 固定的 Core.Abi 三个槽位
    inline constexpr AbiSlotBinding kCoreAbiBindings[] = {
        { 0, "Version", "uint16_t (void)" },
        { 1, "BuildId64", "uint64_t (void)" },
        { 2, "DriveSdkVersion", "uint16_t (void)" },
    };

    // 规格 4.3 固定的 10 个内核核心服务接口，build 期硬检查它们都有 v1
    inline constexpr const char* kRequiredCoreInterfaces[] = {
        "Core.Abi", "Core.Boot", "Core.Dma", "Core.Log", "Core.Mem",
        "Core.Mmio", "Core.Object", "Core.Page", "Core.PhysMap", "Core.Time",
    };

    // 读 Interfaces.def 与 BootClosure.def，Frozen.tsv 存在时一并读入并比对，失败抛 InputError
    Contract Load(const std::string& contractDir);

    // 派生 InterfaceId：SHA-256("LPIF01" || 稳定名 ASCII) 的前 16 字节
    IdBytes DeriveInterfaceId(const std::string& name);

    // 派生 ModuleId：SHA-256("LPMOD01" || 逻辑名 ASCII) 的前 16 字节
    IdBytes DeriveModuleId(const std::string& name);

    // 把 16 字节标识写成 32 位小写十六进制文本
    std::string IdText(const IdBytes& id);

    // 槽位定义规范化摘要：每槽 "<槽号>\t<槽名>\t<签名>" 以 LF 连接后取 SHA-256 前 16 字节
    std::string SlotDigest(const InterfaceVersion& interface);

    // 稳定名或逻辑名派生 C++ 标识：去掉 '.' 后各分量拼接，如 Core.Page -> CorePage
    std::string PascalName(const std::string& name);

    // 接口标识常量名：稳定名派生并加 kIf 前缀，如 Core.Page -> kIfCorePage
    std::string InterfaceConstantName(const std::string& name);

    // 模块标识常量名：逻辑名派生并加 kMod 前缀，如 DevModel -> kModDevModel
    std::string ModuleConstantName(const std::string& name);

    // 接口版本块查找：找不到返回空指针
    const InterfaceVersion* FindVersion(const Contract& contract, const std::string& name, uint16_t version);

    // 接口的最高版本块：接口不存在时返回空指针
    const InterfaceVersion* HighestVersion(const Contract& contract, const std::string& name);

    // 模块契约声明的一条导出：稳定名与版本，槽数由接口定义推出（模块配置不写槽数）
    struct ModuleExportDecl {
        std::string name;      // 稳定名，如 Dev.Model
        uint16_t version = 0;  // 接口版本
    };

    // 模块契约声明的一条导入：稳定名、版本区间与使用槽数
    struct ModuleImportDecl {
        std::string name;          // 稳定名，如 Core.Log
        uint16_t minVersion = 0;   // 区间下限
        uint16_t maxVersion = 0;   // 区间上限，65535 表示不设上限
        uint16_t slotCount = 0;    // 使用槽数
    };

    // 模块契约校验结果：给 PackModule 的 --export/--import 参数行，按声明顺序
    struct ModuleContractResult {
        std::vector<std::string> exportArgs;  // "--export <稳定名>:<版本>:<槽数>"
        std::vector<std::string> importArgs;  // "--import <稳定名>:<Min>:<Max>:<槽数>"
    };

    // 校验模块契约声明并给出 PackModule 参数：失败抛 InputError（file 为 "Module.mk"）
    // 校验项按 Docs/Specs/模块契约层.md 2.4 / 4.3 / 4.5：接口与版本存在、槽数由定义推出、
    // Core.* 不得由模块导出、闭包单例提供者与固定表一致、导入区间与使用槽数合法
    ModuleContractResult ValidateModuleContract(const Contract& contract, const std::string& logicalName, const std::vector<ModuleExportDecl>& exports, const std::vector<ModuleImportDecl>& imports);
}
