/* NvmeRegisters.hpp
    NVMe 控制器寄存器、队列条目与 Identify 数据的字节表：偏移与位域的唯一来源

    寄存器与命令编码按 NVMe 1.4 规范：CAP/VS/CC/CSTS/AQA/ASQ/ACQ、Admin 与 I/O 队列
    条目（SQE 64 字节、CQE 16 字节）、门铃布局（0x1000 起、步长 4 << CAP.DSTRD）、
    命令操作码与状态码；Identify Controller 的 SQES/NN 偏移与 Identify Namespace 的
    LBAF 表（128..191，每项 4 字节，LBADS 在 bit23..16）经 Linux nvme.h / NVMe 规范核对
    本文件只有常量与内联小工具，不含逻辑，宿主夹具与目标侧共用同一份字节表
*/

#pragma once
#include <stdint.h>

namespace Nvme {
    // —— 控制器寄存器偏移（BAR0 内） ——
    constexpr uint32_t kRegCap = 0x00;        // 控制器能力，64 位
    constexpr uint32_t kRegVersion = 0x08;    // 版本，32 位
    constexpr uint32_t kRegIntMaskSet = 0x0C; // 中断屏蔽设置
    constexpr uint32_t kRegIntMaskClear = 0x10; // 中断屏蔽清除
    constexpr uint32_t kRegConfig = 0x14;     // CC 控制器配置
    constexpr uint32_t kRegStatus = 0x1C;     // CSTS 控制器状态
    constexpr uint32_t kRegAqa = 0x24;        // Admin 队列属性
    constexpr uint32_t kRegAsq = 0x28;        // Admin 提交队列基址，64 位
    constexpr uint32_t kRegAcq = 0x30;        // Admin 完成队列基址，64 位
    constexpr uint32_t kRegDoorbell = 0x1000; // 门铃区起点

    // —— CAP 位域 ——
    constexpr uint32_t kCapMqesShift = 0;      // 队列条目上限，0 基
    constexpr uint32_t kCapMqesMask = 0xFFFFu;
    constexpr uint32_t kCapToShift = 24;       // 超时，单位 500ms
    constexpr uint32_t kCapToMask = 0xFFu;
    constexpr uint32_t kCapDstrdShift = 32;    // 门铃步长指数：步长 = 4 << DSTRD
    constexpr uint32_t kCapDstrdMask = 0xFu;
    constexpr uint32_t kCapMpsminShift = 48;   // 最小内存页大小指数：页 = 1 << (12 + MPSMIN)
    constexpr uint32_t kCapMpsminMask = 0xFu;

    // —— VS 位域 ——
    constexpr uint32_t kVersionMajorShift = 16;
    constexpr uint32_t kVersionMajorMask = 0xFFFFu;

    // —— CC 位域 ——
    constexpr uint32_t kCcEnable = 1u << 0;        // EN
    constexpr uint32_t kCcCssShift = 4;            // 命令集：0 = NVM
    constexpr uint32_t kCcMpsShift = 7;            // 内存页大小：0 = 4KiB
    constexpr uint32_t kCcIosqesShift = 16;        // I/O 提交队列条目大小：6 = 64 字节
    constexpr uint32_t kCcIocqesShift = 20;        // I/O 完成队列条目大小：4 = 16 字节
    constexpr uint32_t kCcIosqesBytes = 6;         // 2^6 = 64
    constexpr uint32_t kCcIocqesBytes = 4;         // 2^4 = 16

    // —— CSTS 位域 ——
    constexpr uint32_t kCstsReady = 1u << 0;       // RDY
    constexpr uint32_t kCstsFatal = 1u << 1;       // CFS

    // —— AQA 位域 ——
    constexpr uint32_t kAqaAsqsShift = 0;          // Admin 提交队列深度，0 基
    constexpr uint32_t kAqaAsqsMask = 0xFFFu;
    constexpr uint32_t kAqaAcqsShift = 16;         // Admin 完成队列深度，0 基
    constexpr uint32_t kAqaAcqsMask = 0xFFFu;

    // —— 队列条目 ——
    constexpr uint32_t kSqeBytes = 64;             // 提交队列条目
    constexpr uint32_t kCqeBytes = 16;             // 完成队列条目
    // SQE 字段偏移
    constexpr uint32_t kSqeOpcode = 0x00;          // 操作码在低 8 位
    constexpr uint32_t kSqeCommandId = 0x02;       // 命令标识，u16
    constexpr uint32_t kSqeNamespace = 0x04;       // NSID，u32
    constexpr uint32_t kSqePrp1 = 0x18;            // 数据指针 1，u64
    constexpr uint32_t kSqePrp2 = 0x20;            // 数据指针 2，u64
    constexpr uint32_t kSqeCdw10 = 0x28;           // 命令双字 10
    constexpr uint32_t kSqeCdw11 = 0x2C;           // 命令双字 11
    constexpr uint32_t kSqeCdw12 = 0x30;           // 命令双字 12
    // CQE 字段偏移
    constexpr uint32_t kCqeResult = 0x00;          // 命令相关结果
    constexpr uint32_t kCqeSqHead = 0x08;          // SQHD 在低 16 位，SQID 在高 16 位
    constexpr uint32_t kCqeStatus = 0x0C;          // CID 低 16 位、P bit16、状态 bit31..17
    constexpr uint32_t kCqePhase = 1u << 16;       // 阶段位
    constexpr uint32_t kCqeStatusShift = 17;       // 状态字段起点
    constexpr uint32_t kCqeStatusMask = 0xFFu;     // 状态字段宽度

