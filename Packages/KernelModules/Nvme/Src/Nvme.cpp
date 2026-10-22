/* Nvme.cpp
    NVMe 读路径实现：控制器初始化、Admin 队列、Identify、I/O 队列与 READ 完成轮询

    寄存器读写都经 RegisterAccess 的 32 位回调，64 位寄存器按低半在前拼装；轮询全部有界，
    没有时基依赖（Core.Time 的 TicksPerMs 可能为 0），超时按固定次数上界判定
    完成队列用阶段位区分新旧条目：期望阶段从 1 开始，每绕队列一圈翻转；完成队列内存在使用前
    清零，避免上电残留数据被误判成完成
    读路径每条命令只用一个 PRP1（数据弹跳页），超过页或 MDTR 上限的请求拆成多条命令；
    命令状态码经 MapStatus 映射成 Storage::Error
*/

#include "Nvme.hpp"

#include <Libc.hpp>

#include "NvmeRegisters.hpp"

namespace {
    // 寄存器与命令轮询上限：实机与 QEMU 上都足够宽松，且完全确定
    constexpr uint32_t kRegisterPollLimit = 20000000;
    constexpr uint32_t kCommandPollLimit = 20000000;
    // 门铃步长指数上限：CAP.DSTRD 是 4 位，超过 8 说明实现异常
    constexpr uint32_t kMaxDoorbellStrideShift = 8;
    // MDTS 上限：规范取值 0..8，超过按不设上限处理
    constexpr uint32_t kMaxMdts = 8;
    // 支持的逻辑块大小：4Kn 与 512e 两档
    constexpr uint32_t kLbaSize4k = 4096;
    constexpr uint32_t kLbaSize512 = 512;

    // 读一个 32 位寄存器
    uint32_t Read32(const Nvme::Controller& controller, uint32_t offset) {
        return controller.registers.read(controller.registers.context, offset);
    }

    // 写一个 32 位寄存器
    void Write32(const Nvme::Controller& controller, uint32_t offset, uint32_t value) {
        controller.registers.write(controller.registers.context, offset, value);
    }

    // 读一个 64 位寄存器：低半在前
    uint64_t Read64(const Nvme::Controller& controller, uint32_t offset) {
        return Nvme::Combine64(Read32(controller, offset), Read32(controller, offset + 4));
    }

    // 写一个 64 位寄存器：低半在前
    void Write64(const Nvme::Controller& controller, uint32_t offset, uint64_t value) {
        Write32(controller, offset, static_cast<uint32_t>(value));
        Write32(controller, offset + 4, static_cast<uint32_t>(value >> 32));
    }

    // 小端写入 32 位
    void Put32(uint8_t* target, uint32_t value) {
        target[0] = static_cast<uint8_t>(value);
        target[1] = static_cast<uint8_t>(value >> 8);
        target[2] = static_cast<uint8_t>(value >> 16);
        target[3] = static_cast<uint8_t>(value >> 24);
    }

    // 从完成队列读 32 位字段：条目由设备写，按 volatile 读
    // 轮询循环里没有别的副作用，volatile 让每圈都真的访问内存，避免将来被优化成只读一次
    uint32_t GetCqe32(const uint8_t* entry, uint32_t offset) {
        const volatile uint8_t* bytes = entry + offset;
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
            | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    }

