/* DeviceModel.hpp
    DevModel 模块的设备记录与 Dev.Model v1 六槽实现：枚举结果、资源、认领、启动候选与绑定

    平台能力经 Platform 注入：PCI 配置访问、BootInfo 与物理地址换算、日志；宿主检查用
    PCI 内存配置空间与假 BootInfo 替身，目标侧由 Module.cpp 接到 Core.Boot / Core.Log
    与 PciPorts.cpp 的 0xCF8/0xCFC 访问，本文件不直接执行 IN/OUT，也不包含内核私有头
    启动线索规则：BIOS 驱动器号线索按启动介质过滤存储控制器子类（IDE / SATA /
    NVMe 可作硬盘，IDE / SATA 可作光盘）；UEFI 设备路径当前不解析、BootInfo 未提供线索
    或线索不可读时退回「全部大容量存储控制器」并报告，不猜测具体设备，最终身份由卷标识核对
    绑定事实的三种模式（契约 v1 没有单独的绑定登记槽，Binding 槽承载三种模式）：
    facts->bytes 为 0 是查询（调用方零初始化）、为 sizeof(BindingFacts) 且 state 非零是登记 /
    更新、state 为 0 是清除；写侧只由 DriverLoader 调用，查询侧由 Vfs 等消费模块调用，
    其他长度返回 kInvalidArgument
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

#include "Pci.hpp"

namespace DevModel {
    // 设备记录容量：一条 PCI 功能一条记录，超出即截断并报告
    constexpr uint32_t kMaxDevices = 32;
    // PCI 类码里的存储控制器：class 0x01
    constexpr uint32_t kPciClassStorage = 0x01;
    // 存储控制器子类：IDE、ATA / ATAPI、SATA/AHCI、NVMe
    constexpr uint32_t kPciSubclassIde = 0x01;
    constexpr uint32_t kPciSubclassAta = 0x05;
    constexpr uint32_t kPciSubclassSata = 0x06;
    constexpr uint32_t kPciSubclassNvme = 0x08;
    // 启动介质取值：与 Boot::Media 一致
    constexpr uint32_t kMediaHdd = 2;
    constexpr uint32_t kMediaCdrom = 4;
    // 资源里的中断标志位：bit0 表示中断号来自 PCI 配置空间的中断线
    constexpr uint32_t kIrqFlagPciLine = 0x1;
    // DMA 地址位宽：当前 PCI 存储控制器按 32 位记录，64 位 DMA 属后续阶段
    constexpr uint32_t kDmaAddressBits = 32;
    // 认领标志：v1 只接受 0
    constexpr uint32_t kClaimFlagsNone = 0;

    // 一台设备：契约事实、资源、认领与绑定状态，以及启动候选标记
    struct DeviceRecord {
        DeviceFacts facts = {};         // 设备事实，deviceHandle 在表内唯一
        DeviceResources resources = {}; // BAR / IRQ / DMA 约束
        uint32_t claimed = 0;           // 0 未认领，1 已认领
        uint32_t bound = 0;             // 0 未绑定，1 已绑定
        uint32_t candidate = 0;         // 启动候选：0 不是，1 是
        BindingFacts binding = {};      // 绑定事实，bound 为 1 时有效
    };

    // 平台能力注入：PCI 配置访问、BootInfo、物理地址换算与日志
    struct Platform {
        PciAccess pci;                                                              // PCI 配置空间访问
        const Boot::Info* (*bootInfo)(void* context);                               // Core.Boot 槽 0 的适配
        const void* (*physical)(void* context, uint64_t physical, uint64_t bytes);  // Core.Boot 槽 1 的适配
        void (*logLine)(void* context, const char* tag, const char* text);          // Core.Log 槽 1 的适配
        void* context;                                                              // 透传给四个回调的上下文
    };

    // DevModel 模块状态：设备记录与启动线索事实，全部为定长 POD，无动态初始化
    struct State {
        DeviceRecord devices[kMaxDevices] = {};  // 设备记录
        PciFunction functions[kMaxDevices] = {}; // 枚举中间结果，Init 后不再使用
        uint32_t deviceCount = 0;                // 设备记录数
        uint32_t storageCount = 0;               // 大容量存储控制器数
        uint32_t candidateCount = 0;             // 启动候选数
        uint32_t clueKind = 0;                   // BootInfo.deviceKind
        uint32_t clueMedia = 0;                  // BootInfo.media
        uint32_t clueDrive = 0;                  // BIOS 驱动器号；kNoClueDrive 表示不可读
        uint32_t clueFallback = 0;               // 1 表示线索不可解析、已退回全量枚举
        uint32_t initialized = 0;                // 已初始化标志，重复 Init 幂等
        Platform platform = {};                  // 平台注入
    };

    // BIOS 驱动器号不可读的哨兵
    constexpr uint32_t kNoClueDrive = 0xFFFFFFFF;

    // 初始化：枚举 PCI 设备、填记录与启动候选；失败返回分类，不打印编号让调用方决定
    KernelError Init(State& state, const Platform& platform);
    // 撤销：清空认领与绑定状态；Init 失败与模块回滚都走这里，无外部资源可释放
    void Abort(State& state);

    // Dev.Model 槽 0：枚举设备；cursor 从 0 起，结束后返回 kNotFound
    KernelError Enumerate(State& state, uint32_t* cursor, DeviceFacts* facts);
    // Dev.Model 槽 1：按设备句柄取资源事实
    KernelError Resources(State& state, uint32_t deviceHandle, DeviceResources* resources);
    // Dev.Model 槽 2：认领设备唯一控制权；flags 当前必须为 0，认领句柄即设备句柄
    KernelError Claim(State& state, uint32_t deviceHandle, uint32_t flags, uint32_t* claimHandle);
    // Dev.Model 槽 3：归还控制权；仍有绑定时返回 kBusy，需先 Stop
    KernelError Release(State& state, uint32_t deviceHandle);
    // Dev.Model 槽 4：按启动线索枚举候选设备；线索不可解析时返回全量存储控制器
    KernelError BootCandidates(State& state, uint32_t* cursor, uint32_t* deviceHandle);
    // Dev.Model 槽 5：绑定事实查询（bytes 为 0）与登记 / 清除（bytes 为结构长度）
    KernelError Binding(State& state, uint32_t deviceHandle, BindingFacts* facts);
    // Dev.Model v2 槽 6：把外部枚举发现的设备登记进设备树
    // 校验：facts 非空、bytes 不小于 v1 结构、busKind 非零、classCode 在 24 位内、
    // vendorId / deviceId 在 16 位内；同 busKind + busAddress 已存在返回 kAlreadyExists，
    // 设备表满返回 kOutOfResources；成功后句柄 = 下标 + 1 写回，记录初始未认领、未绑定
    KernelError Register(State& state, const DeviceFacts* facts, uint32_t* deviceHandle);

    // 串口断言用的固定英文标记：baleen-integration 的启动矩阵按整行匹配
    inline constexpr char kMarkerEnumeration[] = "devmodel: pci enumeration done";
    inline constexpr char kMarkerClueMatched[] = "devmodel: boot clue matched";
    inline constexpr char kMarkerClueFallback[] = "devmodel: boot clue fallback to full enumeration";
    inline constexpr char kMarkerTableFull[] = "devmodel: device table is full";
    inline constexpr char kMarkerBootInfoMissing[] = "devmodel: BootInfo is missing";

    // 设备句柄与记录下标互转：句柄 = 下标 + 1，0 永远无效
    inline uint32_t HandleOf(uint32_t index) { return index + 1; }
    // 句柄是否落在当前设备表内
    inline bool ValidHandle(const State& state, uint32_t handle) { return handle != 0 && handle <= state.deviceCount; }
}
