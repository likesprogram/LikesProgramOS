/* Generate.cpp
    生成物实现：把契约输入渲染成 10 份契约头文件，生成顺序与文本模板固定

    每个生成头的文件头块注释、结构成员与常量都写入职责说明；槽位语义只写一句提示，
    不在生成器里重复一份自然语言定义
    文本只由输入与固定模板决定：不写时间戳、构建路径、用户名与随机顺序
*/

#include "Generate.h"

#include <algorithm>
#include <string>
#include <vector>

namespace contract {
    namespace {
        // 内核 ABI 版本当前值：生成的契约头从这里取值，抬版只改这一处
        constexpr uint16_t kKernelAbiVersionValue = 1;
        // 驱动契约版本当前值：生成的契约头从这里取值，抬版只改这一处
        constexpr uint16_t kDriveSdkVersionValue = 1;

        // 标识字节数
        constexpr std::size_t kIdByteCount = 16;

        // 生成文本的行追加器：统一 LF
        class Builder {
        public:
            // 追加一行文本
            void Line(const std::string& text) {
                m_text += text;
                m_text += '\n';
            }
            // 追加一个空行
            void Blank() { m_text += '\n'; }
            // 直接追加一段文本，不补换行
            void Raw(const std::string& text) { m_text += text; }
            // 取累积文本
            const std::string& Text() const { return m_text; }
        private:
            std::string m_text;    // 累积的文本
        };

        // 文件头块注释：文件名、职责与说明行
        void Header(Builder& builder, const char* fileName, const char* duty, const std::vector<std::string>& notes) {
            builder.Line(std::string("/* ") + fileName);
            builder.Line(std::string("    ") + duty);
            for (const std::string& note : notes) builder.Line(note.empty() ? "" : "    " + note);
            builder.Line("*/");
            builder.Blank();
        }

        // 按稳定名去重后的接口名，保持文件顺序（ASCII 升序）
        std::vector<std::string> UniqueInterfaceNames(const Contract& contract) {
            std::vector<std::string> names;
            for (const InterfaceVersion& block : contract.interfaces) {
                if (names.empty() || names.back() != block.name) names.push_back(block.name);
            }
            return names;
        }

        // 接口的最低版本块：接口必须存在，调用前已由 Load 校验
        const InterfaceVersion& LowestVersion(const Contract& contract, const std::string& name) {
            const InterfaceVersion* lowest = nullptr;
            for (const InterfaceVersion& block : contract.interfaces) {
                if (block.name != name) continue;
                if (lowest == nullptr || block.version < lowest->version) lowest = &block;
            }
            return *lowest;
        }

        // 接口最高版本块的引用：输入已通过校验，接口必然存在
        const InterfaceVersion& HighestBlock(const Contract& contract, const char* name) {
            return *HighestVersion(contract, name);
        }

        // 判断接口是否由内核核心提供
        bool IsKernelCoreInterface(const std::string& name) { return name.rfind("Core.", 0) == 0; }

        // 槽结构名：Core.Page v1 -> CorePageSlotsV1
        std::string SlotStructName(const std::string& name, uint16_t version) { return PascalName(name) + "SlotsV" + std::to_string(version); }

        // 槽数常量名：Core.Page v1 -> kCorePageSlotCountV1
        std::string SlotCountName(const std::string& name, uint16_t version) { return "k" + PascalName(name) + "SlotCountV" + std::to_string(version); }

        // 当前版本常量名：Core.Page -> kCorePageCurrentVersion
        std::string CurrentVersionName(const std::string& name) { return "k" + PascalName(name) + "CurrentVersion"; }

        // 导入区间下限常量名：Core.Page -> kCorePageMinVersion
        std::string MinVersionName(const std::string& name) { return "k" + PascalName(name) + "MinVersion"; }

        // 导入区间上限常量名：Core.Page -> kCorePageMaxVersion
        std::string MaxVersionName(const std::string& name) { return "k" + PascalName(name) + "MaxVersion"; }

        // 当前槽数常量名：Core.Page -> kCorePageSlotCount
        std::string SlotCountSummaryName(const std::string& name) { return "k" + PascalName(name) + "SlotCount"; }

        // 槽表 C 符号里的接口名：稳定名的 '.' 删除（Core.Log -> CoreLog）
        // 与 PackModule 的 StripDots 同一条规则：_Contract_Export_<稳定名去点>_v<版本>
        std::string SymbolInterfaceName(const std::string& name) {
            std::string result;
            result.reserve(name.size());
            for (const char c : name) if (c != '.') result.push_back(c);
            return result;
        }

        // 提供者导出表符号名：Core.Page v1 -> _Contract_Export_Core_Page_v1
        std::string ExportSymbolName(const std::string& name, uint16_t version) { return "_Contract_Export_" + SymbolInterfaceName(name) + "_v" + std::to_string(version); }

        // 消费者回填区符号名：Core.Page v1 -> _Contract_Import_Core_Page_v1
        std::string ImportSymbolName(const std::string& name, uint16_t version) { return "_Contract_Import_" + SymbolInterfaceName(name) + "_v" + std::to_string(version); }

        // 16 字节标识的聚合初始化文本，每行 8 字节
        std::string IdInitializer(const IdBytes& id) {
            const char digits[] = "0123456789abcdef";
            std::string text = "{ {\n";
            for (std::size_t index = 0; index < id.bytes.size(); ++index) {
                if (index % 8 == 0) text += "        ";
                text += "0x";
                text.push_back(digits[id.bytes[index] >> 4]);
                text.push_back(digits[id.bytes[index] & 0x0f]);
                const bool lineEnd = (index + 1) % 8 == 0;
                if (index + 1 != id.bytes.size()) {
                    text += ",";
                    if (!lineEnd) text += " ";
                }
                if (lineEnd) text += "\n";
            }
            text += "    } }";
            return text;
        }

        // 把槽位 C 签名插入槽名，生成函数指针成员声明
        std::string SlotMember(const std::string& signature, const std::string& slotName) {
            const std::size_t open = signature.rfind('(');
            return signature.substr(0, open - 1) + " (*" + slotName + ")" + signature.substr(open);
        }