    // 小端读取 32 位
    uint32_t Get32(const uint8_t* source) {
        return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8)
            | (static_cast<uint32_t>(source[2]) << 16) | (static_cast<uint32_t>(source[3]) << 24);
    }

    // 小端读取 64 位
    uint64_t Get64(const uint8_t* source) {
        return static_cast<uint64_t>(Get32(source)) | (static_cast<uint64_t>(Get32(source + 4)) << 32);
    }

    // 清一段字节
    void ZeroBytes(uint8_t* target, uint32_t bytes) {
        for (uint32_t index = 0; index < bytes; ++index) target[index] = 0;
    }

    // 提交队列尾门铃偏移：0x1000 + 2 × 队列号 × 步长
    uint32_t SqTailDoorbell(const Nvme::Controller& controller, uint32_t queueId) {
        return Nvme::kRegDoorbell + (2 * queueId) * controller.doorbellStride;
    }

    // 完成队列头门铃偏移：0x1000 + (2 × 队列号 + 1) × 步长
    uint32_t CqHeadDoorbell(const Nvme::Controller& controller, uint32_t queueId) {
        return Nvme::kRegDoorbell + (2 * queueId + 1) * controller.doorbellStride;
    }

    // 等 CSTS 上的位变成期望值：有界轮询
    bool WaitStatus(const Nvme::Controller& controller, uint32_t mask, bool set) {
        for (uint32_t spin = 0; spin < kRegisterPollLimit; ++spin) {
            const bool present = (Read32(controller, Nvme::kRegStatus) & mask) != 0;
            if (present == set) return true;
        }
        return false;
    }

    // 一条命令的完成等待结果
    enum class WaitResult : uint32_t {
        Ok = 0,     // 收到完成条目
        Timeout,    // 轮询次数用尽
    };

    // 把 16 个双字写进提交队列槽位
    void WriteSqe(uint8_t* queue, uint32_t index, const uint32_t* sqe) {
        uint8_t* slot = queue + index * Nvme::kSqeBytes;
        for (uint32_t word = 0; word < 16; ++word) Put32(slot + word * 4, sqe[word]);
    }

    // 提交一条命令并轮询完成：输出命令结果与状态码；完成队列头指针与阶段位就地推进
    WaitResult Submit(Nvme::Controller& controller, uint32_t queueId, uint8_t* sq, uint8_t* cq, uint32_t depth,
        uint32_t& sqTail, uint32_t& cqHead, uint32_t& cqPhase, uint32_t* sqe, uint32_t* result, uint8_t* status) {
        controller.commandId = controller.commandId + 1;
        sqe[0] = (sqe[0] & 0x0000FFFFu) | ((controller.commandId & 0xFFFFu) << 16);
        WriteSqe(sq, sqTail, sqe);
        sqTail = (sqTail + 1) % depth;
        Write32(controller, SqTailDoorbell(controller, queueId), sqTail);
        for (uint32_t spin = 0; spin < kCommandPollLimit; ++spin) {
            const uint8_t* entry = cq + cqHead * Nvme::kCqeBytes;
            const uint32_t statusWord = GetCqe32(entry, Nvme::kCqeStatus);
            const uint32_t phase = (statusWord & Nvme::kCqePhase) != 0 ? 1u : 0u;
            if (phase != cqPhase) continue;
            // 完成已可见：挡住编译器把后续数据页读取重排到轮询之前
            asm volatile("" : : : "memory");
            if (result != nullptr) *result = GetCqe32(entry, Nvme::kCqeResult);
            if (status != nullptr) {
                *status = static_cast<uint8_t>((statusWord >> Nvme::kCqeStatusShift) & Nvme::kCqeStatusMask);
            }
            cqHead = (cqHead + 1) % depth;
            if (cqHead == 0) cqPhase = (cqPhase == 0) ? 1u : 0u;
            Write32(controller, CqHeadDoorbell(controller, queueId), cqHead);
            return WaitResult::Ok;
        }
        return WaitResult::Timeout;
    }

    // 组装一条 Admin 命令：操作码、NSID、PRP1 与 CDW10/11
    void BuildAdminCommand(uint32_t* sqe, uint8_t opcode, uint32_t nsid, uint64_t prp1, uint32_t cdw10, uint32_t cdw11) {
        for (uint32_t word = 0; word < 16; ++word) sqe[word] = 0;
        sqe[0] = opcode;
        sqe[1] = nsid;
        sqe[6] = static_cast<uint32_t>(prp1);
        sqe[7] = static_cast<uint32_t>(prp1 >> 32);
        sqe[10] = cdw10;
        sqe[11] = cdw11;
    }

    // 发一条 Admin 命令：返回完成等待结果，状态码经 status 带出
    WaitResult AdminCommand(Nvme::Controller& controller, const uint32_t* command, uint8_t* status) {
        uint32_t sqe[16];
        for (uint32_t word = 0; word < 16; ++word) sqe[word] = command[word];
        return Submit(controller, Nvme::kAdminQueueId, controller.adminSq, controller.adminCq, controller.adminDepth,
            controller.adminSqTail, controller.adminCqHead, controller.adminCqPhase, sqe, nullptr, status);
    }

    // Identify：CNS 1 是控制器、0 是命名空间；数据落数据弹跳页
    Nvme::Result Identify(Nvme::Controller& controller, uint32_t cns, uint32_t nsid) {
        uint32_t command[16];
        BuildAdminCommand(command, Nvme::kOpcodeIdentify, nsid, controller.dataPhysical, cns, 0);
        uint8_t status = Nvme::kStatusSuccess;
        if (AdminCommand(controller, command, &status) == WaitResult::Timeout) return Nvme::Result::Timeout;
        if (status != Nvme::kStatusSuccess) return Nvme::Result::Io;
        return Nvme::Result::Ok;
    }

    // 创建 I/O 完成队列或提交队列：PRP1 给出队列基址（页对齐）、PC=1、不启用中断、SQ 指向 1 号 CQ
    Nvme::Result CreateIoQueue(Nvme::Controller& controller, bool completion) {
        const uint32_t size = controller.ioDepth - 1;
        const uint32_t cdw10 = Nvme::kIoQueueId | (size << 16);
        const uint32_t cdw11 = completion ? Nvme::kQueuePhysicallyContiguous
                                          : (Nvme::kQueuePhysicallyContiguous | (Nvme::kIoQueueId << Nvme::kQueueCompletionIdShift));
        // 队列基址必须随创建命令给出：不给（或给 0）时设备会在错误的位置取命令，QEMU 上表现为
        // 把全零条目当成 FLUSH 并回 0x400b；创建/删除队列命令的 NSID 保留，写 0
        const uint64_t base = completion ? controller.ioCqPhysical : controller.ioSqPhysical;
        uint32_t command[16];
        BuildAdminCommand(command, completion ? Nvme::kOpcodeCreateCq : Nvme::kOpcodeCreateSq,
            0, base, cdw10, cdw11);
        uint8_t status = Nvme::kStatusSuccess;
        if (AdminCommand(controller, command, &status) == WaitResult::Timeout) return Nvme::Result::Timeout;
        if (status != Nvme::kStatusSuccess) return Nvme::Result::Io;
        return Nvme::Result::Ok;
    }

    // 删除 I/O 队列：尽力而为，失败不影响停机；删除命令的 NSID 同样保留
    void DeleteIoQueue(Nvme::Controller& controller, bool completion) {
        uint32_t command[16];
        BuildAdminCommand(command, completion ? Nvme::kOpcodeDeleteCq : Nvme::kOpcodeDeleteSq,
            0, 0, Nvme::kIoQueueId, 0);
        uint8_t status = Nvme::kStatusSuccess;
        (void)AdminCommand(controller, command, &status);
    }

    // 从 Identify Controller 数据里取型号：去掉尾部空格与零，超长截断
    void ReadModel(const uint8_t* identity, char* model) {
        uint32_t written = 0;
        for (uint32_t index = 0; index < Nvme::kCtrlModelBytes && written < 40; ++index) {
            model[written++] = static_cast<char>(identity[Nvme::kCtrlModel + index]);
        }
        while (written > 0 && (model[written - 1] == ' ' || model[written - 1] == '\0')) --written;
        model[written] = '\0';
    }

    // 解析 Identify Controller：型号、MDTS 上限、队列条目大小与命名空间数量
    Nvme::Result ParseController(Nvme::Controller& controller) {
        const uint8_t* identity = controller.data;
        ReadModel(identity, controller.model);
        const uint32_t mdts = identity[Nvme::kCtrlMdts];
        controller.maxTransferBytes = (mdts >= 1 && mdts <= kMaxMdts) ? ((1u << mdts) * Nvme::kPageBytes) : 0;
        const uint32_t sqes = identity[Nvme::kCtrlSqes];
        const uint32_t cqes = identity[Nvme::kCtrlCqes];
        if (((sqes >> 4) & 0xFu) < Nvme::kCcIosqesBytes) return Nvme::Result::NotSupported;
        if (((cqes >> 4) & 0xFu) < Nvme::kCcIocqesBytes) return Nvme::Result::NotSupported;
        const uint32_t namespaces = Get32(identity + Nvme::kCtrlNamespaces);
        if (namespaces == 0) return Nvme::Result::Corrupt;
        return Nvme::Result::Ok;
    }

    // 解析 Identify Namespace：NSZE、FLBAS 与 LBA 格式表的 LBADS
    Nvme::Result ParseNamespace(Nvme::Controller& controller) {
        const uint8_t* identity = controller.data;
        controller.blocks = Get64(identity + Nvme::kNsSize);
        if (controller.blocks == 0) return Nvme::Result::Corrupt;
        const uint32_t format = identity[Nvme::kNsFormattedLbaSize] & 0xFu;
        const uint32_t formatCount = identity[Nvme::kNsLbaFormatCountField] + 1;
        if (format >= formatCount || format >= Nvme::kNsLbaFormatCount) return Nvme::Result::Corrupt;
        const uint32_t entry = Get32(identity + Nvme::kNsLbaFormats + format * Nvme::kNsLbaFormatBytes);
        const uint32_t lbads = (entry >> Nvme::kLbaFormatDataSizeShift) & 0xFFu;
        if (lbads == 0) return Nvme::Result::Corrupt;
        if (lbads > 16) return Nvme::Result::NotSupported;
        const uint32_t lbaSize = 1u << lbads;
        if (lbaSize != kLbaSize4k && lbaSize != kLbaSize512) return Nvme::Result::NotSupported;
        controller.lbaSize = lbaSize;
        return Nvme::Result::Ok;
    }

    // 十六进制追加：小写、无前缀、至少一位
    uint32_t AppendHex(char* output, uint32_t position, uint32_t capacity, uint64_t value) {
        static const char kDigits[] = "0123456789abcdef";
        char digits[16];
        uint32_t count = 0;
        do {
            digits[count++] = kDigits[value & 0xFu];
            value >>= 4;
        } while (value != 0);
        while (count > 0 && position < capacity) output[position++] = digits[--count];
        return position;
    }

    // 文本追加：超出容量即截断
    uint32_t AppendText(char* output, uint32_t position, uint32_t capacity, const char* text) {
        for (uint32_t index = 0; text[index] != '\0'; ++index) {
            if (position >= capacity) break;
            output[position++] = text[index];
        }
        return position;
    }
}

