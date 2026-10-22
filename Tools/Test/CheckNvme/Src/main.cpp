/* main.cpp
    CheckNvme：NVMe 读路径模块逻辑的宿主侧检查，手工执行

    检查对象是 Packages/KernelModules/Nvme/Src/Nvme.cpp：用 vpath 按相对路径直接编进本工具，
    不复制实现；Module.cpp 与 Module.mk 的契约表、导入导出与段形态由模块 make check 与
    PackModule --verify 核对，本工具只驱动控制器逻辑

    伪设备：寄存器阵列实现 CAP/VS/CC/CSTS/AQA/ASQ/ACQ 与门铃语义，门铃写入时从提交队列取
    命令、按操作码执行（Identify 填数据页、创建/删除 I/O 队列、READ 从伪介质拷贝到 PRP1），
    再按阶段位写完成条目；可注入「永不完成」「错误状态」与「RDY 不置位」用于超时与错误路径
    队列与数据页都是宿主缓冲，控制器按物理地址写 PRP1，伪设备经登记的 DMA 区间换算回宿主指针

    每个用例打印 [ OK ] 或 [ FAIL ]；用例数固定，末尾与实跑数核对；失败返回 1
*/

#include "Nvme.hpp"
#include "NvmeRegisters.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    // 用例总数：与用例表长度核对，防止用例被误删
    constexpr unsigned kCaseCount = 31;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // DMA 页的假物理地址：每张队列独占一页，页对齐是 NVMe 对 ASQ/ACQ 与 I/O 队列的硬要求
    constexpr uint64_t kAdminSqPagePhysical = 0x200000;
    constexpr uint64_t kAdminCqPagePhysical = 0x201000;
    constexpr uint64_t kIoPagePhysical = 0x202000;
    constexpr uint64_t kIoCqPagePhysical = 0x203000;
    constexpr uint64_t kDataPagePhysical = 0x204000;
    // 伪设备寄存器区大小：覆盖 0x1000 门铃与可能的步长余量
    constexpr uint32_t kRegisterBytes = 0x2000;
    // 捕获的命令条数上限
    constexpr uint32_t kCaptureCount = 16;

    // 小端写入 32 位
    void Put32(uint8_t* target, uint32_t value) {
        target[0] = static_cast<uint8_t>(value);
        target[1] = static_cast<uint8_t>(value >> 8);
        target[2] = static_cast<uint8_t>(value >> 16);
        target[3] = static_cast<uint8_t>(value >> 24);
    }

    // 小端写入 64 位
    void Put64(uint8_t* target, uint64_t value) {
        Put32(target, static_cast<uint32_t>(value));
        Put32(target + 4, static_cast<uint32_t>(value >> 32));
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

    // 登记的 DMA 区间：物理地址到宿主指针
    struct DmaRegion {
        uint64_t physical = 0;  // 假物理起址
        uint8_t* host = nullptr; // 宿主缓冲
        uint32_t bytes = 0;     // 长度
    };

    // 伪 NVMe 控制器：寄存器、队列缓冲、命名空间事实与捕获
    struct FakeNvme {
        uint32_t registers[kRegisterBytes / 4] = {};  // 寄存器与门铃
        // 队列宿主指针：只由协议给出（ASQ/ACQ 寄存器与创建队列命令的 PRP1），伪设备不预置
        uint8_t* adminSq = nullptr;                   // Admin 提交队列宿主指针
        uint8_t* adminCq = nullptr;                   // Admin 完成队列宿主指针
        uint8_t* ioSq = nullptr;                      // I/O 提交队列宿主指针
        uint8_t* ioCq = nullptr;                      // I/O 完成队列宿主指针
        uint32_t queueMisaligned = 0;                 // 队列基址未按页对齐时为 1，使能时报 CFS
        uint32_t deferNext = 0;                       // 为 1 时下一条完成延迟到测试线程写入
        DmaRegion regions[8] = {};                    // 登记的 DMA 区间
        uint32_t regionCount = 0;                     // 区间数
        // 设备侧配置
        uint64_t mqes = 63;              // CAP.MQES，0 基
        uint32_t dstrd = 0;              // CAP.DSTRD
        uint32_t mpsmin = 0;             // CAP.MPSMIN
        bool nvmCommandSet = true;       // CAP 的 NVM 命令集位
        uint32_t version = 0x00010400;   // VS：1.4
        bool rdyOnEnable = true;         // 置 EN 后 RDY 是否就绪
        bool completeCommands = true;    // 为假时命令不写完成条目（超时路径）
        uint8_t statusOverride = 0xFF;   // 下一条命令的状态码；0xFF 表示不注入
        uint64_t nsze = 0x400000;        // NSZE
        uint32_t lbads = 12;             // LBAF0 的 LBADS：12 → 4096
        uint32_t flbas = 0;              // FLBAS 当前格式下标
        uint32_t namespaces = 1;         // Identify Controller 的 NN
        uint32_t mdts = 0;               // MDTS
        uint32_t sqesMax = 6;            // SQES 最大条目指数
        uint32_t cqesMax = 4;            // CQES 最大条目指数
        const char* model = "QEMU HARDDISK";  // MN
        std::vector<uint8_t>* media = nullptr; // 伪介质
        // 队列生产/消费状态
        uint32_t adminDepth = 0;         // Admin 队列深度（由 AQA 得出）
        uint32_t ioDepth = 0;            // I/O 队列深度（由创建队列得出）
        uint32_t adminSqConsumer = 0;    // Admin 提交队列消费位置
        uint32_t adminCqProducer = 0;    // Admin 完成队列生产位置
        uint32_t adminCqPhase = 1;       // Admin 完成队列阶段位
        uint32_t ioSqConsumer = 0;       // I/O 提交队列消费位置
        uint32_t ioCqProducer = 0;       // I/O 完成队列生产位置
        uint32_t ioCqPhase = 1;          // I/O 完成队列阶段位
        // 捕获与计数
        uint32_t capturedSqe[kCaptureCount][16] = {};  // 最近的命令条目
        uint32_t capturedCount = 0;                    // 已捕获条数
        uint32_t identifyCount = 0;                    // Identify 条数
        uint32_t createCqCount = 0;                    // 创建完成队列条数
        uint32_t createSqCount = 0;                    // 创建提交队列条数
        uint32_t deleteCqCount = 0;                    // 删除完成队列条数
        uint32_t deleteSqCount = 0;                    // 删除提交队列条数
        uint32_t readCount = 0;                        // 读命令条数
        uint32_t ccWrites = 0;                         // CC 写次数
        uint32_t lastCc = 0;                           // 最近一次 CC 值
        uint32_t aqaValue = 0;                         // AQA 写入值
        uint64_t asqValue = 0;                         // ASQ 写入值
        uint64_t acqValue = 0;                         // ACQ 写入值
        uint32_t sqTailDoorbells = 0;                  // 提交队列尾门铃次数
        uint32_t cqHeadDoorbells = 0;                  // 完成队列头门铃次数
        uint32_t asqLow = 0;                           // ASQ 低半暂存
        uint32_t acqLow = 0;                           // ACQ 低半暂存
    };

    // 物理地址换算成宿主指针：不在登记区间时返回空，不抛异常（伪设备校验非法基址用）
    uint8_t* TryHostOf(FakeNvme& device, uint64_t physical) {
        for (uint32_t index = 0; index < device.regionCount; ++index) {
            const DmaRegion& region = device.regions[index];
            if (physical < region.physical || physical - region.physical >= region.bytes) continue;
            return region.host + (physical - region.physical);
        }
        return nullptr;
    }

    // 物理地址换算成宿主指针：必须在登记的区间内
    uint8_t* HostOf(FakeNvme& device, uint64_t physical) {
        uint8_t* host = TryHostOf(device, physical);
        if (host == nullptr) Fail("the DMA address is outside the registered regions");
        return host;
    }

    // 登记一段 DMA 区间
    void AddRegion(FakeNvme& device, uint64_t physical, uint8_t* host, uint32_t bytes) {
        Require(device.regionCount < 8, "the DMA region table is full");
        device.regions[device.regionCount].physical = physical;
        device.regions[device.regionCount].host = host;
        device.regions[device.regionCount].bytes = bytes;
        ++device.regionCount;
    }

    // 设置 64 位 CAP 寄存器
    void SetCapability(FakeNvme& device, uint64_t capability) {
        device.registers[Nvme::kRegCap / 4] = static_cast<uint32_t>(capability);
        device.registers[Nvme::kRegCap / 4 + 1] = static_cast<uint32_t>(capability >> 32);
    }

    // 组装 CAP：MQES / TO / DSTRD / CSS(NVM) / MPSMIN
    uint64_t MakeCapability(const FakeNvme& device) {
        uint64_t capability = (device.mqes & 0xFFFFu) | (4ull << Nvme::kCapToShift);
        capability |= static_cast<uint64_t>(device.dstrd & 0xFu) << Nvme::kCapDstrdShift;
        capability |= static_cast<uint64_t>(device.mpsmin & 0xFu) << Nvme::kCapMpsminShift;
        if (device.nvmCommandSet) capability |= 1ull << 37;
        return capability;
    }

    // 写一条完成条目：阶段位随生产者绕圈翻转
    void WriteCqe(uint8_t* queue, uint32_t& producer, uint32_t& phase, uint32_t depth,
        uint16_t commandId, uint8_t status) {
        uint8_t* entry = queue + producer * Nvme::kCqeBytes;
        for (uint32_t index = 0; index < Nvme::kCqeBytes; ++index) entry[index] = 0;
        Put32(entry + Nvme::kCqeStatus, static_cast<uint32_t>(commandId)
            | (phase << 16) | (static_cast<uint32_t>(status) << Nvme::kCqeStatusShift));
        producer = (producer + 1) % depth;
        if (producer == 0) phase = (phase == 0) ? 1u : 0u;
    }

    // 延迟完成：伪设备先把完成信息登记到这里，测试线程稍后写入完成队列
    std::atomic<uint32_t> g_deferReady{0};  // 1 表示有完成等待写入
    FakeNvme* g_deferDevice = nullptr;      // 延迟完成所属的伪设备
    uint32_t g_deferQueue = 0;              // 队列号
    uint16_t g_deferCid = 0;                // 命令标识
    uint8_t g_deferStatus = 0;              // 状态码

    // 登记一条延迟完成：由伪设备在门铃处理里调用，不在同一时刻写完成队列
    void DeferCompletion(FakeNvme& device, uint32_t queueId, uint16_t cid, uint8_t status) {
        g_deferDevice = &device;
        g_deferQueue = queueId;
        g_deferCid = cid;
        g_deferStatus = status;
        g_deferReady.store(1, std::memory_order_release);
    }

    // 测试线程：等延迟完成出现后睡 2ms 再写完成队列，模拟设备异步完成
    // 驱动侧只有每圈重新读完成队列才能看到它；把轮询读提到循环外会一直超时
    void DeferredWriter() {
        for (uint32_t spin = 0; spin < 200000000u; ++spin) {
            if (g_deferReady.load(std::memory_order_acquire) != 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        FakeNvme& device = *g_deferDevice;
        if (g_deferQueue == Nvme::kAdminQueueId) {
            WriteCqe(device.adminCq, device.adminCqProducer, device.adminCqPhase, device.adminDepth, g_deferCid, g_deferStatus);
        } else {
            WriteCqe(device.ioCq, device.ioCqProducer, device.ioCqPhase, device.ioDepth, g_deferCid, g_deferStatus);
        }
        g_deferReady.store(0, std::memory_order_release);
    }

    // 捕获一条命令条目
    void CaptureSqe(FakeNvme& device, const uint8_t* sqe) {
        if (device.capturedCount >= kCaptureCount) return;
        for (uint32_t word = 0; word < 16; ++word) device.capturedSqe[device.capturedCount][word] = Get32(sqe + word * 4);
        ++device.capturedCount;
    }

    // 取第 index 条捕获条目的双字
    uint32_t CapturedWord(const FakeNvme& device, uint32_t index, uint32_t word) {
        Require(index < device.capturedCount, "the captured command is missing");
        return device.capturedSqe[index][word];
    }

    // 从捕获条目里找指定操作码的第一条下标
    uint32_t FindCaptured(const FakeNvme& device, uint8_t opcode) {
        for (uint32_t index = 0; index < device.capturedCount; ++index) {
            if ((device.capturedSqe[index][0] & 0xFFu) == opcode) return index;
        }
        Fail("the expected command opcode was not captured");
    }

    // 填 Identify Controller 数据
    void FillIdentifyController(FakeNvme& device, uint8_t* buffer) {
        for (uint32_t index = 0; index < Nvme::kIdentifyBytes; ++index) buffer[index] = 0;
        Put32(buffer, 0x1B36u | (0x0010u << 16));   // VID / SSVID
        // 型号按字符串复制，不足 40 字节的尾部补空格：不能读过结尾零
        uint32_t written = 0;
        while (written < Nvme::kCtrlModelBytes && device.model[written] != '\0') {
            buffer[Nvme::kCtrlModel + written] = static_cast<uint8_t>(device.model[written]);
            ++written;
        }
        while (written < Nvme::kCtrlModelBytes) buffer[Nvme::kCtrlModel + written++] = ' ';
        buffer[Nvme::kCtrlMdts] = static_cast<uint8_t>(device.mdts);
        buffer[Nvme::kCtrlSqes] = static_cast<uint8_t>((device.sqesMax << 4) | 6u);
        buffer[Nvme::kCtrlCqes] = static_cast<uint8_t>((device.cqesMax << 4) | 4u);
        Put32(buffer + Nvme::kCtrlNamespaces, device.namespaces);
    }

    // 填 Identify Namespace 数据
    void FillIdentifyNamespace(FakeNvme& device, uint8_t* buffer) {
        for (uint32_t index = 0; index < Nvme::kIdentifyBytes; ++index) buffer[index] = 0;
        Put64(buffer + Nvme::kNsSize, device.nsze);
        Put64(buffer + Nvme::kNsCapacity, device.nsze);
        buffer[Nvme::kNsFormattedLbaSize] = static_cast<uint8_t>(device.flbas & 0xFu);
        Put32(buffer + Nvme::kNsLbaFormats, device.lbads << Nvme::kLbaFormatDataSizeShift);
    }

    // 执行一条 Admin 命令：Identify、创建/删除 I/O 队列
    uint8_t ExecuteAdmin(FakeNvme& device, const uint8_t* sqe) {
        const uint8_t opcode = sqe[Nvme::kSqeOpcode];
        const uint32_t cdw10 = Get32(sqe + Nvme::kSqeCdw10);
        switch (opcode) {
            case Nvme::kOpcodeIdentify: {
                ++device.identifyCount;
                uint8_t* buffer = TryHostOf(device, Get64(sqe + Nvme::kSqePrp1));
                if (buffer == nullptr) return Nvme::kStatusInvalidField;
                if ((cdw10 & 0xFFu) == Nvme::kIdentifyController) FillIdentifyController(device, buffer);
                else FillIdentifyNamespace(device, buffer);
                return Nvme::kStatusSuccess;
            }
            case Nvme::kOpcodeCreateCq: {
                ++device.createCqCount;
                // 完成队列基址随 PRP1 给出：非零、页对齐、落在登记区间内，缺一即拒绝
                const uint64_t base = Get64(sqe + Nvme::kSqePrp1);
                if (base == 0 || (base % Nvme::kPageBytes) != 0) return Nvme::kStatusInvalidField;
                device.ioCq = TryHostOf(device, base);
                if (device.ioCq == nullptr) return Nvme::kStatusInvalidField;
                device.ioDepth = ((cdw10 >> 16) & 0xFFFFu) + 1;
                return Nvme::kStatusSuccess;
            }
            case Nvme::kOpcodeCreateSq: {
                ++device.createSqCount;
                const uint64_t base = Get64(sqe + Nvme::kSqePrp1);
                if (base == 0 || (base % Nvme::kPageBytes) != 0) return Nvme::kStatusInvalidField;
                device.ioSq = TryHostOf(device, base);
                if (device.ioSq == nullptr) return Nvme::kStatusInvalidField;
                return Nvme::kStatusSuccess;
            }
            case Nvme::kOpcodeDeleteCq: {
                ++device.deleteCqCount;
                return Nvme::kStatusSuccess;
            }
            case Nvme::kOpcodeDeleteSq: {
                ++device.deleteSqCount;
                return Nvme::kStatusSuccess;
            }
            default:
                return Nvme::kStatusInvalidOpcode;
        }
    }

    // 执行一条 I/O 命令：只实现读
    uint8_t ExecuteIo(FakeNvme& device, const uint8_t* sqe) {
        const uint8_t opcode = sqe[Nvme::kSqeOpcode];
        if (opcode != Nvme::kOpcodeRead) return Nvme::kStatusInvalidOpcode;
        ++device.readCount;
        const uint64_t slba = Get32(sqe + Nvme::kSqeCdw10) | (static_cast<uint64_t>(Get32(sqe + Nvme::kSqeCdw11)) << 32);
        const uint32_t nlb = (Get32(sqe + Nvme::kSqeCdw12) & 0xFFFFu) + 1;
        uint8_t* target = HostOf(device, Get64(sqe + Nvme::kSqePrp1));
        const uint32_t lbaSize = 1u << device.lbads;
        const uint64_t offset = slba * lbaSize;
        const uint64_t bytes = static_cast<uint64_t>(nlb) * lbaSize;
        Require(device.media != nullptr, "the fake media is missing");
        Require(offset + bytes <= device.media->size(), "the fake read is outside the media");
        std::memcpy(target, device.media->data() + offset, static_cast<size_t>(bytes));
        return Nvme::kStatusSuccess;
    }

    // 处理提交队列尾门铃：把消费位置到写入值之间的命令执行完
    void ProcessSqDoorbell(FakeNvme& device, uint32_t queueId, uint32_t tail) {
        if (queueId == Nvme::kAdminQueueId) {
            if (device.adminDepth == 0) return;
            while (device.adminSqConsumer != tail) {
                const uint8_t* sqe = device.adminSq + device.adminSqConsumer * Nvme::kSqeBytes;
                CaptureSqe(device, sqe);
                uint8_t status = device.statusOverride;
                if (status == 0xFF) status = ExecuteAdmin(device, sqe);
                device.statusOverride = 0xFF;
                const uint16_t cid = static_cast<uint16_t>(Get32(sqe + Nvme::kSqeCommandId) & 0xFFFFu);
                if (device.deferNext != 0) {
                    device.deferNext = 0;
                    DeferCompletion(device, Nvme::kAdminQueueId, cid, status);
                } else if (device.completeCommands) {
                    WriteCqe(device.adminCq, device.adminCqProducer, device.adminCqPhase, device.adminDepth, cid, status);
                }
                device.adminSqConsumer = (device.adminSqConsumer + 1) % device.adminDepth;
            }
            return;
        }
        if (device.ioDepth == 0) return;
        while (device.ioSqConsumer != tail) {
            const uint8_t* sqe = device.ioSq + device.ioSqConsumer * Nvme::kSqeBytes;
            CaptureSqe(device, sqe);
            uint8_t status = device.statusOverride;
            if (status == 0xFF) status = ExecuteIo(device, sqe);
            device.statusOverride = 0xFF;
            const uint16_t cid = static_cast<uint16_t>(Get32(sqe + Nvme::kSqeCommandId) & 0xFFFFu);
            if (device.deferNext != 0) {
                device.deferNext = 0;
                DeferCompletion(device, Nvme::kIoQueueId, cid, status);
            } else if (device.completeCommands) {
                WriteCqe(device.ioCq, device.ioCqProducer, device.ioCqPhase, device.ioDepth, cid, status);
            }
            device.ioSqConsumer = (device.ioSqConsumer + 1) % device.ioDepth;
        }
    }

    // 处理门铃写入：区分提交队列尾与完成队列头
    void HandleDoorbell(FakeNvme& device, uint32_t offset, uint32_t value) {
        const uint32_t stride = 4u << device.dstrd;
        const uint32_t index = (offset - Nvme::kRegDoorbell) / stride;
        const uint32_t queueId = index / 2;
        if ((index & 1u) != 0) {
            ++device.cqHeadDoorbells;
            return;
        }
        ++device.sqTailDoorbells;
        ProcessSqDoorbell(device, queueId, value);
    }

    // 伪寄存器读：门铃读零，其余取阵列值
    uint32_t FakeRegisterRead(void* context, uint32_t offset) {
        FakeNvme* device = static_cast<FakeNvme*>(context);
        if (offset >= Nvme::kRegDoorbell) return 0;
        Require(offset % 4 == 0 && offset / 4 < kRegisterBytes / 4, "the fake register offset is out of range");
        return device->registers[offset / 4];
    }

    // 从 ASQ/ACQ 寄存器值刷新 Admin 队列宿主指针：非零且页对齐才有效，否则标记使能失败
    void RefreshAdminQueues(FakeNvme& device) {
        // 每次按当前寄存器值重判：中间态（高半还没写）不算非法，最终值才是权威
        device.queueMisaligned = 0;
        device.adminSq = nullptr;
        device.adminCq = nullptr;
        if (device.asqValue == 0 || device.acqValue == 0
            || (device.asqValue % Nvme::kPageBytes) != 0 || (device.acqValue % Nvme::kPageBytes) != 0) {
            device.queueMisaligned = 1;
            return;
        }
        device.adminSq = TryHostOf(device, device.asqValue);
        device.adminCq = TryHostOf(device, device.acqValue);
        if (device.adminSq == nullptr || device.adminCq == nullptr) device.queueMisaligned = 1;
    }

    // 伪寄存器写：CC/AQA/ASQ/ACQ/门铃有设备语义，其余直接存
    void FakeRegisterWrite(void* context, uint32_t offset, uint32_t value) {
        FakeNvme* device = static_cast<FakeNvme*>(context);
        if (offset >= Nvme::kRegDoorbell) {
            HandleDoorbell(*device, offset, value);
            return;
        }
        Require(offset % 4 == 0 && offset / 4 < kRegisterBytes / 4, "the fake register offset is out of range");
        device->registers[offset / 4] = value;
        if (offset == Nvme::kRegConfig) {
            ++device->ccWrites;
            device->lastCc = value;
            const bool enable = (value & Nvme::kCcEnable) != 0;
            if (enable) {
                if (device->queueMisaligned != 0) {
                    // 与 QEMU 一致：队列基址未按页对齐时使能失败并置 CFS，RDY 不置位
                    device->registers[Nvme::kRegStatus / 4] |= Nvme::kCstsFatal;
                } else if (device->rdyOnEnable) {
                    device->registers[Nvme::kRegStatus / 4] |= Nvme::kCstsReady;
                }
            } else {
                device->registers[Nvme::kRegStatus / 4] &= ~Nvme::kCstsReady;
            }
            return;
        }
        if (offset == Nvme::kRegAqa) {
            device->aqaValue = value;
            device->adminDepth = ((value & Nvme::kAqaAsqsMask) >> Nvme::kAqaAsqsShift) + 1;
            return;
        }
        if (offset == Nvme::kRegAsq) {
            device->asqLow = value;
            device->asqValue = Nvme::Combine64(device->asqLow, static_cast<uint32_t>(device->asqValue >> 32));
            RefreshAdminQueues(*device);
            return;
        }
        if (offset == Nvme::kRegAsq + 4) {
            device->asqValue = Nvme::Combine64(device->asqLow, value);
            RefreshAdminQueues(*device);
            return;
        }
        if (offset == Nvme::kRegAcq) {
            device->acqLow = value;
            device->acqValue = Nvme::Combine64(device->acqLow, static_cast<uint32_t>(device->acqValue >> 32));
            RefreshAdminQueues(*device);
            return;
        }
        if (offset == Nvme::kRegAcq + 4) {
            device->acqValue = Nvme::Combine64(device->acqLow, value);
            RefreshAdminQueues(*device);
        }
    }

    // 控制器夹具：伪设备、DMA 页、伪介质与控制器实例
    struct Fixture {
        FakeNvme device;                    // 伪设备
        uint8_t adminSqPage[0x1000] = {};   // Admin 提交队列页
        uint8_t adminCqPage[0x1000] = {};   // Admin 完成队列页
        uint8_t ioPage[0x1000] = {};        // I/O 提交队列页
        uint8_t ioCqPage[0x1000] = {};      // I/O 完成队列页
        uint8_t dataPage[0x1000] = {};      // 数据弹跳页
        std::vector<uint8_t> media;         // 伪介质
        Nvme::Controller controller;        // 控制器实例
    };

    // 建夹具：登记 DMA 区间、填控制器指针、设置伪介质与寄存器；可重复调用
    void Setup(Fixture& fixture, uint64_t mediaBytes = 0x100000) {
        fixture.device = FakeNvme{};
        fixture.controller = Nvme::Controller{};
        fixture.media.assign(static_cast<size_t>(mediaBytes), 0);
        for (size_t index = 0; index < fixture.media.size(); ++index) {
            fixture.media[index] = static_cast<uint8_t>((index * 7 + 3) & 0xFF);
        }
        fixture.device.media = &fixture.media;
        AddRegion(fixture.device, kAdminSqPagePhysical, fixture.adminSqPage, sizeof(fixture.adminSqPage));
        AddRegion(fixture.device, kAdminCqPagePhysical, fixture.adminCqPage, sizeof(fixture.adminCqPage));
        AddRegion(fixture.device, kIoPagePhysical, fixture.ioPage, sizeof(fixture.ioPage));
        AddRegion(fixture.device, kIoCqPagePhysical, fixture.ioCqPage, sizeof(fixture.ioCqPage));
        AddRegion(fixture.device, kDataPagePhysical, fixture.dataPage, sizeof(fixture.dataPage));
        SetCapability(fixture.device, MakeCapability(fixture.device));
        fixture.device.registers[Nvme::kRegVersion / 4] = fixture.device.version;
        fixture.controller = Nvme::Controller{};
        fixture.controller.registers.read = &FakeRegisterRead;
        fixture.controller.registers.write = &FakeRegisterWrite;
        fixture.controller.registers.context = &fixture.device;
        fixture.controller.adminSq = fixture.adminSqPage;
        fixture.controller.adminCq = fixture.adminCqPage;
        fixture.controller.ioSq = fixture.ioPage;
        fixture.controller.ioCq = fixture.ioCqPage;
        fixture.controller.data = fixture.dataPage;
        fixture.controller.adminSqPhysical = kAdminSqPagePhysical;
        fixture.controller.adminCqPhysical = kAdminCqPagePhysical;
        fixture.controller.ioSqPhysical = kIoPagePhysical;
        fixture.controller.ioCqPhysical = kIoCqPagePhysical;
        fixture.controller.dataPhysical = kDataPagePhysical;
        fixture.controller.pageBytes = Nvme::kPageBytes;
    }

    // 初始化成功并返回控制器
    void InitOk(Fixture& fixture) {
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::Ok, "the controller must initialize");
    }

    // 读一段介质并核对内容与伪介质一致
    void ReadAndCheck(Fixture& fixture, uint64_t offset, uint32_t bytes) {
        std::vector<uint8_t> buffer(bytes, 0xEE);
        Require(Nvme::Read(fixture.controller, offset, buffer.data(), bytes) == Storage::Error::Ok, "the read must succeed");
        Require(std::memcmp(buffer.data(), fixture.media.data() + offset, bytes) == 0, "the read data must match the media");
    }

    // —— 初始化用例 ——
    // 正常初始化：CAP/VS 校验、CC 使能、AQA/ASQ/ACQ、Identify 两条、创建 I/O 队列两条
    void CaseInitialize() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        Require(fixture.controller.enabled == 1, "the controller must be enabled");
        Require(fixture.device.ccWrites >= 2, "CC must be written at least twice");
        Require((fixture.device.lastCc & Nvme::kCcEnable) != 0, "the last CC write must enable the controller");
        Require(((fixture.device.lastCc >> Nvme::kCcIosqesShift) & 0xFu) == Nvme::kCcIosqesBytes, "the I/O SQE size is wrong");
        Require(((fixture.device.lastCc >> Nvme::kCcIocqesShift) & 0xFu) == Nvme::kCcIocqesBytes, "the I/O CQE size is wrong");
        Require(fixture.device.adminDepth == Nvme::kAdminQueueMaxEntries, "the admin queue depth must be capped by the module");
        Require(fixture.device.ioDepth == Nvme::kIoQueueMaxEntries, "the I/O queue depth must be capped by the module");
        Require(fixture.device.aqaValue == (((Nvme::kAdminQueueMaxEntries - 1) << Nvme::kAqaAsqsShift)
            | ((Nvme::kAdminQueueMaxEntries - 1) << Nvme::kAqaAcqsShift)), "AQA is wrong");
        Require(fixture.device.asqValue == kAdminSqPagePhysical, "ASQ is wrong");
        Require(fixture.device.acqValue == kAdminCqPagePhysical, "ACQ is wrong");
        Require(fixture.device.adminSq == fixture.adminSqPage && fixture.device.adminCq == fixture.adminCqPage,
            "the admin queues must be page aligned");
        Require(fixture.device.ioSq == fixture.ioPage && fixture.device.ioCq == fixture.ioCqPage,
            "the I/O queues must be page aligned");
        Require(fixture.device.identifyCount == 2, "the controller must issue two Identify commands");
        Require(fixture.device.createCqCount == 1 && fixture.device.createSqCount == 1, "the I/O queue pair must be created");
        Require(fixture.device.sqTailDoorbells >= 4, "the submit queue doorbell count is wrong");
        Require(fixture.device.cqHeadDoorbells >= 4, "the completion queue doorbell count is wrong");
    }

    // Identify 命令编码：CNS 与 NSID、PRP1 指向数据页
    void CaseIdentifyEncoding() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        const uint32_t controllerIndex = FindCaptured(fixture.device, Nvme::kOpcodeIdentify);
        Require((CapturedWord(fixture.device, controllerIndex, 10) & 0xFFu) == Nvme::kIdentifyController, "the controller Identify CNS is wrong");
        Require(CapturedWord(fixture.device, controllerIndex, 1) == 0, "the controller Identify NSID must be zero");
        Require(Nvme::Combine64(CapturedWord(fixture.device, controllerIndex, 6), CapturedWord(fixture.device, controllerIndex, 7))
            == kDataPagePhysical, "the Identify PRP1 is wrong");
        uint32_t namespaceIndex = 0;
        bool found = false;
        for (uint32_t index = 0; index < fixture.device.capturedCount; ++index) {
            if ((fixture.device.capturedSqe[index][0] & 0xFFu) != Nvme::kOpcodeIdentify) continue;
            if ((fixture.device.capturedSqe[index][10] & 0xFFu) != Nvme::kIdentifyNamespace) continue;
            namespaceIndex = index;
            found = true;
            break;
        }
        Require(found, "the namespace Identify command is missing");
        Require(CapturedWord(fixture.device, namespaceIndex, 1) == Nvme::kNamespaceId, "the namespace Identify NSID is wrong");
    }

    // 创建队列命令编码：深度、PC 与 CQID
    void CaseCreateQueueEncoding() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        const uint32_t cqIndex = FindCaptured(fixture.device, Nvme::kOpcodeCreateCq);
        const uint32_t cqCdw10 = CapturedWord(fixture.device, cqIndex, 10);
        Require((cqCdw10 & 0xFFFFu) == Nvme::kIoQueueId, "the completion queue id is wrong");
        Require(((cqCdw10 >> 16) & 0xFFFFu) == Nvme::kIoQueueMaxEntries - 1, "the completion queue size is wrong");
        Require((CapturedWord(fixture.device, cqIndex, 11) & Nvme::kQueuePhysicallyContiguous) != 0, "PC must be set");
        const uint32_t sqIndex = FindCaptured(fixture.device, Nvme::kOpcodeCreateSq);
        const uint32_t sqCdw11 = CapturedWord(fixture.device, sqIndex, 11);
        Require((sqCdw11 & Nvme::kQueuePhysicallyContiguous) != 0, "the submit queue PC must be set");
        Require(((sqCdw11 >> Nvme::kQueueCompletionIdShift) & 0xFFFFu) == Nvme::kIoQueueId, "the submit queue CQID is wrong");
        // 队列基址必须随创建命令的 PRP1 给出：不给时设备会在错误的位置取命令
        Require(Nvme::Combine64(CapturedWord(fixture.device, cqIndex, 6), CapturedWord(fixture.device, cqIndex, 7))
            == kIoCqPagePhysical, "the completion queue base must be carried in PRP1");
        Require(Nvme::Combine64(CapturedWord(fixture.device, sqIndex, 6), CapturedWord(fixture.device, sqIndex, 7))
            == kIoPagePhysical, "the submit queue base must be carried in PRP1");
        Require(CapturedWord(fixture.device, cqIndex, 1) == 0 && CapturedWord(fixture.device, sqIndex, 1) == 0,
            "the create queue NSID must be reserved");
    }

    // VS 主版本不受支持：拒绝
    void CaseVersionUnsupported() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.version = 0x00020000;
        fixture.device.registers[Nvme::kRegVersion / 4] = fixture.device.version;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a 2.x version must be rejected");
    }

    // NVM 命令集缺失：拒绝
    void CaseCommandSetMissing() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.nvmCommandSet = false;
        SetCapability(fixture.device, MakeCapability(fixture.device));
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a controller without NVM must be rejected");
    }

    // RDY 不就绪：初始化超时
    void CaseReadyTimeout() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.rdyOnEnable = false;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::Timeout, "a controller that never becomes ready must time out");
    }

    // 队列深度不足：MQES 为 0 时拒绝
    void CaseQueueTooSmall() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.mqes = 0;
        SetCapability(fixture.device, MakeCapability(fixture.device));
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a one-entry queue must be rejected");
    }

    // 最小内存页大于调用方页：拒绝
    void CasePageSizeMismatch() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.mpsmin = 1;
        SetCapability(fixture.device, MakeCapability(fixture.device));
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "an 8 KiB minimum page must be rejected");
    }

    // 队列条目大小不足：SQES 最大值小于 64 字节时拒绝
    void CaseEntrySizeUnsupported() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.sqesMax = 4;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a 16-byte SQE must be rejected");
    }

    // 初始化入口的指针校验：队列或寄存器缺失时拒绝
    void CaseInitializeBadPointers() {
        Fixture fixture;
        Setup(fixture);
        fixture.controller.adminSq = nullptr;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a missing admin queue must be rejected");
        Setup(fixture);
        fixture.controller.dataPhysical = 0;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a missing data address must be rejected");
        Setup(fixture);
        fixture.controller.registers.read = nullptr;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "missing registers must be rejected");
        Setup(fixture);
        fixture.controller.ioCqPhysical = kIoCqPagePhysical + 0x10;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a misaligned queue base must be rejected");
    }

    // 完成延迟到达：伪设备不在门铃里写完成，由测试线程睡 2ms 后写入
    // 轮询必须每圈重新读完成队列，否则会读到首次的全零并一直超时
    void CaseDeferredCompletion() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        fixture.device.deferNext = 1;
        std::thread writer(&DeferredWriter);
        std::vector<uint8_t> buffer(4096, 0);
        // 先 join 再断言：用例失败时线程仍可 join，不会在析构里 terminate
        const Storage::Error readError = Nvme::Read(fixture.controller, 0, buffer.data(), 4096);
        writer.join();
        Require(readError == Storage::Error::Ok, "a late completion must still be observed");
        Require(std::memcmp(buffer.data(), fixture.media.data(), 4096) == 0, "the late read data must match the media");
        Require(g_deferReady.load(std::memory_order_acquire) == 0, "the deferred completion must be consumed");
    }

    // —— Identify 解析用例 ——
    // 4Kn：LBADS 12 → 逻辑块 4096，块数与容量随 NSZE
    void CaseIdentify4Kn() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.lbads = 12;
        InitOk(fixture);
        Require(fixture.controller.lbaSize == 4096, "the 4Kn logical block size is wrong");
        Require(fixture.controller.blocks == fixture.device.nsze, "the namespace size is wrong");
        Require(std::string(fixture.controller.model) == "QEMU HARDDISK", "the model string is wrong");
        Require(Nvme::kOps.BlockBytes(&fixture.controller) == 4096, "the ops block size must be 4096");
        Require(Nvme::kOps.CapacityBytes(&fixture.controller) == fixture.device.nsze * 4096, "the ops capacity is wrong");
    }

    // 512e：LBADS 9 → 逻辑块 512
    void CaseIdentify512e() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.lbads = 9;
        InitOk(fixture);
        Require(fixture.controller.lbaSize == 512, "the 512e logical block size is wrong");
        Require(Nvme::kOps.BlockBytes(&fixture.controller) == 512, "the ops block size must be 512");
    }

    // 型号去尾空格、MDTS 解析
    void CaseIdentifyModelAndMdts() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.model = "QEMU HARDDISK   ";
        fixture.device.mdts = 2;
        InitOk(fixture);
        Require(std::string(fixture.controller.model) == "QEMU HARDDISK", "the trailing spaces must be trimmed");
        Require(fixture.controller.maxTransferBytes == 4 * Nvme::kPageBytes, "MDTS must cap the transfer at 16 KiB");
        Fixture unlimited;
        Setup(unlimited);
        unlimited.device.mdts = 0;
        InitOk(unlimited);
        Require(unlimited.controller.maxTransferBytes == 0, "MDTS zero must mean no limit");
    }

    // 容量为零：拒绝
    void CaseNamespaceZero() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.nsze = 0;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::Corrupt, "a zero namespace must be rejected");
    }

    // LBA 格式下标越界：拒绝
    void CaseLbaFormatIndexInvalid() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.flbas = 3;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::Corrupt, "an out-of-range LBA format must be rejected");
    }

    // LBA 大小不受支持：1024 字节拒绝
    void CaseLbaSizeUnsupported() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.lbads = 10;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::NotSupported, "a 1024-byte LBA must be rejected");
    }

    // 命名空间数量为零：拒绝
    void CaseNoNamespaces() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.namespaces = 0;
        Require(Nvme::Initialize(fixture.controller) == Nvme::Result::Corrupt, "a controller without namespaces must be rejected");
    }

    // —— 读路径用例 ——
    // 读命令编码：opcode / NSID / PRP1 / SLBA / NLB 与命令标识递增
    void CaseReadEncoding() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        Require(Nvme::Read(fixture.controller, 4096, fixture.dataPage, 4096) == Storage::Error::Ok, "the read must succeed");
        const uint32_t index = FindCaptured(fixture.device, Nvme::kOpcodeRead);
        Require(CapturedWord(fixture.device, index, 1) == Nvme::kNamespaceId, "the read NSID is wrong");
        Require(Nvme::Combine64(CapturedWord(fixture.device, index, 6), CapturedWord(fixture.device, index, 7)) == kDataPagePhysical,
            "the read PRP1 is wrong");
        Require(CapturedWord(fixture.device, index, 10) == 1, "the read SLBA low is wrong");
        Require(CapturedWord(fixture.device, index, 11) == 0, "the read SLBA high is wrong");
        Require((CapturedWord(fixture.device, index, 12) & 0xFFFFu) == 0, "the read NLB must be zero based");
        const uint16_t firstId = static_cast<uint16_t>(CapturedWord(fixture.device, index, 0) >> 16);
        Require(Nvme::Read(fixture.controller, 0, fixture.dataPage, 4096) == Storage::Error::Ok, "the second read must succeed");
        uint32_t second = 0;
        for (uint32_t step = index + 1; step < fixture.device.capturedCount; ++step) {
            if ((fixture.device.capturedSqe[step][0] & 0xFFu) == Nvme::kOpcodeRead) {
                second = step;
                break;
            }
        }
        Require(second != 0, "the second read command is missing");
        const uint16_t secondId = static_cast<uint16_t>(CapturedWord(fixture.device, second, 0) >> 16);
        Require(firstId != secondId, "command ids must increase");
    }

    // 读数据与伪介质一致：4Kn 与 512e 两档
    void CaseReadData() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        ReadAndCheck(fixture, 0, 4096);
        ReadAndCheck(fixture, 4096, 8192);
        Fixture small;
        Setup(small);
        small.device.lbads = 9;
        InitOk(small);
        ReadAndCheck(small, 512, 1024);
    }

    // 跨页读：按数据弹跳页分块，每条命令一个 PRP1
    void CaseReadChunking() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        ReadAndCheck(fixture, 0, 3 * 4096);
        Require(fixture.device.readCount == 3, "a three-page read must issue three commands");
    }

    // MDTS 解析与分块：单条命令只用一个 PRP1，因此命令按数据页分块
    void CaseReadMdtsLimit() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.mdts = 1;
        InitOk(fixture);
        Require(fixture.controller.maxTransferBytes == 2 * Nvme::kPageBytes, "MDTS 1 must parse to 8 KiB");
        ReadAndCheck(fixture, 0, 3 * 4096);
        Require(fixture.device.readCount == 3, "single-PRP1 chunking must issue one command per data page");
    }

    // 阶段位翻转：队列深度 4 时连续五条读命令绕圈
    void CasePhaseFlip() {
        Fixture fixture;
        Setup(fixture);
        fixture.device.mqes = 3;
        SetCapability(fixture.device, MakeCapability(fixture.device));
        InitOk(fixture);
        Require(fixture.controller.ioDepth == 4, "the I/O queue depth must follow CAP.MQES");
        for (uint32_t index = 0; index < 5; ++index) {
            ReadAndCheck(fixture, static_cast<uint64_t>(index) * 4096, 4096);
        }
        Require(fixture.device.readCount == 5, "five reads must issue five commands");
    }

    // 命令永不完成：读超时映射成 I/O 错误
    void CaseReadTimeout() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        fixture.device.completeCommands = false;
        std::vector<uint8_t> buffer(4096, 0);
        Require(Nvme::Read(fixture.controller, 0, buffer.data(), 4096) == Storage::Error::Io, "a timeout must map to an I/O error");
    }

    // 错误状态码映射：逐个状态码核对
    void CaseStatusMapping() {
        Require(Nvme::MapStatus(Nvme::kStatusSuccess) == Storage::Error::Ok, "success must map to Ok");
        Require(Nvme::MapStatus(Nvme::kStatusInvalidOpcode) == Storage::Error::NotSupported, "invalid opcode must map to NotSupported");
        Require(Nvme::MapStatus(Nvme::kStatusInvalidField) == Storage::Error::InvalidArgument, "invalid field must map to InvalidArgument");
        Require(Nvme::MapStatus(Nvme::kStatusDataTransfer) == Storage::Error::Io, "data transfer error must map to Io");
        Require(Nvme::MapStatus(Nvme::kStatusInternal) == Storage::Error::Io, "internal error must map to Io");
        Require(Nvme::MapStatus(Nvme::kStatusLbaOutOfRange) == Storage::Error::InvalidArgument, "LBA out of range must map to InvalidArgument");
        Require(Nvme::MapStatus(Nvme::kStatusCapacityExceeded) == Storage::Error::InvalidArgument, "capacity exceeded must map to InvalidArgument");
        Require(Nvme::MapStatus(Nvme::kStatusNamespaceNotReady) == Storage::Error::NotSupported, "namespace not ready must map to NotSupported");
        Require(Nvme::MapStatus(Nvme::kStatusReservationConflict) == Storage::Error::ReadOnly, "reservation conflict must map to ReadOnly");
        Require(Nvme::MapStatus(Nvme::kStatusFormatInProgress) == Storage::Error::Io, "format in progress must map to Io");
        Require(Nvme::MapStatus(Nvme::kStatusWriteToReadOnly) == Storage::Error::ReadOnly, "write to read only must map to ReadOnly");
        Require(Nvme::MapStatus(0x7F) == Storage::Error::Io, "an unknown status must map to Io");
    }

    // 注入错误状态：读返回映射后的错误
    void CaseReadInjectedStatus() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        std::vector<uint8_t> buffer(4096, 0);
        fixture.device.statusOverride = Nvme::kStatusInvalidField;
        Require(Nvme::Read(fixture.controller, 0, buffer.data(), 4096) == Storage::Error::InvalidArgument,
            "an injected invalid-field status must surface");
        fixture.device.statusOverride = Nvme::kStatusWriteToReadOnly;
        Require(Nvme::Read(fixture.controller, 0, buffer.data(), 4096) == Storage::Error::ReadOnly,
            "an injected read-only status must surface");
        Require(fixture.device.statusOverride == 0xFF, "the injected status must be consumed once");
    }

    // 参数与范围校验：对齐、越界、空指针与未初始化
    void CaseReadArguments() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        std::vector<uint8_t> buffer(8192, 0);
        Require(Nvme::Read(fixture.controller, 0, nullptr, 4096) == Storage::Error::InvalidArgument, "a null buffer must be rejected");
        Require(Nvme::Read(fixture.controller, 0, buffer.data(), 0) == Storage::Error::InvalidArgument, "a zero length must be rejected");
        Require(Nvme::Read(fixture.controller, 512, buffer.data(), 4096) == Storage::Error::InvalidArgument, "an unaligned offset must be rejected");
        Require(Nvme::Read(fixture.controller, 0, buffer.data(), 1000) == Storage::Error::InvalidArgument, "an unaligned length must be rejected");
        Require(Nvme::Read(fixture.controller, fixture.device.nsze * 4096, buffer.data(), 4096) == Storage::Error::InvalidArgument,
            "a read past the namespace must be rejected");
        Require(Nvme::Read(fixture.controller, (fixture.device.nsze - 1) * 4096, buffer.data(), 8192) == Storage::Error::InvalidArgument,
            "a read crossing the namespace end must be rejected");
        Nvme::Controller uninitialized{};
        Require(Nvme::Read(uninitialized, 0, buffer.data(), 4096) == Storage::Error::NotSupported, "an uninitialized controller must be rejected");
    }

    // 块设备调用表：容量、可写标志、写只读、刷新成功
    void CaseOpsTable() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        Require(Nvme::kOps.capabilities == 0, "the capability bits must be zero");
        Require(Nvme::kOps.Writable(&fixture.controller) == 1, "a physical disk must be writable");
        Require(Nvme::kOps.Write(&fixture.controller, 0, fixture.dataPage, 4096) == Storage::Error::ReadOnly, "writes must report read only");
        Require(Nvme::kOps.Flush(&fixture.controller) == Storage::Error::Ok, "flush must succeed");
        ReadAndCheck(fixture, 0, 4096);
    }

    // 标识行格式：型号、块数与块大小
    void CaseDescribeDevice() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        char line[96] = {};
        const uint32_t length = Nvme::DescribeDevice(fixture.controller, line, sizeof(line));
        Require(std::string(line) == "nvme: QEMU HARDDISK sectors=0x400000 block=0x1000",
            std::string("the device line is wrong: ") + line);
        Require(length == std::string(line).size(), "the reported length must match the text");
        Fixture small;
        Setup(small);
        small.device.lbads = 9;
        InitOk(small);
        char smallLine[96] = {};
        (void)Nvme::DescribeDevice(small.controller, smallLine, sizeof(smallLine));
        Require(std::string(smallLine) == "nvme: QEMU HARDDISK sectors=0x400000 block=0x200",
            std::string("the 512e device line is wrong: ") + smallLine);
    }

    // 停机：删除 I/O 队列、清 CC.EN，停机后读返回不支持
    void CaseShutdown() {
        Fixture fixture;
        Setup(fixture);
        InitOk(fixture);
        ReadAndCheck(fixture, 0, 4096);
        Nvme::Shutdown(fixture.controller);
        Require(fixture.device.deleteSqCount == 1 && fixture.device.deleteCqCount == 1, "shutdown must delete the I/O queue pair");
        Require((fixture.device.lastCc & Nvme::kCcEnable) == 0, "shutdown must clear CC.EN");
        Require(fixture.controller.enabled == 0, "the controller must be marked disabled");
        std::vector<uint8_t> buffer(4096, 0);
        Require(Nvme::Read(fixture.controller, 0, buffer.data(), 4096) == Storage::Error::NotSupported, "a disabled controller must be rejected");
        Nvme::Shutdown(fixture.controller);
        Require(fixture.device.deleteSqCount == 1, "a second shutdown must not delete queues again");
    }

    // 夹具自检：寄存器与队列常量与生成头一致
    void CaseContractFacts() {
        Require(Nvme::kSqeBytes == 64 && Nvme::kCqeBytes == 16, "the queue entry sizes are wrong");
        Require(Nvme::kRegDoorbell == 0x1000, "the doorbell base is wrong");
        Require(Nvme::kCqeStatusShift == 17, "the completion status shift is wrong");
        Require(Nvme::kOpcodeRead == 0x02 && Nvme::kOpcodeIdentify == 0x06, "the command opcodes are wrong");
        Require(Nvme::kCtrlSqes == 0x200 && Nvme::kCtrlNamespaces == 0x204, "the Identify Controller offsets are wrong");
        Require(Nvme::kNsFormattedLbaSize == 0x1A && Nvme::kNsLbaFormatCountField == 0x19 && Nvme::kNsLbaFormats == 0x80,
            "the Identify Namespace offsets are wrong");
    }

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // —— 初始化与寄存器 ——
        cases.push_back({ "初始化：CAP/VS、CC 使能、AQA/ASQ/ACQ 与队列创建", CaseInitialize });
        cases.push_back({ "初始化：Identify 命令编码", CaseIdentifyEncoding });
        cases.push_back({ "初始化：创建 I/O 队列命令编码", CaseCreateQueueEncoding });
        cases.push_back({ "初始化：VS 主版本不受支持", CaseVersionUnsupported });
        cases.push_back({ "初始化：缺 NVM 命令集", CaseCommandSetMissing });
        cases.push_back({ "初始化：RDY 不就绪超时", CaseReadyTimeout });
        cases.push_back({ "初始化：队列深度不足", CaseQueueTooSmall });
        cases.push_back({ "初始化：最小内存页不匹配", CasePageSizeMismatch });
        cases.push_back({ "初始化：队列条目大小不足", CaseEntrySizeUnsupported });
        cases.push_back({ "初始化：指针缺失", CaseInitializeBadPointers });
        // —— Identify 解析 ——
        cases.push_back({ "Identify：4Kn 逻辑块与容量", CaseIdentify4Kn });
        cases.push_back({ "Identify：512e 逻辑块", CaseIdentify512e });
        cases.push_back({ "Identify：型号去尾空格与 MDTS", CaseIdentifyModelAndMdts });
        cases.push_back({ "Identify：容量为零拒绝", CaseNamespaceZero });
        cases.push_back({ "Identify：LBA 格式下标越界", CaseLbaFormatIndexInvalid });
        cases.push_back({ "Identify：LBA 大小不受支持", CaseLbaSizeUnsupported });
        cases.push_back({ "Identify：命名空间数量为零", CaseNoNamespaces });
        // —— 读路径 ——
        cases.push_back({ "读：命令编码与命令标识递增", CaseReadEncoding });
        cases.push_back({ "读：4Kn 与 512e 数据一致", CaseReadData });
        cases.push_back({ "读：跨页分块", CaseReadChunking });
        cases.push_back({ "读：MDTS 限制分块", CaseReadMdtsLimit });
        cases.push_back({ "读：阶段位绕圈翻转", CasePhaseFlip });
        cases.push_back({ "读：完成延迟到达仍被观察到", CaseDeferredCompletion });
        cases.push_back({ "读：命令超时映射 I/O 错误", CaseReadTimeout });
        cases.push_back({ "读：状态码映射表", CaseStatusMapping });
        cases.push_back({ "读：注入错误状态", CaseReadInjectedStatus });
        cases.push_back({ "读：参数与范围校验", CaseReadArguments });
        cases.push_back({ "块设备：ops 表语义", CaseOpsTable });
        cases.push_back({ "块设备：标识行格式", CaseDescribeDevice });
        cases.push_back({ "停机：删除队列与清使能", CaseShutdown });
        // —— 常量自检 ——
        cases.push_back({ "常量：队列与 Identify 偏移", CaseContractFacts });
        return cases;
    }
}

// 入口：跑全部用例并汇总；失败返回 1
int main() {
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckNvme: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckNvme: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << " [ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckNvme: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