        // 生成一个版本的槽位结构：成员按槽序，槽数与布局用静态断言钉死
        void SlotStruct(Builder& builder, const InterfaceVersion& block) {
            const std::string structName = SlotStructName(block.name, block.version);
            std::vector<std::string> members;
            std::size_t width = 0;
            for (const Slot& slot : block.slots) {
                const std::string member = "    " + SlotMember(slot.signature, slot.name) + ";";
                members.push_back(member);
                width = std::max(width, member.size());
            }
            builder.Line("struct " + structName + " {");
            for (std::size_t index = 0; index < members.size(); ++index) {
                const std::string padding(width + 2 - members[index].size(), ' ');
                builder.Line(members[index] + padding + "// 槽 " + std::to_string(index));
            }
            builder.Line("};");
            builder.Line("static_assert(sizeof(" + structName + ") == " + std::to_string(block.slots.size()) + " * sizeof(void*), \"" + structName + " 的槽数与布局必须为 " + std::to_string(block.slots.size()) + " 个函数指针\");");
        }

        // Types.hpp 的固定主体：基础类型、映射与契约结构
        constexpr const char kTypesBody[] = R"CONTRACT(// 两个类型是否相同：只对同一类型给出 true，用于 ABI 绑定断言
template <typename Left, typename Right>
struct SameContractType {
    // 左右类型不同
    static constexpr bool kValue = false;
};

template <typename Left>
struct SameContractType<Left, Left> {
    // 左右类型相同
    static constexpr bool kValue = true;
};

// 模块入口与 Core.* 槽位的返回码：u32 底层，0 表示成功，编号发布后只增不改
enum class KernelError : uint32_t {
    kOk = 0,                // 成功
    kNotSupported = 1,      // 当前版本或实现不支持该操作，调用方不得重试同一调用
    kInvalidArgument = 2,   // 参数非法：空指针、零长度、未对齐、越界、取值不在允许集合
    kOutOfResources = 3,    // 资源不足：页帧、DMA、对象表、句柄表
    kBusy = 4,              // 对象已被占用或操作进行中，调用方可稍后重试
    kIo = 5,                // 底层 I/O 失败
    kCorrupt = 6,           // 结构非法：魔数、校验、范围或环路错误
    kNotFound = 7,          // 对象、设备、路径或版本不存在
    kPermission = 8,        // 设备或卷状态不允许该操作
    kNoSpace = 9,           // 空间不足
    kTimeout = 10,          // 有界等待超时
    kAlreadyExists = 11,    // 登记或绑定已存在
    kVersionMismatch = 12,  // 版本不匹配
    kState = 13,            // 对象状态不允许该操作
};

// 错误码的稳定短名，供诊断 detail 与模块日志使用；名字文本不是编号
constexpr const char* KernelErrorName(KernelError error) {
    switch (error) {
        case KernelError::kOk: return "kOk";
        case KernelError::kNotSupported: return "kNotSupported";
        case KernelError::kInvalidArgument: return "kInvalidArgument";
        case KernelError::kOutOfResources: return "kOutOfResources";
        case KernelError::kBusy: return "kBusy";
        case KernelError::kIo: return "kIo";
        case KernelError::kCorrupt: return "kCorrupt";
        case KernelError::kNotFound: return "kNotFound";
        case KernelError::kPermission: return "kPermission";
        case KernelError::kNoSpace: return "kNoSpace";
        case KernelError::kTimeout: return "kTimeout";
        case KernelError::kAlreadyExists: return "kAlreadyExists";
        case KernelError::kVersionMismatch: return "kVersionMismatch";
        case KernelError::kState: return "kState";
    }
    return "unknown";
}

// 存储面失败归一为模块返回码的固定映射，两套稳定编号不混用
constexpr KernelError KernelErrorFromStorage(Storage::Error error) {
    switch (error) {
        case Storage::Error::Ok: return KernelError::kOk;
        case Storage::Error::NotFound: return KernelError::kNotFound;
        case Storage::Error::NotDirectory: return KernelError::kInvalidArgument;
        case Storage::Error::IsDirectory: return KernelError::kInvalidArgument;
        case Storage::Error::NotSupported: return KernelError::kNotSupported;
        case Storage::Error::Corrupt: return KernelError::kCorrupt;
        case Storage::Error::Io: return KernelError::kIo;
        case Storage::Error::ReadOnly: return KernelError::kPermission;
        case Storage::Error::NoSpace: return KernelError::kNoSpace;
        case Storage::Error::NameTooLong: return KernelError::kInvalidArgument;
        case Storage::Error::InvalidArgument: return KernelError::kInvalidArgument;
    }
    return KernelError::kInvalidArgument;
}

// 块设备与子设备的句柄：0 表示无效
using DeviceHandle = uint32_t;

// 设备认领句柄：0 表示无效
using ClaimHandle = uint32_t;

// 设备绑定句柄：0 表示无效
using BindHandle = uint32_t;

// 文件系统登记的标识：0 表示无效
using FileSystemId = uint32_t;

// 对象登记的句柄：0 表示无效
using ObjectHandle = uint32_t;

// 文件引用：VFS 自有的不透明值，不是句柄表项
using FileReference = uint64_t;

// 块设备能力位：设备有真实刷新原语，对应 Storage::BlockDevice::HasFlushPrimitive
inline constexpr uint32_t kBlockCapabilityFlushPrimitive = 1;

// 块设备实现调用表 v1：首字段是能力位而不是函数指针，所有回调第一个参数是登记时的 context
struct BlockDeviceOps {
    uint32_t capabilities;                                                               // 能力位：bit0 为 kBlockCapabilityFlushPrimitive
    uint32_t (*BlockBytes)(void* context);                                               // 逻辑块大小，字节，必须是 2 的幂
    uint64_t (*CapacityBytes)(void* context);                                            // 设备容量，字节
    uint32_t (*Writable)(void* context);                                                 // 是否可写：0 只读，非 0 可写
    Storage::Error (*Read)(void* context, uint64_t offset, void* buffer, uint32_t bytes); // 读 [offset, offset+bytes) 到调用方缓冲
    Storage::Error (*Write)(void* context, uint64_t offset, const void* buffer, uint32_t bytes);  // 写，只读设备返回 ReadOnly
    Storage::Error (*Flush)(void* context);                                              // 刷新写缓存，无缓存返回 Ok
};
static_assert(sizeof(BlockDeviceOps) == 56, "BlockDeviceOps v1 必须是能力位加 6 个函数指针");

// 文件系统实现调用表 v1：布局与 BlockDeviceOps 同受 DriveSDKVersion 约束
struct FileSystemOps {
    const char* (*Format)(void* context);                                                  // 格式名，诊断用，如 "ext4"
    Storage::Error (*Mount)(void* context, const BlockDeviceOps* device, void* deviceContext, uint32_t mode);  // 识别并挂载设备
    void (*Unmount)(void* context);                                                        // 卸载并释放对设备的引用
    void (*Caps)(void* context, Storage::FileSystemCaps* caps);                            // 回报实现能力位
    Storage::Error (*VolumeIdentity)(void* context, uint8_t* volumeId);                    // 写 16 字节卷标识，挂载成功后有效
    Storage::Error (*Stat)(void* context, const char* path, Storage::FileInfo* info);      // 按卷内绝对路径查询事实
    Storage::Error (*Open)(void* context, const char* path, uint64_t* object);             // 按路径解析对象标识
    Storage::Error (*Read)(void* context, uint64_t object, uint64_t offset, void* buffer, uint32_t bytes, uint32_t* read);  // 从偏移读至多 bytes
    Storage::Error (*ReadDirectory)(void* context, uint64_t object, uint64_t* cursor, char* name, uint32_t nameBytes, uint32_t* nameLength, Storage::FileInfo* info);  // 游标式目录遍历
};
static_assert(sizeof(FileSystemOps) == 72, "FileSystemOps v1 必须是 9 个函数指针");

// 驱动描述 v1：驱动在 Init 里构造并登记，字节表固定 64 字节
struct DriverDesc {
    uint32_t bytes;                          // 0x00 结构长度，v1 为 0x40
    uint16_t driveSdkVersion;                // 0x04 驱动实现的 DriveSDK 版本
    uint16_t opsKind;                        // 0x06 能力类别：1 表示 BlockDeviceOps
    uint32_t busKind;                        // 0x08 匹配的总线类别，0 通配
    uint32_t classCode;                      // 0x0C 匹配的设备类码，0xFFFFFFFF 通配
    uint32_t vendorId;                       // 0x10 匹配的厂商 ID，0xFFFF 通配
    uint32_t deviceId;                       // 0x14 匹配的设备 ID，0xFFFFFFFF 通配
    uint64_t flags;                          // 0x18 保留，当前必须为 0
    Storage::Error (*start)(void* context);  // 0x20 启用回调，由 Drv.Loader.Start 调用
    void (*stop)(void* context);             // 0x28 停用回调，由 Drv.Loader.Stop 调用
    const void* ops;                         // 0x30 ops 表指针，v1 指向 BlockDeviceOps
    void* context;                           // 0x38 驱动实例上下文，回调原样传回
};
static_assert(sizeof(DriverDesc) == 0x40, "DriverDesc v1 必须是 64 字节");
static_assert(offsetof(DriverDesc, start) == 0x20, "DriverDesc.start 偏移固定为 0x20");
static_assert(offsetof(DriverDesc, stop) == 0x28, "DriverDesc.stop 偏移固定为 0x28");
static_assert(offsetof(DriverDesc, ops) == 0x30, "DriverDesc.ops 偏移固定为 0x30");
static_assert(offsetof(DriverDesc, context) == 0x38, "DriverDesc.context 偏移固定为 0x38");

// 设备事实 v1：可扩展，bytes 之后字段只追加，总线地址按设备模型事实编码
struct DeviceFacts {
    uint32_t bytes;             // 结构长度
    DeviceHandle deviceHandle;  // 设备句柄
    uint32_t busKind;           // 总线类别
    uint32_t classCode;         // 设备类码
    uint32_t vendorId;          // 厂商 ID
    uint32_t deviceId;          // 设备 ID
    uint64_t busAddress;        // 总线地址事实，编码按设备模型事实给出
};
static_assert(sizeof(DeviceFacts) == 32, "DeviceFacts v1 必须是 32 字节");

// 设备 BAR 表项 v1
struct DeviceBar {
    uint32_t index;  // BAR 索引
    uint32_t flags;  // 标志位
    uint64_t base;   // 基址
    uint64_t bytes;  // 长度
};
static_assert(sizeof(DeviceBar) == 24, "DeviceBar v1 必须是 24 字节");

// 设备 IRQ 表项 v1
struct DeviceIrq {
    uint32_t irq;    // 中断号
    uint32_t flags;  // 标志位
};
static_assert(sizeof(DeviceIrq) == 8, "DeviceIrq v1 必须是 8 字节");

// 设备 BAR 表容量：v1 固定
inline constexpr uint32_t kDeviceMaxBars = 6;

// 设备 IRQ 表容量：v1 固定
inline constexpr uint32_t kDeviceMaxIrqs = 4;

// 设备资源事实 v1：可扩展，BAR 表与 IRQ 表容量固定
struct DeviceResources {
    uint32_t bytes;                     // 结构长度
    uint32_t barCount;                  // BAR 表实际条目数
    DeviceBar bars[kDeviceMaxBars];     // BAR 表
    uint32_t irqCount;                  // IRQ 表实际条目数
    DeviceIrq irqs[kDeviceMaxIrqs];     // IRQ 表
    uint32_t dmaAddressBits;            // DMA 地址位宽约束
};
static_assert(sizeof(DeviceResources) == 192, "DeviceResources v1 必须是 192 字节");

// 设备绑定事实 v1：可扩展，Start 成功后 ops 与 context 才有效
struct BindingFacts {
    uint32_t bytes;               // 结构长度
    BindHandle bindHandle;        // 绑定句柄
    DeviceHandle deviceHandle;    // 设备句柄
    ModuleId driverModule;        // 驱动模块标识
    uint32_t state;               // 状态位
    const BlockDeviceOps* ops;    // 设备调用表
    void* context;                // 驱动实例上下文
};
static_assert(sizeof(BindingFacts) == 48, "BindingFacts v1 必须是 48 字节");

// 系统卷事实 v1：挂载成功后用于诊断与归属记录
struct SystemVolumeFacts {
    uint32_t bytes;            // 结构长度
    uint8_t volumeUuid[16];    // 卷标识
    uint8_t buildId[32];       // 内核集构建标识
};
static_assert(sizeof(SystemVolumeFacts) == 52, "SystemVolumeFacts v1 必须是 52 字节");

// 模块导出一条：与容器导出条目同构，在内存里由 PackModule 翻译
struct ModuleExportEntry {
    InterfaceId id;       // 接口标识
    uint16_t version;     // 接口版本
    uint16_t slotCount;   // 槽位数
    const void* slots;    // 槽位表指针
};
static_assert(sizeof(ModuleExportEntry) == 32, "ModuleExportEntry v1 必须是 32 字节");

// 模块导入一条：与容器导入条目同构，槽位回填区由消费者模块提供
struct ModuleImportEntry {
    InterfaceId id;        // 接口标识
    uint16_t minVersion;   // 区间下限
    uint16_t maxVersion;   // 区间上限，0xFFFF 表示不设上限
    uint16_t slotCount;    // 使用的槽数
    void* slots;           // 回填区指针
};
static_assert(sizeof(ModuleImportEntry) == 32, "ModuleImportEntry v1 必须是 32 字节");

// 模块表：PackModule 把它翻译成容器表字节，装载器只按容器表工作
struct ModuleTables {
    uint32_t bytes;                    // 结构长度
    uint16_t exportCount;              // 导出条目数
    uint16_t importCount;              // 导入条目数
    const ModuleExportEntry* exports;  // 导出条目数组
    const ModuleImportEntry* imports;  // 导入条目数组
};
static_assert(sizeof(ModuleTables) == 24, "ModuleTables v1 必须是 24 字节");

// USB 控制传输的 setup 包 v1：8 字节，字段按 USB 2.0 第 9.3 节的小端字节序放置
struct UsbSetupPacket {
    uint8_t requestType;  // 0x00 bmRequestType：方向、类型与接收者
    uint8_t request;      // 0x01 bRequest
    uint16_t value;       // 0x02 wValue
    uint16_t index;       // 0x04 wIndex
    uint16_t length;      // 0x06 wLength
};
static_assert(sizeof(UsbSetupPacket) == 8, "UsbSetupPacket v1 必须是 8 字节");
static_assert(offsetof(UsbSetupPacket, requestType) == 0x00, "UsbSetupPacket.requestType 偏移固定为 0x00");
static_assert(offsetof(UsbSetupPacket, request) == 0x01, "UsbSetupPacket.request 偏移固定为 0x01");
static_assert(offsetof(UsbSetupPacket, value) == 0x02, "UsbSetupPacket.value 偏移固定为 0x02");
static_assert(offsetof(UsbSetupPacket, index) == 0x04, "UsbSetupPacket.index 偏移固定为 0x04");
static_assert(offsetof(UsbSetupPacket, length) == 0x06, "UsbSetupPacket.length 偏移固定为 0x06");

// USB 控制器句柄：UsbCore 分配，0 表示无效
using ControllerHandle = uint32_t;

// USB 设备句柄：UsbCore 分配，0 表示无效
using UsbDeviceHandle = uint32_t;

// USB 类驱动登记句柄：UsbCore 分配，0 表示无效
using ClassHandle = uint32_t;

// USB 设备事实 v1：可扩展，bytes 首字段，字段只追加
struct UsbDeviceFacts {
    uint32_t bytes;                       // 结构长度
    UsbDeviceHandle deviceHandle;         // UsbCore 的设备句柄
    ControllerHandle controllerHandle;    // 所属控制器句柄
    uint32_t classCode;                   // 接口类码
    uint32_t subclass;                    // 接口子类
    uint32_t protocol;                    // 接口协议
    uint32_t vendorId;                    // 设备厂商 ID
    uint32_t productId;                   // 设备产品 ID
    uint32_t speed;                       // 速度：1 低速、2 全速、3 高速、4 超速
    uint32_t port;                        // 根端口号或 hub 端口路径编号
};
static_assert(sizeof(UsbDeviceFacts) == 40, "UsbDeviceFacts v1 必须是 40 字节");
static_assert(offsetof(UsbDeviceFacts, deviceHandle) == 0x04, "UsbDeviceFacts.deviceHandle 偏移固定为 0x04");
static_assert(offsetof(UsbDeviceFacts, controllerHandle) == 0x08, "UsbDeviceFacts.controllerHandle 偏移固定为 0x08");
static_assert(offsetof(UsbDeviceFacts, speed) == 0x20, "UsbDeviceFacts.speed 偏移固定为 0x20");
static_assert(offsetof(UsbDeviceFacts, port) == 0x24, "UsbDeviceFacts.port 偏移固定为 0x24");

// USB 控制器调用表 v1：控制器驱动实现后经 Usb.Host.AddController 注册给 UsbCore
// 所有回调的第一个参数是注册时给的 context；deviceAddress 是 UsbCore 分配的设备地址
struct UsbControllerOps {
    KernelError (*SubmitControl)(void* context, uint32_t deviceAddress, const UsbSetupPacket* packet, void* buffer, uint32_t bytes, uint32_t* actual);  // 提交控制传输并推进到完成
    KernelError (*SubmitBulk)(void* context, uint32_t deviceAddress, uint32_t endpoint, void* buffer, uint32_t bytes, uint32_t* actual);  // 提交批量传输并推进到完成
    KernelError (*Poll)(void* context, uint32_t budget);                  // 推进完成事件，budget 是本次调用最多处理的完成数
    uint32_t (*PortCount)(void* context);                                 // 根端口数
    KernelError (*PortStatus)(void* context, uint32_t port, uint32_t* status);  // 读端口状态位
    KernelError (*PortReset)(void* context, uint32_t port);               // 复位端口并等待完成
    KernelError (*EnableDevice)(void* context, uint32_t port, uint32_t deviceAddress);  // 为端口设备分配地址并启用
    KernelError (*DisableDevice)(void* context, uint32_t deviceAddress);  // 停用设备并释放地址
};
static_assert(sizeof(UsbControllerOps) == 8 * sizeof(void*), "UsbControllerOps v1 必须是 8 个函数指针");

// USB 类驱动回调表 v1：Hub/Storage 等类驱动经 Usb.Host.RegisterClass 注册给 UsbCore
struct UsbClassOps {
    KernelError (*Probe)(void* context, const UsbDeviceFacts* facts, uint32_t* claimed);  // 判断是否接管该设备：claimed 非零表示接管
    KernelError (*Bind)(void* context, const UsbDeviceFacts* facts, uint32_t classHandle);  // 绑定已接管的设备，classHandle 是登记时得到的句柄
    void (*Unbind)(void* context, uint32_t deviceHandle);                // 解绑设备，释放类实例持有的资源
};
static_assert(sizeof(UsbClassOps) == 3 * sizeof(void*), "UsbClassOps v1 必须是 3 个函数指针");
)CONTRACT";