namespace Nvme {
    Storage::Error MapStatus(uint8_t status) {
        switch (status) {
            case kStatusSuccess: return Storage::Error::Ok;
            case kStatusInvalidOpcode: return Storage::Error::NotSupported;
            case kStatusInvalidField: return Storage::Error::InvalidArgument;
            case kStatusDataTransfer: return Storage::Error::Io;
            case kStatusInternal: return Storage::Error::Io;
            case kStatusAbortRequested: return Storage::Error::Io;
            case kStatusLbaOutOfRange: return Storage::Error::InvalidArgument;
            case kStatusCapacityExceeded: return Storage::Error::InvalidArgument;
            case kStatusNamespaceNotReady: return Storage::Error::NotSupported;
            case kStatusReservationConflict: return Storage::Error::ReadOnly;
            case kStatusFormatInProgress: return Storage::Error::Io;
            case kStatusWriteToReadOnly: return Storage::Error::ReadOnly;
            default: return Storage::Error::Io;
        }
    }

    Result Initialize(Controller& controller) {
        if (controller.registers.read == nullptr || controller.registers.write == nullptr) return Result::NotSupported;
        if (controller.adminSq == nullptr || controller.adminCq == nullptr || controller.ioSq == nullptr
            || controller.ioCq == nullptr || controller.data == nullptr) {
            return Result::NotSupported;
        }
        if (controller.adminSqPhysical == 0 || controller.adminCqPhysical == 0 || controller.ioSqPhysical == 0
            || controller.ioCqPhysical == 0 || controller.dataPhysical == 0) {
            return Result::NotSupported;
        }
        if (controller.pageBytes < kPageBytes) return Result::NotSupported;
        // NVMe 要求 ASQ/ACQ 按内存页对齐，I/O 队列（PC=1）同理：不对齐直接拒绝，不落到设备侧
        if ((controller.adminSqPhysical % controller.pageBytes) != 0
            || (controller.adminCqPhysical % controller.pageBytes) != 0
            || (controller.ioSqPhysical % controller.pageBytes) != 0
            || (controller.ioCqPhysical % controller.pageBytes) != 0) {
            return Result::NotSupported;
        }
        // CAP/VS：只支持 NVMe 1.x 的 NVM 命令集，最小内存页不能超过调用方的页
        const uint64_t capability = Read64(controller, kRegCap);
        const uint32_t version = Read32(controller, kRegVersion);
        if (((version >> kVersionMajorShift) & kVersionMajorMask) != 1) return Result::NotSupported;
        if (((capability >> 37) & 1u) == 0) return Result::NotSupported;
        const uint32_t mpsmin = static_cast<uint32_t>((capability >> kCapMpsminShift) & kCapMpsminMask);
        if (controller.pageBytes < (1u << (12 + mpsmin))) return Result::NotSupported;
        const uint32_t dstrd = static_cast<uint32_t>((capability >> kCapDstrdShift) & kCapDstrdMask);
        if (dstrd > kMaxDoorbellStrideShift) return Result::NotSupported;
        controller.doorbellStride = 4u << dstrd;
        // 队列深度：取 CAP.MQES+1 与模块上限的较小值，至少两条才能区分空与满
        const uint32_t maxEntries = static_cast<uint32_t>((capability >> kCapMqesShift) & kCapMqesMask) + 1;
        controller.adminDepth = maxEntries < kAdminQueueMaxEntries ? maxEntries : kAdminQueueMaxEntries;
        controller.ioDepth = maxEntries < kIoQueueMaxEntries ? maxEntries : kIoQueueMaxEntries;
        if (controller.adminDepth < kMinQueueEntries || controller.ioDepth < kMinQueueEntries) return Result::NotSupported;
        // 关闭控制器：CC.EN=0 并等 RDY 清零
        Write32(controller, kRegConfig, 0);
        if (!WaitStatus(controller, kCstsReady, false)) return Result::Timeout;
        // Admin 队列属性与基址
        const uint32_t aqa = ((controller.adminDepth - 1) << kAqaAsqsShift) | ((controller.adminDepth - 1) << kAqaAcqsShift);
        Write32(controller, kRegAqa, aqa);
        Write64(controller, kRegAsq, controller.adminSqPhysical);
        Write64(controller, kRegAcq, controller.adminCqPhysical);
        // 使能：NVM 命令集、4KiB 内存页、64 字节 SQE、16 字节 CQE
        const uint32_t config = kCcEnable | (0u << kCcCssShift) | (0u << kCcMpsShift)
            | (kCcIosqesBytes << kCcIosqesShift) | (kCcIocqesBytes << kCcIocqesShift);
        Write32(controller, kRegConfig, config);
        if (!WaitStatus(controller, kCstsReady, true)) return Result::Timeout;
        if ((Read32(controller, kRegStatus) & kCstsFatal) != 0) return Result::Io;
        // 完成队列清零后再用：上电残留不能当成完成条目
        ZeroBytes(controller.adminCq, controller.adminDepth * kCqeBytes);
        ZeroBytes(controller.ioCq, controller.ioDepth * kCqeBytes);
        controller.adminSqTail = 0;
        controller.adminCqHead = 0;
        controller.adminCqPhase = 1;
        controller.ioSqTail = 0;
        controller.ioCqHead = 0;
        controller.ioCqPhase = 1;
        controller.commandId = 0;
        // Identify 控制器与命名空间
        if (const Result result = Identify(controller, kIdentifyController, 0); result != Result::Ok) return result;
        if (const Result result = ParseController(controller); result != Result::Ok) return result;
        if (const Result result = Identify(controller, kIdentifyNamespace, kNamespaceId); result != Result::Ok) return result;
        if (const Result result = ParseNamespace(controller); result != Result::Ok) return result;
        // I/O 队列：先建完成队列，再建提交队列
        if (const Result result = CreateIoQueue(controller, true); result != Result::Ok) return result;
        if (const Result result = CreateIoQueue(controller, false); result != Result::Ok) return result;
        controller.enabled = 1;
        return Result::Ok;
    }