    // —— 命令操作码 ——
    constexpr uint8_t kOpcodeDeleteSq = 0x00;      // 删除 I/O 提交队列
    constexpr uint8_t kOpcodeCreateSq = 0x01;      // 创建 I/O 提交队列
    constexpr uint8_t kOpcodeRead = 0x02;          // 读
    constexpr uint8_t kOpcodeDeleteCq = 0x04;      // 删除 I/O 完成队列
    constexpr uint8_t kOpcodeCreateCq = 0x05;      // 创建 I/O 完成队列
    constexpr uint8_t kOpcodeIdentify = 0x06;      // Identify

    // —— Identify 的 CNS 取值 ——
    constexpr uint32_t kIdentifyNamespace = 0;     // 命名空间
    constexpr uint32_t kIdentifyController = 1;    // 控制器

    // —— 命令状态码（CQE 状态字段） ——
    constexpr uint8_t kStatusSuccess = 0x00;
    constexpr uint8_t kStatusInvalidOpcode = 0x01;
    constexpr uint8_t kStatusInvalidField = 0x02;
    constexpr uint8_t kStatusDataTransfer = 0x04;
    constexpr uint8_t kStatusInternal = 0x06;
    constexpr uint8_t kStatusAbortRequested = 0x07;
    constexpr uint8_t kStatusLbaOutOfRange = 0x10;
    constexpr uint8_t kStatusCapacityExceeded = 0x11;
    constexpr uint8_t kStatusNamespaceNotReady = 0x12;
    constexpr uint8_t kStatusReservationConflict = 0x13;
    constexpr uint8_t kStatusFormatInProgress = 0x14;
    constexpr uint8_t kStatusWriteToReadOnly = 0x80;

    // —— 创建队列的 CDW11 位 ——
    constexpr uint32_t kQueuePhysicallyContiguous = 1u << 0;  // PC
    constexpr uint32_t kQueueInterruptEnabled = 1u << 1;      // IEN
    constexpr uint32_t kQueueCompletionIdShift = 16;          // CQID

    // —— Identify Controller 字段偏移（NVMe 1.4） ——
    constexpr uint32_t kCtrlSerial = 0x04;         // SN，20 字节
    constexpr uint32_t kCtrlSerialBytes = 20;
    constexpr uint32_t kCtrlModel = 0x18;          // MN，40 字节
    constexpr uint32_t kCtrlModelBytes = 40;
    constexpr uint32_t kCtrlFirmware = 0x40;       // FR，8 字节
    constexpr uint32_t kCtrlFirmwareBytes = 8;
    constexpr uint32_t kCtrlMdts = 0x4D;           // MDTS：单命令上限 = 2^MDTS × 最小页
    constexpr uint32_t kCtrlSqes = 0x200;          // SQES：bit7..4 最大值、bit3..0 最小值
    constexpr uint32_t kCtrlCqes = 0x201;          // CQES
    constexpr uint32_t kCtrlNamespaces = 0x204;    // NN，命名空间数量

    // —— Identify Namespace 字段偏移 ——
    constexpr uint32_t kNsSize = 0x00;             // NSZE，逻辑块数，u64
    constexpr uint32_t kNsCapacity = 0x08;         // NCAP，u64
    constexpr uint32_t kNsFormattedLbaSize = 0x1A; // FLBAS：bit3..0 是当前格式下标
    constexpr uint32_t kNsLbaFormatCountField = 0x19; // NLBAF：支持的 LBA 格式数，0 基
    constexpr uint32_t kNsLbaFormats = 0x80;       // LBAF[16]，每项 4 字节
    constexpr uint32_t kNsLbaFormatBytes = 4;
    constexpr uint32_t kNsLbaFormatCount = 16;
    constexpr uint32_t kLbaFormatDataSizeShift = 16;  // LBADS 在项内 bit23..16

    // —— 其他固定值 ——
    constexpr uint32_t kPageBytes = 0x1000;        // MPS=0 的最小内存页
    constexpr uint32_t kIdentifyBytes = 0x1000;    // Identify 返回一页数据
    constexpr uint32_t kNamespaceId = 1;           // 当前只使用 1 号命名空间
    constexpr uint32_t kAdminQueueId = 0;          // Admin 队列对编号
    constexpr uint32_t kIoQueueId = 1;             // I/O 队列对编号
    constexpr uint32_t kAdminQueueMaxEntries = 16; // Admin 队列深度上限
    constexpr uint32_t kIoQueueMaxEntries = 64;    // I/O 队列深度上限
    constexpr uint32_t kMinQueueEntries = 2;       // 至少两条才能区分空与满

    // 从 64 位寄存器的高低位拼出完整值
    inline uint64_t Combine64(uint32_t low, uint32_t high) {
        return static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
    }
}