        // Entry.hpp 的固定主体：KernelAbiContext、入口符号与三入口声明
        constexpr const char kEntryBody[] = R"CONTRACT(// 内核 ABI 上下文结构版本
inline constexpr uint16_t kKernelAbiContextVersion = 1;

// 内核到模块的唯一入口参数：32 字节、首字段是长度、只追加
struct KernelAbiContext {
    uint32_t bytes;             // 0x00 本结构长度
    uint16_t version;           // 0x04 上下文结构版本
    uint16_t kernelAbiVersion;  // 0x06 当前 KernelAbiVersion
    uint64_t moduleBase;        // 0x08 模块装载映像的基址
    ModuleId moduleId;          // 0x10 本模块的模块标识
};
static_assert(sizeof(KernelAbiContext) == 32, "KernelAbiContext v1 必须是 32 字节");
static_assert(offsetof(KernelAbiContext, version) == 0x04, "KernelAbiContext.version 偏移固定为 0x04");
static_assert(offsetof(KernelAbiContext, kernelAbiVersion) == 0x06, "KernelAbiContext.kernelAbiVersion 偏移固定为 0x06");
static_assert(offsetof(KernelAbiContext, moduleBase) == 0x08, "KernelAbiContext.moduleBase 偏移固定为 0x08");
static_assert(offsetof(KernelAbiContext, moduleId) == 0x10, "KernelAbiContext.moduleId 偏移固定为 0x10");

// 初始化入口符号名：模块必须导出且偏移非零
inline constexpr char kModuleEntryInit[] = "_Module_Init";

// 激活入口符号名：模块可以缺省
inline constexpr char kModuleEntryActivate[] = "_Module_Activate";

// 中止入口符号名：模块可以缺省
inline constexpr char kModuleEntryAbort[] = "_Module_Abort";

// 模块初始化入口：装载步骤 10 调用，只做可撤销准备
// 声明带 default 可见性：模块编译用 -fvisibility=hidden，定义继承该属性后 PackModule 才能从符号表取到入口
extern "C" __attribute__((visibility("default"))) KernelError _Module_Init(const KernelAbiContext* context);

// 模块激活入口：阶段 3 起在激活条件满足后调用
extern "C" __attribute__((visibility("default"))) KernelError _Module_Activate(const KernelAbiContext* context);

// 模块中止入口：Init 返回非 kOk 且有入口时调用，撤销已登记的资源
extern "C" __attribute__((visibility("default"))) KernelError _Module_Abort(const KernelAbiContext* context);
)CONTRACT";

        // Contract.hpp 的固定主体：聚合 include 全部生成头
        constexpr const char kContractBody[] = R"CONTRACT(#pragma once