    Storage::Error Read(Controller& controller, uint64_t offset, void* buffer, uint32_t bytes) {
        if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
        if (controller.enabled == 0 || controller.lbaSize == 0) return Storage::Error::NotSupported;
        if ((offset % controller.lbaSize) != 0 || (bytes % controller.lbaSize) != 0) return Storage::Error::InvalidArgument;
        const uint64_t lba = offset / controller.lbaSize;
        const uint64_t count = bytes / controller.lbaSize;
        if (lba >= controller.blocks || count > controller.blocks - lba) return Storage::Error::InvalidArgument;
        uint8_t* target = static_cast<uint8_t*>(buffer);
        uint64_t remaining = bytes;
        uint64_t current = lba;
        while (remaining > 0) {
            uint32_t chunk = controller.pageBytes;
            if (controller.maxTransferBytes != 0 && chunk > controller.maxTransferBytes) chunk = controller.maxTransferBytes;
            if (remaining < chunk) chunk = static_cast<uint32_t>(remaining);
            const uint32_t nlb = chunk / controller.lbaSize;
            uint32_t sqe[16];
            for (uint32_t word = 0; word < 16; ++word) sqe[word] = 0;
            sqe[0] = kOpcodeRead;
            sqe[1] = kNamespaceId;
            sqe[6] = static_cast<uint32_t>(controller.dataPhysical);
            sqe[7] = static_cast<uint32_t>(controller.dataPhysical >> 32);
            sqe[10] = static_cast<uint32_t>(current);
            sqe[11] = static_cast<uint32_t>(current >> 32);
            sqe[12] = nlb - 1;
            uint8_t status = kStatusSuccess;
            if (Submit(controller, kIoQueueId, controller.ioSq, controller.ioCq, controller.ioDepth,
                    controller.ioSqTail, controller.ioCqHead, controller.ioCqPhase, sqe, nullptr, &status) == WaitResult::Timeout) {
                return Storage::Error::Io;
            }
            if (status != kStatusSuccess) return MapStatus(status);
            memcpy(target, controller.data, chunk);
            target += chunk;
            current += nlb;
            remaining -= chunk;
        }
        return Storage::Error::Ok;
    }