#include <InterfaceId.hpp>
#include <ModuleId.hpp>
#include <Types.hpp>
#include <Entry.hpp>
#include <Slots.hpp>
#include <Exports.hpp>
#include <Imports.hpp>
#include <BootClosure.hpp>
#include <Abi.hpp>
)CONTRACT";

        // 生成 InterfaceId.hpp：标识类型、判等与全部接口标识常量
        std::string GenerateInterfaceId(const Contract& contract) {
            Builder builder;
            Header(builder, "InterfaceId.hpp", "接口标识类型与全部稳定名的常量：由 MakeContract 从 Interfaces.def 生成，不手改", {
                "",
                "InterfaceId = SHA-256(\"LPIF01\" || 稳定名 ASCII 字节) 的前 16 字节，全零非法",
                "文本形式为 32 位小写十六进制，只用于日志、诊断与宿主检查输出",
                "运行期任何代码都不从名字重新计算标识",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("// 16 字节接口标识：全零不是合法接口标识");
            builder.Line("struct InterfaceId {");
            builder.Line("    uint8_t bytes[16];    // 原始标识字节");
            builder.Line("};");
            builder.Blank();
            builder.Line("// 判等");
            builder.Line("constexpr bool operator==(const InterfaceId& left, const InterfaceId& right) {");
            builder.Line("    for (uint32_t index = 0; index < 16; ++index) if (left.bytes[index] != right.bytes[index]) return false;");
            builder.Line("    return true;");
            builder.Line("}");
            builder.Blank();
            builder.Line("// 全零判定：全零不是合法接口标识");
            builder.Line("constexpr bool IsZero(const InterfaceId& id) {");
            builder.Line("    for (uint32_t index = 0; index < 16; ++index) if (id.bytes[index] != 0) return false;");
            builder.Line("    return true;");
            builder.Line("}");
            builder.Blank();
            builder.Line("// 写 32 位小写十六进制文本：输出缓冲至少 33 字节，返回写入的字符数");
            builder.Line("constexpr uint32_t Text(const InterfaceId& id, char* out) {");
            builder.Line("    const char digits[] = \"0123456789abcdef\";");
            builder.Line("    for (uint32_t index = 0; index < 16; ++index) {");
            builder.Line("        out[index * 2] = digits[id.bytes[index] >> 4];");
            builder.Line("        out[index * 2 + 1] = digits[id.bytes[index] & 0x0f];");
            builder.Line("    }");
            builder.Line("    return 32;");
            builder.Line("}");
            for (const std::string& name : UniqueInterfaceNames(contract)) {
                builder.Blank();
                builder.Line("// 稳定名 " + name + " 的接口标识");
                builder.Line("inline constexpr InterfaceId " + InterfaceConstantName(name) + " = " + IdInitializer(DeriveInterfaceId(name)) + ";");
            }
            return builder.Text();
        }

        // 生成 ModuleId.hpp：模块标识类型、内核核心全零标识与全部逻辑名常量
        std::string GenerateModuleId(const Contract& contract) {
            Builder builder;
            Header(builder, "ModuleId.hpp", "模块标识类型与全部逻辑名的常量：由 MakeContract 从 BootClosure.def 生成，不手改", {
                "",
                "ModuleId = SHA-256(\"LPMOD01\" || 逻辑名 ASCII 字节) 的前 16 字节",
                "内核核心没有模块条目，需要表示内核核心提供时使用全零标识 kModuleKernelCore",
                "文本形式为 32 位小写十六进制",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("// 16 字节模块标识：全零表示内核核心");
            builder.Line("struct ModuleId {");
            builder.Line("    uint8_t bytes[16];    // 原始标识字节");
            builder.Line("};");
            builder.Blank();
            builder.Line("// 判等");
            builder.Line("constexpr bool operator==(const ModuleId& left, const ModuleId& right) {");
            builder.Line("    for (uint32_t index = 0; index < 16; ++index) if (left.bytes[index] != right.bytes[index]) return false;");
            builder.Line("    return true;");
            builder.Line("}");
            builder.Blank();
            builder.Line("// 全零判定：内核核心的模块标识全零，模块侧全零非法");
            builder.Line("constexpr bool IsZero(const ModuleId& id) {");
            builder.Line("    for (uint32_t index = 0; index < 16; ++index) if (id.bytes[index] != 0) return false;");
            builder.Line("    return true;");
            builder.Line("}");
            builder.Blank();
            builder.Line("// 写 32 位小写十六进制文本：输出缓冲至少 33 字节，返回写入的字符数");
            builder.Line("constexpr uint32_t Text(const ModuleId& id, char* out) {");
            builder.Line("    const char digits[] = \"0123456789abcdef\";");
            builder.Line("    for (uint32_t index = 0; index < 16; ++index) {");
            builder.Line("        out[index * 2] = digits[id.bytes[index] >> 4];");
            builder.Line("        out[index * 2 + 1] = digits[id.bytes[index] & 0x0f];");
            builder.Line("    }");
            builder.Line("    return 32;");
            builder.Line("}");
            builder.Blank();
            builder.Line("// 内核核心的模块标识：全零，用于内核核心提供的归属与导出记录");
            builder.Line("inline constexpr ModuleId kModuleKernelCore = { {");
            builder.Line("        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,");
            builder.Line("        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00");
            builder.Line("    } };");
            for (const ClosureMember& member : contract.closure) {
                builder.Blank();
                builder.Line("// 逻辑名 " + member.name + " 的模块标识（闭包顺序 " + std::to_string(member.order) + "）");
                builder.Line("inline constexpr ModuleId " + ModuleConstantName(member.name) + " = " + IdInitializer(DeriveModuleId(member.name)) + ";");
            }
            return builder.Text();
        }

        // 生成 Types.hpp：返回码、句柄与契约结构
        std::string GenerateTypes() {
            Builder builder;
            Header(builder, "Types.hpp", "契约层基础类型：返回码、句柄与契约结构：由 MakeContract 从模块契约层生成，不手改", {
                "",
                "KernelError 的取值与名字固定；Storage::Error 与",
                "KernelError 的分面与固定映射见 7.3，两套稳定编号不混用",
                "可扩展结构首字段是 uint32_t bytes，按长度有界读取、尾部只追加；ops 表与",
                "<接口>Slots 是固定结构，布局随版本冻结",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stddef.h>");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("#include <BootInfo.hpp>");
            builder.Line("#include <Storage/Error.hpp>");
            builder.Line("#include <Storage/FileSystem.hpp>");
            builder.Blank();
            builder.Line("#include <InterfaceId.hpp>");
            builder.Line("#include <ModuleId.hpp>");
            builder.Blank();
            builder.Raw(kTypesBody);
            return builder.Text();
        }

        // 生成 Entry.hpp：KernelAbiContext、入口符号名与三入口声明
        std::string GenerateEntry() {
            Builder builder;
            Header(builder, "Entry.hpp", "入口 ABI：KernelAbiContext、入口符号名与三入口声明：由 MakeContract 从模块契约层生成，不手改", {
                "",
                "上下文只追加字段、既有字段偏移不变，模块按 bytes 有界读取；三入口返回",
                "KernelError，_Module_Activate 与 _Module_Abort 允许缺省",
                "字节表与调用约定固定",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stddef.h>");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("#include <Types.hpp>");
            builder.Blank();
            builder.Raw(kEntryBody);
            return builder.Text();
        }

        // 生成 Slots.hpp：每接口每版本的槽位结构、别名、槽数常量与槽表符号
        std::string GenerateSlots(const Contract& contract) {
            Builder builder;
            Header(builder, "Slots.hpp", "全部接口版本的槽位结构：成员顺序即槽位号，布局随版本冻结：由 MakeContract 生成，不手改", {
                "",
                "结构体是函数指针数组的强类型视图，",
                "导出表、导入回填区与容器槽位表都以它为唯一布局来源",
                "每个结构用 static_assert 钉死槽数与布局，当前版本别名等于最高版本的结构",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stddef.h>");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("#include <Types.hpp>");
            for (const InterfaceVersion& block : contract.interfaces) {
                builder.Blank();
                builder.Line("// " + block.name + " v" + std::to_string(block.version) + "：槽位表");
                SlotStruct(builder, block);
                builder.Blank();
                builder.Line("// " + block.name + " v" + std::to_string(block.version) + " 的槽位数");
                builder.Line("inline constexpr uint16_t " + SlotCountName(block.name, block.version) + " = " + std::to_string(block.slots.size()) + ";");
            }
            for (const std::string& name : UniqueInterfaceNames(contract)) {
                const InterfaceVersion& highest = HighestBlock(contract, name.c_str());
                builder.Blank();
                builder.Line("// " + name + " 当前版本别名：等于最高版本的结构");
                builder.Line("using " + PascalName(name) + "Slots = " + SlotStructName(name, highest.version) + ";");
                builder.Blank();
                builder.Line("// " + name + " 当前版本号");
                builder.Line("inline constexpr uint16_t " + CurrentVersionName(name) + " = " + std::to_string(highest.version) + ";");
            }
            for (const InterfaceVersion& block : contract.interfaces) {
                const std::string version = std::to_string(block.version);
                builder.Blank();
                builder.Line("// " + block.name + " v" + version + " 的提供者导出表符号：内核核心或闭包模块按此符号导出");
                builder.Line("// 非 const：PackModule 要求槽表落在可写段（Data/Bss）才能逐槽读 RELATIVE 重定位");
                builder.Line("// 声明带 default 可见性：模块编译用 -fvisibility=hidden，定义继承该属性后符号才能进 ELF 符号表");
                builder.Line("extern \"C\" __attribute__((visibility(\"default\"))) " + SlotStructName(block.name, block.version) + " " + ExportSymbolName(block.name, block.version) + ";");
                builder.Blank();
                builder.Line("// " + block.name + " v" + version + " 的消费者回填区符号：装载器按导入表回填");
                builder.Line("extern \"C\" __attribute__((visibility(\"default\"))) " + SlotStructName(block.name, block.version) + " " + ImportSymbolName(block.name, block.version) + ";");
            }
            return builder.Text();
        }

        // 生成 Exports.hpp：内核核心导出表、闭包单例提供者表与提供者模块导出表
        std::string GenerateExports(const Contract& contract) {
            Builder builder;
            Header(builder, "Exports.hpp", "内核核心导出表与闭包单例提供者表：由 MakeContract 生成，不手改", {
                "",
                "内核核心只提供 Core.* 接口，导出表按接口稳定名升序；闭包单例的提供者",
                "与固定表一致，重复提供者在生成期即失败",
                "槽位表指针由 Slots.hpp 声明的 _Contract_Export_<接口>_v<版本> 符号给出",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("#include <InterfaceId.hpp>");
            builder.Line("#include <ModuleId.hpp>");
            builder.Line("#include <Types.hpp>");
            builder.Line("#include <Slots.hpp>");
            builder.Blank();
            std::vector<const InterfaceVersion*> coreBlocks;
            for (const InterfaceVersion& block : contract.interfaces) if (IsKernelCoreInterface(block.name)) coreBlocks.push_back(&block);
            builder.Line("// 内核核心导出表条目数：全部 Core.* 接口的每个版本");
            builder.Line("inline constexpr uint16_t kKernelExportCount = " + std::to_string(coreBlocks.size()) + ";");
            builder.Blank();
            builder.Line("// 内核核心导出表：接口标识、版本、槽数与槽位表指针");
            builder.Line("inline const ModuleExportEntry kKernelExports[kKernelExportCount] = {");
            for (const InterfaceVersion* block : coreBlocks) {
                builder.Line("    { " + InterfaceConstantName(block->name) + ", " + std::to_string(block->version) + ", " + std::to_string(block->slots.size()) + ", &" + ExportSymbolName(block->name, block->version) + " },");
            }
            builder.Line("};");
            builder.Blank();
            builder.Line("// 闭包单例提供者：接口标识、版本与唯一提供者模块标识");
            builder.Line("struct ClosureProviderEntry {");
            builder.Line("    InterfaceId id;       // 接口标识");
            builder.Line("    uint16_t version;     // 接口版本");
            builder.Line("    ModuleId module;      // 唯一提供者模块标识");
            builder.Line("};");
            builder.Blank();
            // 每个闭包单例接口的每个已定义版本各一条：多版本接口（如 Dev.Model v1/v2）由同一提供者导出
            std::vector<std::pair<const SingletonProvider*, const InterfaceVersion*>> closureEntries;
            for (const SingletonProvider& provider : kSingletonProviders) {
                for (const InterfaceVersion& block : contract.interfaces) {
                    if (block.name == provider.interfaceName) closureEntries.push_back({ &provider, &block });
                }
            }
            builder.Line("// 闭包单例提供者条目数：每接口每版本一条");
            builder.Line("inline constexpr uint16_t kClosureProviderCount = " + std::to_string(closureEntries.size()) + ";");
            builder.Blank();
            builder.Line("// 闭包单例提供者表：按接口稳定名与版本升序，提供者唯一");
            builder.Line("inline constexpr ClosureProviderEntry kClosureProviders[kClosureProviderCount] = {");
            for (const auto& pair : closureEntries) {
                builder.Line("    { " + InterfaceConstantName(pair.second->name) + ", " + std::to_string(pair.second->version) + ", " + ModuleConstantName(pair.first->moduleName) + " },");
            }
            builder.Line("};");
            for (const ClosureMember& member : contract.closure) {
                std::vector<const InterfaceVersion*> owned;
                for (const SingletonProvider& provider : kSingletonProviders) {
                    if (member.name != provider.moduleName) continue;
                    for (const InterfaceVersion& block : contract.interfaces) {
                        if (block.name == provider.interfaceName) owned.push_back(&block);
                    }
                }
                if (owned.empty()) continue;
                const std::string pathName = PascalName(member.name);
                builder.Blank();
                builder.Line("// " + member.name + " 模块导出条目数：闭包单例由该模块唯一提供");
                builder.Line("inline constexpr uint16_t k" + pathName + "ExportCount = " + std::to_string(owned.size()) + ";");
                builder.Blank();
                builder.Line("// " + member.name + " 模块导出表");
                builder.Line("inline const ModuleExportEntry k" + pathName + "Exports[k" + pathName + "ExportCount] = {");
                for (const InterfaceVersion* block : owned) {
                    builder.Line("    { " + InterfaceConstantName(block->name) + ", " + std::to_string(block->version) + ", " + std::to_string(block->slots.size()) + ", &" + ExportSymbolName(block->name, block->version) + " },");
                }
                builder.Line("};");
            }
            return builder.Text();
        }

        // 生成 Imports.hpp：导入条目常量与构造辅助
        std::string GenerateImports(const Contract& contract) {
            Builder builder;
            Header(builder, "Imports.hpp", "导入条目常量与构造辅助：由 MakeContract 生成，不手改", {
                "",
                "运行期模块的导入声明（CONTRACT_IMPORTS）由模块构建配置给出，T1.7 骨架接入后",
                "由 MakeContract 的扩展生成每模块导入表；本头先给出按接口可推导的区间常量",
                "区间选择规则：使用槽数不得超过所选版本的槽数",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("#include <Types.hpp>");
            builder.Blank();
            builder.Line("// 导入版本上限哨兵：0xFFFF 表示不设上限");
            builder.Line("inline constexpr uint16_t kImportVersionUnlimited = 0xFFFF;");
            builder.Blank();
            builder.Line("// 构造一条导入条目：槽位回填区由消费者模块提供");
            builder.Line("constexpr ModuleImportEntry MakeImportEntry(InterfaceId id, uint16_t minVersion, uint16_t maxVersion, uint16_t slotCount, void* slots) {");
            builder.Line("    return ModuleImportEntry{ id, minVersion, maxVersion, slotCount, slots };");
            builder.Line("}");
            for (const std::string& name : UniqueInterfaceNames(contract)) {
                const InterfaceVersion& lowest = LowestVersion(contract, name);
                const InterfaceVersion& highest = HighestBlock(contract, name.c_str());
                builder.Blank();
                builder.Line("// " + name + " 的导入区间下限：当前定义的最低版本");
                builder.Line("inline constexpr uint16_t " + MinVersionName(name) + " = " + std::to_string(lowest.version) + ";");
                builder.Blank();
                builder.Line("// " + name + " 的导入区间上限：当前定义的最高版本");
                builder.Line("inline constexpr uint16_t " + MaxVersionName(name) + " = " + std::to_string(highest.version) + ";");
                builder.Blank();
                builder.Line("// " + name + " 当前版本的槽位数：使用槽数不得超过它");
                builder.Line("inline constexpr uint16_t " + SlotCountSummaryName(name) + " = " + std::to_string(highest.slots.size()) + ";");
            }
            return builder.Text();
        }

        // 生成 BootClosure.hpp：内核期望集与闭包成员数
        std::string GenerateBootClosure(const Contract& contract) {
            Builder builder;
            Header(builder, "BootClosure.hpp", "引导闭包期望集表：由 MakeContract 从 BootClosure.def 生成，不手改", {
                "",
                "期望集顺序即预装、注册与 E-KRN-55 完整性判定的顺序，行序由输入文件决定",
                "成员固定 13 个、发行配置不能裁剪；MakeManifest 的预装条目与 CheckPreload",
                "都从本表取，不在别处维护第二份成员表",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("#include <ModuleId.hpp>");
            builder.Blank();
            builder.Line("// 引导闭包成员数：固定值，发行配置不能裁剪");
            builder.Line("inline constexpr uint16_t kBootClosureCount = " + std::to_string(contract.closure.size()) + ";");
            builder.Blank();
            builder.Line("// 期望集条目：预装顺序与逻辑名派生的模块标识");
            builder.Line("struct BootClosureEntry {");
            builder.Line("    uint16_t order;     // 预装顺序，1..13");
            builder.Line("    ModuleId moduleId;  // 逻辑名派生的模块标识");
            builder.Line("};");
            builder.Blank();
            builder.Line("// 内核期望集：行序即预装顺序与注册顺序");
            builder.Line("inline constexpr BootClosureEntry kBootClosure[kBootClosureCount] = {");
            for (const ClosureMember& member : contract.closure) {
                builder.Line("    { " + std::to_string(member.order) + ", " + ModuleConstantName(member.name) + " },");
            }
            builder.Line("};");
            return builder.Text();
        }

        // 生成 Abi.hpp：两个版本常量、Core.Abi 槽 0/2 的固定实现与绑定断言
        std::string GenerateAbi(const Contract& contract) {
            Builder builder;
            Header(builder, "Abi.hpp", "内核 ABI 版本、驱动契约版本与 Core.Abi 版本块绑定：由 MakeContract 生成，不手改", {
                "",
                "两个版本互相独立，取值与抬版条件固定",
                "Core.Abi 槽 0 返回 kKernelAbiVersion，槽 2 返回 kDriveSdkVersion",
                "内核核心导出表必须登记本头给出的实现，不得另写数字",
            });
            builder.Line("#pragma once");
            builder.Line("#include <stdint.h>");
            builder.Blank();
            builder.Line("#include <Slots.hpp>");
            builder.Blank();
            builder.Line("// 内核 ABI 版本：入口、KernelAbiContext、KernelError 语义与核心服务集合的版本");
            builder.Line("inline constexpr uint16_t kKernelAbiVersion = " + std::to_string(kKernelAbiVersionValue) + ";");
            builder.Blank();
            builder.Line("// 驱动契约版本：BlockDeviceOps / FileSystemOps / DriverDesc 布局的版本");
            builder.Line("inline constexpr uint16_t kDriveSdkVersion = " + std::to_string(kDriveSdkVersionValue) + ";");
            builder.Blank();
            builder.Line("// Core.Abi 槽 0 的固定实现：返回内核 ABI 版本");
            builder.Line("extern \"C\" inline uint16_t _Contract_CoreAbi_Version(void) { return kKernelAbiVersion; }");
            builder.Blank();
            builder.Line("// Core.Abi 槽 2 的固定实现：返回驱动契约版本");
            builder.Line("extern \"C\" inline uint16_t _Contract_CoreAbi_DriveSdkVersion(void) { return kDriveSdkVersion; }");
            builder.Blank();
            builder.Line("// Core.Abi 槽 0 的实现符号名：内核核心导出表按名字登记");
            builder.Line("inline constexpr char kCoreAbiVersionSymbol[] = \"_Contract_CoreAbi_Version\";");
            builder.Blank();
            builder.Line("// Core.Abi 槽 2 的实现符号名：内核核心导出表按名字登记");
            builder.Line("inline constexpr char kCoreAbiDriveSdkVersionSymbol[] = \"_Contract_CoreAbi_DriveSdkVersion\";");
            builder.Blank();
            builder.Line("// Core.Abi 版本块必须存在且槽数不少于 3");
            const InterfaceVersion& abi = HighestBlock(contract, "Core.Abi");
            builder.Line("static_assert(" + CurrentVersionName("Core.Abi") + " >= 1, \"Core.Abi 必须在 Interfaces.def 中定义\");");
            builder.Line("static_assert(" + SlotCountName("Core.Abi", 1) + " >= 3, \"Core.Abi v1 的槽数必须不少于 3\");");
            builder.Line("static_assert(" + SlotCountName("Core.Abi", 1) + " == " + std::to_string(abi.slots.size()) + ", \"Core.Abi v1 的槽数必须与生成时一致\");");
            builder.Blank();
            builder.Line("// Core.Abi 槽 0 与槽 2 的签名固定为 uint16_t (void)");
            builder.Line("static_assert(SameContractType<decltype(CoreAbiSlotsV1::Version), uint16_t (*)(void)>::kValue, \"Core.Abi 槽 0 的签名必须是 uint16_t (void)\");");
            builder.Line("static_assert(SameContractType<decltype(CoreAbiSlotsV1::DriveSdkVersion), uint16_t (*)(void)>::kValue, \"Core.Abi 槽 2 的签名必须是 uint16_t (void)\");");
            return builder.Text();
        }

        // 生成 Contract.hpp：聚合 include 全部生成头
        std::string GenerateContractAll() {
            Builder builder;
            Header(builder, "Contract.hpp", "契约生成头的聚合入口：一次引入全部生成物，由 MakeContract 生成，不手改", {
                "",
                "需要契约的包在编译选项里加入生成目录后 include 本头即可；生成物不提交，",
                "构建顺序固定为先跑 MakeContract 再编译内核核心、模块与 PackModule",
            });
            builder.Raw(kContractBody);
            return builder.Text();
        }
    }

    std::vector<OutputFile> Generate(const Contract& contract) {
        std::vector<OutputFile> files;
        files.push_back(OutputFile{ "InterfaceId.hpp", GenerateInterfaceId(contract) });
        files.push_back(OutputFile{ "ModuleId.hpp", GenerateModuleId(contract) });
        files.push_back(OutputFile{ "Types.hpp", GenerateTypes() });
        files.push_back(OutputFile{ "Entry.hpp", GenerateEntry() });
        files.push_back(OutputFile{ "Slots.hpp", GenerateSlots(contract) });
        files.push_back(OutputFile{ "Exports.hpp", GenerateExports(contract) });
        files.push_back(OutputFile{ "Imports.hpp", GenerateImports(contract) });
        files.push_back(OutputFile{ "BootClosure.hpp", GenerateBootClosure(contract) });
        files.push_back(OutputFile{ "Abi.hpp", GenerateAbi(contract) });
        files.push_back(OutputFile{ "Contract.hpp", GenerateContractAll() });
        return files;
    }
}