    void Shutdown(Controller& controller) {
        if (controller.registers.read == nullptr || controller.registers.write == nullptr) return;
        if (controller.enabled != 0) {
            DeleteIoQueue(controller, false);
            DeleteIoQueue(controller, true);
        }
        Write32(controller, kRegConfig, 0);
        (void)WaitStatus(controller, kCstsReady, false);
        controller.enabled = 0;
    }

    uint32_t DescribeDevice(const Controller& controller, char* output, uint32_t capacity) {
        if (output == nullptr || capacity == 0) return 0;
        uint32_t position = AppendText(output, 0, capacity, "nvme: ");
        position = AppendText(output, position, capacity, controller.model);
        position = AppendText(output, position, capacity, " sectors=0x");
        position = AppendHex(output, position, capacity, controller.blocks);
        position = AppendText(output, position, capacity, " block=0x");
        position = AppendHex(output, position, capacity, controller.lbaSize);
        if (position < capacity) output[position] = '\0';
        return position;
    }

    namespace {
        // 块设备回调：context 是 Controller 指针
        uint32_t OpsBlockBytes(void* context) {
            return static_cast<Controller*>(context)->lbaSize;
        }

        uint64_t OpsCapacityBytes(void* context) {
            const Controller* controller = static_cast<Controller*>(context);
            return controller->blocks * controller->lbaSize;
        }

        uint32_t OpsWritable(void* context) {
            (void)context;
            return 1u;
        }

        Storage::Error OpsRead(void* context, uint64_t offset, void* buffer, uint32_t bytes) {
            return Read(*static_cast<Controller*>(context), offset, buffer, bytes);
        }

        Storage::Error OpsWrite(void* context, uint64_t offset, const void* buffer, uint32_t bytes) {
            (void)context;
            (void)offset;
            (void)buffer;
            (void)bytes;
            return Storage::Error::ReadOnly;
        }

        Storage::Error OpsFlush(void* context) {
            (void)context;
            return Storage::Error::Ok;
        }
    }

    // 块设备调用表：能力位为 0（没有真实刷新原语），回调全部是模块内函数地址
    const BlockDeviceOps kOps = {0u, &OpsBlockBytes, &OpsCapacityBytes, &OpsWritable, &OpsRead, &OpsWrite, &OpsFlush};
}
