/* main.cpp
    CheckDevModel：DevModel 与 DriverLoader 模块实现的宿主侧检查，手工执行

    检查对象是模块源码，用 vpath 按相对路径直接编进本工具，不复制实现：
      Packages/KernelModules/DevModel/Src/Pci.cpp           PCI 枚举与 BAR 解码
      Packages/KernelModules/DevModel/Src/DeviceModel.cpp   Dev.Model v1 六槽
      Packages/KernelModules/DriverLoader/Src/DriverRegistry.cpp  Drv.Loader v1 四槽
    PciPorts.cpp 与两个 Module.cpp 不参与：前者是 0xCF8/0xCFC 目标侧访问，后者是契约表接线，
    它们的符号与段落在模块 ELF 的 make check 与 PackModule --verify 里核对

    平台侧由替身注入：伪 PCI 配置空间按总线坐标存配置寄存器、用掩码模拟「写全 1 回读」的
    BAR 长度探测；假 BootInfo 与物理地址换算提供启动线索；日志收集器保存固定英文标记；
    DriverLoader 拿到的是指向 DeviceModel 免费函数的 Dev.Model 槽表，与装载器回填后的形态一致

    每个用例打印 [ OK ] 或 [ FAIL ]；用例数固定，末尾与实跑数核对；失败返回 1
*/

#include "DeviceModel.hpp"
#include "DriverRegistry.hpp"
#include "Pci.hpp"

#include <Abi.hpp>
#include <Exports.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // 用例总数：与用例表长度核对，防止用例被误删
    constexpr unsigned kCaseCount = 43;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // —— 伪 PCI 配置空间 ——
    // 一个伪功能：配置寄存器、BAR 原值与写全 1 回读的掩码
    struct FakeEntry {
        bool present = false;      // 该功能是否存在
        uint32_t address = 0;      // 总线坐标：bus << 16 | device << 11 | function << 8
        uint8_t config[256] = {};  // 配置寄存器字节
        uint32_t barValue[6] = {}; // BAR 原值（写回后读到的值）
        uint32_t barMask[6] = {};  // BAR 探测掩码
        bool barProbe[6] = {};     // 该 BAR 是否处于写全 1 的探测态
    };

    // 伪 PCI 总线：功能表与访问计数
    struct FakePci {
        FakeEntry entries[64];  // 功能表
        uint32_t count = 0;     // 已登记功能数
        uint32_t reads = 0;     // 读次数
        uint32_t writes = 0;    // 写次数
    };

    // 小端读取配置寄存器里的 32 位值
    uint32_t GetConfig32(const FakeEntry& entry, uint32_t offset) {
        uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) value |= static_cast<uint32_t>(entry.config[offset + index]) << (8 * index);
        return value;
    }

    // 小端写入配置寄存器里的 32 位值
    void SetConfig32(FakeEntry& entry, uint32_t offset, uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) entry.config[offset + index] = static_cast<uint8_t>(value >> (8 * index));
    }

    // 按总线坐标找功能；找不到返回空
    FakeEntry* FindEntry(FakePci& pci, uint32_t address) {
        for (uint32_t index = 0; index < pci.count; ++index) {
            if (pci.entries[index].present && pci.entries[index].address == address) return &pci.entries[index];
        }
        return nullptr;
    }

    // 登记一个伪 PCI 功能：厂商 / 设备 / 类码 / 头类型 / 中断线
    FakeEntry& AddEntry(FakePci& pci, uint32_t bus, uint32_t device, uint32_t function,
        uint32_t vendorId, uint32_t deviceId, uint32_t classCode, uint32_t headerType, uint32_t irq) {
        Require(pci.count < 64, "the fake PCI bus is full");
        FakeEntry& entry = pci.entries[pci.count++];
        entry = FakeEntry{};
        entry.present = true;
        entry.address = (bus << 16) | (device << 11) | (function << 8);
        SetConfig32(entry, 0x00, (vendorId & 0xFFFFu) | ((deviceId & 0xFFFFu) << 16));
        SetConfig32(entry, 0x08, ((classCode & 0xFFu) << 8) | (((classCode >> 8) & 0xFFu) << 16) | (((classCode >> 16) & 0xFFu) << 24));
        SetConfig32(entry, 0x0C, (headerType & 0xFFu) << 16);
        SetConfig32(entry, 0x3C, irq & 0xFFu);
        return entry;
    }

    // 设置 32 位 BAR：写入原值并记录掩码（内存 BAR 的低 4 位、I/O BAR 的低 2 位固定为 0）
    void SetBar32(FakePci& pci, uint32_t address, uint32_t index, uint32_t value, uint32_t bytes, uint32_t lowBits) {
        FakeEntry* entry = FindEntry(pci, address);
        Require(entry != nullptr, "the fake PCI function is missing");
        Require(index < 6, "the fake BAR index is out of range");
        entry->barValue[index] = value;
        entry->barMask[index] = (~(bytes - 1)) & ~lowBits;
        entry->barProbe[index] = false;
        SetConfig32(*entry, 0x10 + 4 * index, value);
    }

    // 设置 32 位内存 BAR
    void SetMemoryBar(FakePci& pci, uint32_t address, uint32_t index, uint32_t base, uint32_t bytes) {
        SetBar32(pci, address, index, base & ~0xFu, bytes, 0xFu);
    }

    // 设置 32 位 I/O BAR
    void SetIoBar(FakePci& pci, uint32_t address, uint32_t index, uint32_t base, uint32_t bytes) {
        SetBar32(pci, address, index, (base & ~0x3u) | 0x1u, bytes, 0x3u);
    }

    // 设置 64 位内存 BAR：低半带类型位，高半单独登记掩码
    void SetMemoryBar64(FakePci& pci, uint32_t address, uint32_t index, uint64_t base, uint64_t bytes) {
        FakeEntry* entry = FindEntry(pci, address);
        Require(entry != nullptr, "the fake PCI function is missing");
        Require(index + 1 < 6, "the fake 64-bit BAR index is out of range");
        const uint64_t mask = ~(bytes - 1);
        const uint32_t low = static_cast<uint32_t>(base & 0xFFFFFFFFu) & ~0xFu;
        const uint32_t high = static_cast<uint32_t>(base >> 32);
        entry->barValue[index] = low | 0x4u | 0x8u;   // 类型位 10b：64 位内存 BAR，可预取
        entry->barMask[index] = static_cast<uint32_t>(mask & 0xFFFFFFFFu) & ~0xFu;
        entry->barProbe[index] = false;
        entry->barValue[index + 1] = high;
        entry->barMask[index + 1] = static_cast<uint32_t>(mask >> 32);
        entry->barProbe[index + 1] = false;
        SetConfig32(*entry, 0x10 + 4 * index, entry->barValue[index]);
        SetConfig32(*entry, 0x10 + 4 * (index + 1), entry->barValue[index + 1]);
    }

    // 伪配置空间读：空槽返回 0xFFFF，探测态返回掩码
    uint32_t FakePciRead(void* context, uint32_t address, uint32_t offset) {
        FakePci* pci = static_cast<FakePci*>(context);
        ++pci->reads;
        FakeEntry* entry = FindEntry(*pci, address);
        if (entry == nullptr) return 0xFFFFFFFFu;
        const uint32_t aligned = offset & 0xFCu;
        if (aligned >= 0x10 && aligned < 0x10 + 4 * 6) {
            const uint32_t index = (aligned - 0x10) / 4;
            if (entry->barProbe[index]) return entry->barMask[index];
        }
        return GetConfig32(*entry, aligned);
    }

    // 伪配置空间写：BAR 写全 1 进入探测态，其余值写回原值
    void FakePciWrite(void* context, uint32_t address, uint32_t offset, uint32_t value) {
        FakePci* pci = static_cast<FakePci*>(context);
        ++pci->writes;
        FakeEntry* entry = FindEntry(*pci, address);
        if (entry == nullptr) return;
        const uint32_t aligned = offset & 0xFCu;
        if (aligned >= 0x10 && aligned < 0x10 + 4 * 6) {
            const uint32_t index = (aligned - 0x10) / 4;
            if (value == 0xFFFFFFFFu) {
                entry->barProbe[index] = true;
            } else {
                entry->barProbe[index] = false;
                entry->barValue[index] = value;
            }
            SetConfig32(*entry, aligned, value);
            return;
        }
        SetConfig32(*entry, aligned, value);
    }

    // 把伪总线包成 PciAccess
    DevModel::PciAccess AccessOf(FakePci& pci) {
        DevModel::PciAccess access;
        access.read = &FakePciRead;
        access.write = &FakePciWrite;
        access.context = &pci;
        return access;
    }

    // —— 夹具：伪总线、BootInfo、物理换算与日志 ——
    struct Fixture {
        FakePci pci;                      // 伪 PCI 总线
        Boot::Info info;                  // 假 BootInfo
        uint8_t clue[4] = {};             // BIOS 驱动器号字节
        bool physicalFails = false;       // 物理换算返回空
        std::vector<std::string> logs;    // 收集 [ tag ] text
        DevModel::State state;            // DevModel 模块状态
    };

    // 建默认设备树：IDE / VGA / AHCI / NVMe / 多功能 USB+SATA / PCI-PCI 桥 / CardBus / 另一条总线的 SATA
    void BuildDefaultTree(Fixture& fixture) {
        FakePci& pci = fixture.pci;
        // bus 0 dev 1：IDE 控制器，class 0x010180，BAR0..3 I/O，BAR4 I/O，IRQ 14
        const uint32_t ide = (0u << 16) | (1u << 11);
        AddEntry(pci, 0, 1, 0, 0x8086, 0x7010, 0x010180, 0x00, 14);
        SetIoBar(pci, ide, 0, 0x1F0, 8);
        SetIoBar(pci, ide, 1, 0x3F4, 4);
        SetIoBar(pci, ide, 2, 0x170, 8);
        SetIoBar(pci, ide, 3, 0x374, 4);
        SetIoBar(pci, ide, 4, 0xC000, 16);
        // bus 0 dev 2：VGA，无 BAR、无中断线
        AddEntry(pci, 0, 2, 0, 0x1234, 0x1111, 0x030000, 0x00, 0xFF);
        // bus 0 dev 3：AHCI，class 0x010601，BAR5 内存 8 KiB，IRQ 11
        const uint32_t ahci = (0u << 16) | (3u << 11);
        AddEntry(pci, 0, 3, 0, 0x8086, 0x2829, 0x010601, 0x00, 11);
        SetMemoryBar(pci, ahci, 5, 0xFEBF0000, 0x2000);
        // bus 0 dev 4：NVMe，class 0x010802，BAR0 64 位内存 16 KiB 可预取，IRQ 10
        const uint32_t nvme = (0u << 16) | (4u << 11);
        AddEntry(pci, 0, 4, 0, 0x1B36, 0x0010, 0x010802, 0x00, 10);
        SetMemoryBar64(pci, nvme, 0, 0xFE000000, 0x4000);
        // bus 0 dev 5：多功能，功能 0 是 USB、功能 1 是 SATA
        AddEntry(pci, 0, 5, 0, 0x8086, 0x1234, 0x0C0330, 0x80, 0);
        AddEntry(pci, 0, 5, 1, 0x8086, 0x2829, 0x010601, 0x00, 7);
        // bus 0 dev 6：PCI-PCI 桥，头类型 1，BAR0/1 内存
        const uint32_t bridge = (0u << 16) | (6u << 11);
        AddEntry(pci, 0, 6, 0, 0x8086, 0x244E, 0x060400, 0x01, 0);
        SetMemoryBar(pci, bridge, 0, 0xFEC00000, 0x1000);
        SetMemoryBar(pci, bridge, 1, 0xFEC01000, 0x1000);
        // bus 0 dev 7：CardBus 桥，头类型 2，不解析 BAR
        AddEntry(pci, 0, 7, 0, 0x104C, 0xAC50, 0x060700, 0x02, 0);
        // bus 1 dev 0：另一条总线上的 SATA，验证总线号编码
        const uint32_t sata = (1u << 16) | (0u << 11);
        AddEntry(pci, 1, 0, 0, 0x8086, 0x2829, 0x010601, 0x00, 5);
        SetMemoryBar(pci, sata, 5, 0xFEBF2000, 0x2000);
    }

    // 默认启动线索：BIOS 驱动器号 0x80、介质为硬盘
    void SetBiosClue(Fixture& fixture, uint32_t media, uint8_t drive) {
        fixture.info = Boot::Info{};
        fixture.info.deviceKind = static_cast<uint32_t>(Boot::DeviceKind::BiosDrive);
        fixture.info.media = media;
        fixture.info.devicePath = 0x1000;
        fixture.info.devicePathBytes = 1;
        fixture.clue[0] = drive;
    }

    // 平台回调：BootInfo 指向夹具，物理换算只认线索字节
    const Boot::Info* BootInfoOf(void* context) { return &static_cast<Fixture*>(context)->info; }
    const void* PhysicalOf(void* context, uint64_t physical, uint64_t bytes) {
        Fixture* fixture = static_cast<Fixture*>(context);
        if (fixture->physicalFails) return nullptr;
        if (physical != fixture->info.devicePath || bytes > sizeof(fixture->clue)) return nullptr;
        return fixture->clue;
    }
    void LogOf(void* context, const char* tag, const char* text) {
        Fixture* fixture = static_cast<Fixture*>(context);
        fixture->logs.push_back(std::string(tag != nullptr ? tag : "") + " " + std::string(text != nullptr ? text : ""));
    }

    // 平台组装
    DevModel::Platform PlatformOf(Fixture& fixture) {
        DevModel::Platform platform;
        platform.pci = AccessOf(fixture.pci);
        platform.bootInfo = &BootInfoOf;
        platform.physical = &PhysicalOf;
        platform.logLine = &LogOf;
        platform.context = &fixture;
        return platform;
    }

    // 日志里是否出现某条 [ tag ] text
    bool HasLog(const Fixture& fixture, const char* tag, const char* text) {
        const std::string expected = std::string(tag) + " " + text;
        for (const std::string& line : fixture.logs) {
            if (line == expected) return true;
        }
        return false;
    }

    // 初始化成功的夹具：默认树 + BIOS 硬盘线索
    void InitFixture(Fixture& fixture) {
        BuildDefaultTree(fixture);
        SetBiosClue(fixture, DevModel::kMediaHdd, 0x80);
        Require(DevModel::Init(fixture.state, PlatformOf(fixture)) == KernelError::kOk, "the fixture must initialize");
    }

    // —— 伪 PCI 总线上的用例 ——
    // 默认树枚举：数量、首条事实、类码与总线地址编码
    void CaseEnumerateDefaultTree() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        Require(result.count == 9, "the default tree must have nine functions");
        Require(result.truncated == 0, "the default tree must not be truncated");
        const DevModel::PciFunction& ide = fixture.state.functions[0];
        Require(ide.bus == 0 && ide.device == 1 && ide.function == 0, "the first function must be bus 0 device 1 function 0");
        Require(ide.vendorId == 0x8086 && ide.deviceId == 0x7010, "the IDE identity is wrong");
        Require(ide.classCode == 0x010180, "the IDE class code is wrong");
        Require(ide.busAddress == 0x0008, "the IDE bus address must be bus<<8|device<<3|function");
        Require(ide.irq == 14, "the IDE interrupt line is wrong");
        Require(ide.barCount == 5, "the IDE BAR count is wrong");
    }

    // 多功能展开：功能 0 的头类型 bit7 置位时扫描功能 1；单功能设备只读功能 0
    void CaseMultiFunction() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        uint32_t multiFunctions = 0;
        for (uint32_t index = 0; index < result.count; ++index) {
            if (fixture.state.functions[index].device == 5) ++multiFunctions;
        }
        Require(multiFunctions == 2, "the multi-function device must yield two functions");
        for (uint32_t index = 0; index < result.count; ++index) {
            if (fixture.state.functions[index].device == 2) {
                Require(fixture.state.functions[index].function == 0, "a single-function device must only yield function 0");
            }
        }
    }

    // 总线号：第二条总线上的设备带 bus 1 与对应的总线地址
    void CaseBusNumber() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        const DevModel::PciFunction& last = fixture.state.functions[result.count - 1];
        Require(last.bus == 1 && last.busAddress == 0x0100, "the bus 1 device must carry bus 1 in its address");
    }

    // 容量截断：容量不足时截断并置标志
    void CaseTruncatedEnumeration() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, 3);
        Require(result.count == 3 && result.truncated == 1, "a small capacity must truncate the enumeration");
        const DevModel::PciEnumeration empty = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, 0);
        Require(empty.count == 0 && empty.truncated == 0, "a zero capacity must enumerate nothing");
    }

    // 头类型 1 的桥只解析两个 BAR；头类型 2 不解析 BAR
    void CaseHeaderTypes() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        for (uint32_t index = 0; index < result.count; ++index) {
            const DevModel::PciFunction& function = fixture.state.functions[index];
            if (function.device == 6) Require(function.barCount == 2, "a PCI-PCI bridge must expose two BARs");
            if (function.device == 7) Require(function.barCount == 0, "a CardBus bridge must expose no BARs");
        }
    }

    // 32 位内存 BAR：基址、长度与标志
    void CaseMemoryBar() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        for (uint32_t index = 0; index < result.count; ++index) {
            const DevModel::PciFunction& function = fixture.state.functions[index];
            if (function.device != 3) continue;
            Require(function.barCount == 1, "the AHCI function must expose one BAR");
            Require(function.bars[0].index == 5, "the AHCI BAR index must be preserved");
            Require(function.bars[0].base == 0xFEBF0000, "the AHCI BAR base is wrong");
            Require(function.bars[0].bytes == 0x2000, "the AHCI BAR length is wrong");
            Require(function.bars[0].flags == 0, "the AHCI BAR must be plain memory");
            return;
        }
        Fail("the AHCI function is missing");
    }

    // I/O BAR：基址按 4 字节对齐、标志置 I/O 位
    void CaseIoBar() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        const DevModel::PciFunction& ide = fixture.state.functions[0];
        Require(result.count > 0 && ide.barCount == 5, "the IDE function must expose five BARs");
        Require(ide.bars[0].flags == DevModel::kBarFlagIo && ide.bars[0].base == 0x1F0 && ide.bars[0].bytes == 8,
            "the IDE BAR0 must be an I/O BAR");
        Require(ide.bars[4].flags == DevModel::kBarFlagIo && ide.bars[4].base == 0xC000 && ide.bars[4].bytes == 16,
            "the IDE BAR4 must be an I/O BAR");
    }

    // 64 位 BAR：成对合并、只产出一条记录、标志带 64 位与可预取
    void CaseMemoryBar64() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        for (uint32_t index = 0; index < result.count; ++index) {
            const DevModel::PciFunction& function = fixture.state.functions[index];
            if (function.device != 4) continue;
            Require(function.barCount == 1, "a 64-bit BAR must yield one entry");
            Require(function.bars[0].base == 0xFE000000, "the 64-bit BAR base is wrong");
            Require(function.bars[0].bytes == 0x4000, "the 64-bit BAR length is wrong");
            Require(function.bars[0].flags == (DevModel::kBarFlag64 | DevModel::kBarFlagPrefetch),
                "the 64-bit BAR flags are wrong");
            return;
        }
        Fail("the NVMe function is missing");
    }

    // 未分配 BAR：基址为零时置未分配标志
    void CaseUnassignedBar() {
        Fixture fixture{};
        const uint32_t address = (0u << 16) | (9u << 11);
        AddEntry(fixture.pci, 0, 9, 0, 0x8086, 0x2829, 0x010601, 0x00, 0);
        SetMemoryBar(fixture.pci, address, 5, 0, 0x2000);
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        Require(result.count == 1 && fixture.state.functions[0].barCount == 1, "the unassigned BAR must still be recorded");
        const DeviceBar& bar = fixture.state.functions[0].bars[0];
        Require(bar.base == 0 && bar.bytes == 0x2000, "the unassigned BAR base and length are wrong");
        Require((bar.flags & DevModel::kBarFlagUnassigned) != 0, "the unassigned BAR must carry the flag");
    }

    // 未实现 BAR：回读掩码为零时跳过；保留类型同样跳过
    void CaseSkippedBars() {
        Fixture fixture{};
        const uint32_t missing = (0u << 16) | (10u << 11);
        AddEntry(fixture.pci, 0, 10, 0, 0x8086, 0x0001, 0x010601, 0x00, 0);
        SetBar32(fixture.pci, missing, 5, 0, 0x1000, 0xFu);
        SetConfig32(fixture.pci.entries[0], 0x10 + 4 * 5, 0x00000000u);
        fixture.pci.entries[0].barMask[5] = 0;   // 回读全零：未实现
        const uint32_t reserved = (0u << 16) | (11u << 11);
        AddEntry(fixture.pci, 0, 11, 0, 0x8086, 0x0002, 0x010601, 0x00, 0);
        SetBar32(fixture.pci, reserved, 5, 0x2u, 0x1000, 0xFu);   // 类型位 01b：保留
        const DevModel::PciEnumeration result = DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        Require(result.count == 2, "both fake functions must be enumerated");
        Require(fixture.state.functions[0].barCount == 0, "an unimplemented BAR must be skipped");
        Require(fixture.state.functions[1].barCount == 0, "a reserved BAR type must be skipped");
    }

    // BAR 探测恢复原值：枚举结束后配置寄存器里的 BAR 与探测前一致
    void CaseBarProbeRestores() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        DevModel::EnumeratePci(AccessOf(fixture.pci), fixture.state.functions, DevModel::kMaxDevices);
        const uint32_t ide = (0u << 16) | (1u << 11);
        const FakeEntry* entry = FindEntry(fixture.pci, ide);
        Require(entry != nullptr, "the IDE function is missing");
        Require(entry->barValue[0] == ((0x1F0u & ~0x3u) | 0x1u), "the I/O BAR must be restored after probing");
        Require(entry->barValue[4] == ((0xC000u & ~0x3u) | 0x1u), "the bus master BAR must be restored after probing");
        Require(entry->barProbe[0] == false && entry->barProbe[4] == false, "probing must not stay active");
    }

    // —— DeviceModel：初始化与记录 ——
    // 默认树初始化：记录数、存储控制器数、候选数与线索事实
    void CaseInitRecords() {
        Fixture fixture{};
        InitFixture(fixture);
        Require(fixture.state.deviceCount == 9, "the device record count is wrong");
        Require(fixture.state.storageCount == 5, "the storage controller count is wrong");
        Require(fixture.state.candidateCount == 5, "the boot candidate count is wrong");
        Require(fixture.state.clueKind == static_cast<uint32_t>(Boot::DeviceKind::BiosDrive), "the clue kind is wrong");
        Require(fixture.state.clueDrive == 0x80, "the BIOS drive number is wrong");
        Require(fixture.state.clueFallback == 0, "a readable BIOS clue must not fall back");
        Require(HasLog(fixture, "INFO", DevModel::kMarkerEnumeration), "the enumeration marker is missing");
        Require(HasLog(fixture, "INFO", DevModel::kMarkerClueMatched), "the matched clue marker is missing");
    }

    // 重复初始化幂等：第二次返回成功且不重复枚举
    void CaseInitIdempotent() {
        Fixture fixture{};
        InitFixture(fixture);
        const size_t logs = fixture.logs.size();
        Require(DevModel::Init(fixture.state, PlatformOf(fixture)) == KernelError::kOk, "the second Init must succeed");
        Require(fixture.state.deviceCount == 9, "the second Init must keep the records");
        Require(fixture.logs.size() == logs, "the second Init must not log again");
    }

    // 平台回调缺失：拒绝初始化
    void CaseInitBadPlatform() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        SetBiosClue(fixture, DevModel::kMediaHdd, 0x80);
        DevModel::Platform platform = PlatformOf(fixture);
        platform.logLine = nullptr;
        Require(DevModel::Init(fixture.state, platform) == KernelError::kInvalidArgument, "a missing log callback must be rejected");
        platform = PlatformOf(fixture);
        platform.pci.write = nullptr;
        Require(DevModel::Init(fixture.state, platform) == KernelError::kInvalidArgument, "a missing PCI write callback must be rejected");
    }

    // BootInfo 缺失：返回 kCorrupt 并写标记
    void CaseInitMissingBootInfo() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        SetBiosClue(fixture, DevModel::kMediaHdd, 0x80);
        fixture.info = Boot::Info{};
        DevModel::Platform platform = PlatformOf(fixture);
        platform.bootInfo = nullptr;
        Require(DevModel::Init(fixture.state, platform) == KernelError::kInvalidArgument, "a missing BootInfo callback must be rejected");
        platform = PlatformOf(fixture);
        platform.bootInfo = [](void*) -> const Boot::Info* { return nullptr; };
        Require(DevModel::Init(fixture.state, platform) == KernelError::kCorrupt, "a null BootInfo must be rejected");
        Require(HasLog(fixture, "WARN", DevModel::kMarkerBootInfoMissing), "the missing BootInfo marker is missing");
    }

    // 启动线索回退：UEFI 设备路径 / 未提供线索都退回全量存储控制器并写标记
    void CaseClueFallbackKinds() {
        for (const uint32_t kind : { static_cast<uint32_t>(Boot::DeviceKind::UefiDevicePath),
                 static_cast<uint32_t>(Boot::DeviceKind::None) }) {
            Fixture fixture{};
            BuildDefaultTree(fixture);
            SetBiosClue(fixture, DevModel::kMediaHdd, 0x80);
            fixture.info.deviceKind = kind;
            Require(DevModel::Init(fixture.state, PlatformOf(fixture)) == KernelError::kOk, "the fallback fixture must initialize");
            Require(fixture.state.clueFallback == 1, "an unparsable clue must fall back");
            Require(fixture.state.candidateCount == fixture.state.storageCount, "the fallback must report all storage controllers");
            Require(HasLog(fixture, "WARN", DevModel::kMarkerClueFallback), "the fallback marker is missing");
        }
    }

    // 启动线索不可读：物理换算失败或长度为零都退回全量枚举
    void CaseClueUnreadable() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        SetBiosClue(fixture, DevModel::kMediaHdd, 0x80);
        fixture.physicalFails = true;
        Require(DevModel::Init(fixture.state, PlatformOf(fixture)) == KernelError::kOk, "an unreadable clue must still initialize");
        Require(fixture.state.clueFallback == 1 && fixture.state.clueDrive == DevModel::kNoClueDrive, "the clue must be marked unknown");
        Fixture shortClue{};
        BuildDefaultTree(shortClue);
        SetBiosClue(shortClue, DevModel::kMediaHdd, 0x80);
        shortClue.info.devicePathBytes = 0;
        Require(DevModel::Init(shortClue.state, PlatformOf(shortClue)) == KernelError::kOk, "a zero-length clue must still initialize");
        Require(shortClue.state.clueFallback == 1, "a zero-length clue must fall back");
    }

    // 光盘线索：候选排除 NVMe 控制器
    void CaseCdromCandidates() {
        Fixture fixture{};
        BuildDefaultTree(fixture);
        SetBiosClue(fixture, DevModel::kMediaCdrom, 0x9F);
        Require(DevModel::Init(fixture.state, PlatformOf(fixture)) == KernelError::kOk, "the CD-ROM fixture must initialize");
        Require(fixture.state.clueFallback == 0, "a readable BIOS clue must not fall back");
        Require(fixture.state.candidateCount == 4, "the CD-ROM candidates must exclude NVMe");
    }

    // 设备表截断：超过容量时记录前 32 条并写满表标记
    void CaseDeviceTableFull() {
        Fixture fixture{};
        for (uint32_t device = 0; device < 32; ++device) {
            AddEntry(fixture.pci, 0, device, 0, 0x8086, 0x2829, 0x010601, 0x00, 0);
        }
        AddEntry(fixture.pci, 1, 0, 0, 0x8086, 0x2829, 0x010601, 0x00, 0);
        SetBiosClue(fixture, DevModel::kMediaHdd, 0x80);
        Require(DevModel::Init(fixture.state, PlatformOf(fixture)) == KernelError::kOk, "a truncated tree must still initialize");
        Require(fixture.state.deviceCount == DevModel::kMaxDevices, "the device table must stop at its capacity");
        Require(HasLog(fixture, "WARN", DevModel::kMarkerTableFull), "the table-full marker is missing");
    }

    // —— DeviceModel：六槽 ——
    // Enumerate：句柄从 1 连续、结束后返回 kNotFound
    void CaseEnumerateSlot() {
        Fixture fixture{};
        InitFixture(fixture);
        uint32_t cursor = 0;
        uint32_t count = 0;
        for (;;) {
            DeviceFacts facts{};
            const KernelError error = DevModel::Enumerate(fixture.state, &cursor, &facts);
            if (error == KernelError::kNotFound) break;
            Require(error == KernelError::kOk, "Enumerate must return kOk until the end");
            Require(facts.bytes == sizeof(DeviceFacts), "Enumerate must write the structure length");
            Require(facts.deviceHandle == count + 1, "device handles must be contiguous from one");
            Require(facts.busKind == DevModel::kBusKindPci, "the bus kind must be PCI");
            ++count;
        }
        Require(count == fixture.state.deviceCount, "Enumerate must walk every device");
        Require(DevModel::Enumerate(fixture.state, nullptr, nullptr) == KernelError::kInvalidArgument, "null arguments must be rejected");
        DeviceFacts small{};
        small.bytes = 1;
        cursor = 0;
        Require(DevModel::Enumerate(fixture.state, &cursor, &small) == KernelError::kInvalidArgument, "an undersized buffer must be rejected");
    }

    // Resources：BAR 表、IRQ 表与 DMA 位宽；句柄与缓冲校验
    void CaseResourcesSlot() {
        Fixture fixture{};
        InitFixture(fixture);
        DeviceResources resources{};
        Require(DevModel::Resources(fixture.state, 1, &resources) == KernelError::kOk, "device 1 resources must be available");
        Require(resources.bytes == sizeof(DeviceResources), "Resources must write the structure length");
        Require(resources.barCount == 5, "the IDE BAR count is wrong");
        Require(resources.bars[0].base == 0x1F0 && resources.bars[0].bytes == 8, "the IDE BAR0 is wrong");
        Require(resources.irqCount == 1 && resources.irqs[0].irq == 14, "the IDE IRQ is wrong");
        Require((resources.irqs[0].flags & DevModel::kIrqFlagPciLine) != 0, "the IRQ must come from the PCI line");
        Require(resources.dmaAddressBits == DevModel::kDmaAddressBits, "the DMA address width is wrong");
        Require(DevModel::Resources(fixture.state, 0, &resources) == KernelError::kNotFound, "handle 0 must be rejected");
        Require(DevModel::Resources(fixture.state, 99, &resources) == KernelError::kNotFound, "an unknown handle must be rejected");
        Require(DevModel::Resources(fixture.state, 1, nullptr) == KernelError::kInvalidArgument, "a null buffer must be rejected");
        DeviceResources small{};
        small.bytes = 8;
        Require(DevModel::Resources(fixture.state, 1, &small) == KernelError::kInvalidArgument, "an undersized buffer must be rejected");
    }

    // Claim / Release：唯一控制者、标志与状态
    void CaseClaimRelease() {
        Fixture fixture{};
        InitFixture(fixture);
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.state, 1, 0, &claimHandle) == KernelError::kOk, "the first claim must succeed");
        Require(claimHandle == 1, "the claim handle must equal the device handle in v1");
        Require(DevModel::Claim(fixture.state, 1, 0, &claimHandle) == KernelError::kBusy, "a second claim must report busy");
        Require(DevModel::Claim(fixture.state, 0, 0, &claimHandle) == KernelError::kNotFound, "handle 0 must be rejected");
        Require(DevModel::Claim(fixture.state, 1, 1, &claimHandle) == KernelError::kInvalidArgument, "nonzero flags must be rejected");
        Require(DevModel::Claim(fixture.state, 1, 0, nullptr) == KernelError::kInvalidArgument, "a null output must be rejected");
        Require(DevModel::Release(fixture.state, 2) == KernelError::kState, "releasing an unclaimed device must report state");
        Require(DevModel::Release(fixture.state, 1) == KernelError::kOk, "releasing the claim must succeed");
        Require(DevModel::Release(fixture.state, 99) == KernelError::kNotFound, "an unknown handle must be rejected");
        Require(DevModel::Claim(fixture.state, 1, 0, &claimHandle) == KernelError::kOk, "the device must be claimable again");
    }

    // BootCandidates：BIOS 硬盘候选按顺序返回，结束后 kNotFound
    void CaseBootCandidates() {
        Fixture fixture{};
        InitFixture(fixture);
        uint32_t cursor = 0;
        uint32_t count = 0;
        for (;;) {
            uint32_t handle = 0;
            const KernelError error = DevModel::BootCandidates(fixture.state, &cursor, &handle);
            if (error == KernelError::kNotFound) break;
            Require(error == KernelError::kOk, "BootCandidates must return kOk until the end");
            Require(handle >= 1 && handle <= fixture.state.deviceCount, "the candidate handle is out of range");
            DeviceFacts facts{};
            uint32_t walk = handle - 1;
            Require(DevModel::Enumerate(fixture.state, &walk, &facts) == KernelError::kOk, "the candidate must be enumerable");
            Require((facts.classCode >> 16) == DevModel::kPciClassStorage, "a candidate must be a storage controller");
            ++count;
        }
        Require(count == fixture.state.candidateCount, "BootCandidates must report every candidate");
        Require(DevModel::BootCandidates(fixture.state, nullptr, nullptr) == KernelError::kInvalidArgument, "null arguments must be rejected");
    }

    // Binding：未绑定查询、未认领登记、登记 / 更新 / 清除
    void CaseBindingSlot() {
        Fixture fixture{};
        InitFixture(fixture);
        BindingFacts facts{};
        Require(DevModel::Binding(fixture.state, 1, &facts) == KernelError::kNotFound, "an unbound device must report not found");
        facts = BindingFacts{};
        facts.bytes = sizeof(BindingFacts);
        facts.state = 1;
        Require(DevModel::Binding(fixture.state, 1, &facts) == KernelError::kState, "binding without a claim must be rejected");
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.state, 1, 0, &claimHandle) == KernelError::kOk, "the claim must succeed");
        facts = BindingFacts{};
        facts.bytes = sizeof(BindingFacts);
        facts.bindHandle = 7;
        facts.state = 1;
        Require(DevModel::Binding(fixture.state, 1, &facts) == KernelError::kOk, "the binding registration must succeed");
        BindingFacts query{};
        Require(DevModel::Binding(fixture.state, 1, &query) == KernelError::kOk, "the binding query must succeed");
        Require(query.bindHandle == 7 && query.deviceHandle == 1 && query.state == 1, "the binding facts are wrong");
        Require(query.ops == nullptr && query.context == nullptr, "a bound but not started driver must have no ops");
        BindingFacts started{};
        started.bytes = sizeof(BindingFacts);
        started.bindHandle = 7;
        started.state = 2;
        static BlockDeviceOps ops{};
        started.ops = &ops;
        started.context = &fixture;
        Require(DevModel::Binding(fixture.state, 1, &started) == KernelError::kOk, "the binding update must succeed");
        BindingFacts updated{};
        Require(DevModel::Binding(fixture.state, 1, &updated) == KernelError::kOk, "the updated binding must be readable");
        Require(updated.state == 2 && updated.ops == &ops && updated.context == &fixture, "the updated binding facts are wrong");
        BindingFacts cleared{};
        cleared.bytes = sizeof(BindingFacts);
        cleared.state = 0;
        Require(DevModel::Binding(fixture.state, 1, &cleared) == KernelError::kOk, "clearing the binding must succeed");
        BindingFacts missing{};
        Require(DevModel::Binding(fixture.state, 1, &missing) == KernelError::kNotFound, "a cleared binding must be gone");
        Require(DevModel::Release(fixture.state, 1) == KernelError::kOk, "the claim must be releasable after clearing");
        BindingFacts bad{};
        bad.bytes = 7;
        Require(DevModel::Binding(fixture.state, 1, &bad) == KernelError::kInvalidArgument, "a bad structure length must be rejected");
        Require(DevModel::Binding(fixture.state, 1, nullptr) == KernelError::kInvalidArgument, "a null buffer must be rejected");
    }

    // Release 与绑定：仍有绑定时不允许归还控制权
    void CaseReleaseWhileBound() {
        Fixture fixture;
        InitFixture(fixture);
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.state, 1, 0, &claimHandle) == KernelError::kOk, "the claim must succeed");
        BindingFacts facts{};
        facts.bytes = sizeof(BindingFacts);
        facts.bindHandle = 1;
        facts.state = 1;
        Require(DevModel::Binding(fixture.state, 1, &facts) == KernelError::kOk, "the binding must register");
        Require(DevModel::Release(fixture.state, 1) == KernelError::kBusy, "a bound device must not be released");
        BindingFacts cleared{};
        cleared.bytes = sizeof(BindingFacts);
        cleared.state = 0;
        Require(DevModel::Binding(fixture.state, 1, &cleared) == KernelError::kOk, "the binding must clear");
        Require(DevModel::Release(fixture.state, 1) == KernelError::kOk, "the claim must be releasable after clearing");
    }

    // —— DevModel v2：Register 槽 ——
    // 组装一条待登记的设备事实
    DeviceFacts MakeFacts(uint32_t busKind, uint32_t classCode, uint32_t vendorId, uint32_t deviceId, uint32_t busAddress) {
        DeviceFacts facts{};
        facts.bytes = static_cast<uint32_t>(sizeof(DeviceFacts));
        facts.busKind = busKind;
        facts.classCode = classCode;
        facts.vendorId = vendorId;
        facts.deviceId = deviceId;
        facts.busAddress = busAddress;
        return facts;
    }

    // Register 正例：句柄 = 下标 + 1、Enumerate 可见、事实原样保留
    void CaseRegisterDevice() {
        Fixture fixture;
        InitFixture(fixture);
        const uint32_t before = fixture.state.deviceCount;
        const DeviceFacts facts = MakeFacts(2, 0x080650, 0x46F4, 0x0001, 0x0102);
        uint32_t handle = 0;
        Require(DevModel::Register(fixture.state, &facts, &handle) == KernelError::kOk, "the registration must succeed");
        Require(handle == before + 1, "the handle must be index + 1");
        Require(fixture.state.deviceCount == before + 1, "the device count must advance");
        DeviceFacts listed{};
        uint32_t cursor = before;
        Require(DevModel::Enumerate(fixture.state, &cursor, &listed) == KernelError::kOk, "the registered device must be enumerable");
        Require(listed.deviceHandle == handle, "the enumerated handle must match");
        Require(listed.busKind == 2 && listed.classCode == 0x080650 && listed.busAddress == 0x0102, "the facts must be preserved");
        Require(listed.vendorId == 0x46F4 && listed.deviceId == 0x0001, "the identity must be preserved");
        DeviceResources resources{};
        Require(DevModel::Resources(fixture.state, handle, &resources) == KernelError::kOk, "the registered device must expose resources");
        Require(resources.barCount == 0 && resources.irqCount == 0, "a registered device starts without resources");
        Require(fixture.state.storageCount == 5 && fixture.state.candidateCount == 5, "a non-storage device must not change the boot candidates");
    }

    // Register 重复：同 busKind + busAddress 拒绝，不同总线类别允许
    void CaseRegisterDuplicate() {
        Fixture fixture;
        InitFixture(fixture);
        const DeviceFacts facts = MakeFacts(2, 0x080650, 0x46F4, 0x0001, 0x0102);
        uint32_t handle = 0;
        Require(DevModel::Register(fixture.state, &facts, &handle) == KernelError::kOk, "the first registration must succeed");
        Require(DevModel::Register(fixture.state, &facts, &handle) == KernelError::kAlreadyExists, "a duplicate address must be rejected");
        const DeviceFacts otherBus = MakeFacts(3, 0x080650, 0x46F4, 0x0001, 0x0102);
        Require(DevModel::Register(fixture.state, &otherBus, &handle) == KernelError::kOk, "the same address on another bus must be accepted");
    }

    // Register 校验：空指针、结构长度、总线类别、类码与标识宽度、未初始化
    void CaseRegisterValidation() {
        Fixture fixture;
        InitFixture(fixture);
        uint32_t handle = 0;
        Require(DevModel::Register(fixture.state, nullptr, &handle) == KernelError::kInvalidArgument, "a null facts pointer must be rejected");
        const DeviceFacts facts = MakeFacts(2, 0x080650, 0x46F4, 0x0001, 0x0102);
        Require(DevModel::Register(fixture.state, &facts, nullptr) == KernelError::kInvalidArgument, "a null output must be rejected");
        DeviceFacts shortFacts = facts;
        shortFacts.bytes = 8;
        Require(DevModel::Register(fixture.state, &shortFacts, &handle) == KernelError::kInvalidArgument, "a short structure must be rejected");
        DeviceFacts wildcard = facts;
        wildcard.busKind = 0;
        Require(DevModel::Register(fixture.state, &wildcard, &handle) == KernelError::kInvalidArgument, "a zero bus kind must be rejected");
        DeviceFacts wideClass = facts;
        wideClass.classCode = 0x01000000;
        Require(DevModel::Register(fixture.state, &wideClass, &handle) == KernelError::kInvalidArgument, "a class code beyond 24 bits must be rejected");
        DeviceFacts wideVendor = facts;
        wideVendor.vendorId = 0x10000;
        Require(DevModel::Register(fixture.state, &wideVendor, &handle) == KernelError::kInvalidArgument, "a vendor id beyond 16 bits must be rejected");
        DeviceFacts wideDevice = facts;
        wideDevice.deviceId = 0x10000;
        Require(DevModel::Register(fixture.state, &wideDevice, &handle) == KernelError::kInvalidArgument, "a device id beyond 16 bits must be rejected");
        Fixture fresh;
        BuildDefaultTree(fresh);
        SetBiosClue(fresh, DevModel::kMediaHdd, 0x80);
        Require(DevModel::Register(fresh.state, &facts, &handle) == KernelError::kState, "registration before Init must be rejected");
    }

    // Register 表满：第 33 条记录返回 kOutOfResources
    void CaseRegisterTableFull() {
        Fixture fixture;
        InitFixture(fixture);
        uint32_t handle = 0;
        for (uint32_t index = fixture.state.deviceCount; index < DevModel::kMaxDevices; ++index) {
            const DeviceFacts facts = MakeFacts(2, 0x080650, 0x46F4, static_cast<uint32_t>(index), 0x0200 + index);
            Require(DevModel::Register(fixture.state, &facts, &handle) == KernelError::kOk, "registration below capacity must succeed");
        }
        Require(fixture.state.deviceCount == DevModel::kMaxDevices, "the table must be full");
        const DeviceFacts overflow = MakeFacts(2, 0x080650, 0x46F4, 0x00FF, 0x0300);
        Require(DevModel::Register(fixture.state, &overflow, &handle) == KernelError::kOutOfResources, "registration beyond capacity must fail");
        Require(fixture.state.deviceCount == DevModel::kMaxDevices, "a failed registration must not grow the table");
    }

    // Register 与认领 / 绑定闭环：登记后按 v1 语义认领并写绑定事实，查询可见
    void CaseRegisterBindingLoop() {
        Fixture fixture;
        InitFixture(fixture);
        const DeviceFacts facts = MakeFacts(2, 0x080650, 0x46F4, 0x0001, 0x0102);
        uint32_t handle = 0;
        Require(DevModel::Register(fixture.state, &facts, &handle) == KernelError::kOk, "the registration must succeed");
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.state, handle, 0, &claimHandle) == KernelError::kOk, "the registered device must be claimable");
        BindingFacts registered{};
        registered.bytes = sizeof(BindingFacts);
        registered.bindHandle = 9;
        registered.state = 2;
        static BlockDeviceOps ops{};
        registered.ops = &ops;
        registered.context = &fixture;
        Require(DevModel::Binding(fixture.state, handle, &registered) == KernelError::kOk, "the binding must register");
        BindingFacts queried{};
        Require(DevModel::Binding(fixture.state, handle, &queried) == KernelError::kOk, "the v1 binding query must succeed");
        Require(queried.bindHandle == 9 && queried.deviceHandle == handle, "the queried binding handle is wrong");
        Require(queried.state == 2 && queried.ops == &ops && queried.context == &fixture, "the queried binding facts are wrong");
        Require(DevModel::Release(fixture.state, handle) == KernelError::kBusy, "a bound device must not be released");
        BindingFacts cleared{};
        cleared.bytes = sizeof(BindingFacts);
        cleared.state = 0;
        Require(DevModel::Binding(fixture.state, handle, &cleared) == KernelError::kOk, "the binding must clear");
        Require(DevModel::Release(fixture.state, handle) == KernelError::kOk, "the claim must be releasable after clearing");
    }

    // —— DriverLoader 夹具 ——
    // 伪驱动实例：启停计数与 start 的返回码
    struct FakeDriver {
        uint32_t startCalls = 0;              // start 调用次数
        uint32_t stopCalls = 0;               // stop 调用次数
        Storage::Error startResult = Storage::Error::Ok;  // start 返回码
    };

    // 空 ops 表：DriverDesc.ops 只要求非空，宿主检查不调用其中的回调
    BlockDeviceOps g_fakeOps{};

    // 伪 start 回调
    Storage::Error FakeStart(void* context) {
        FakeDriver* driver = static_cast<FakeDriver*>(context);
        ++driver->startCalls;
        return driver->startResult;
    }

    // 伪 stop 回调
    void FakeStop(void* context) {
        FakeDriver* driver = static_cast<FakeDriver*>(context);
        ++driver->stopCalls;
    }

    // 组装一条驱动描述：匹配字段按参数填，回调与上下文指向伪驱动
    void MakeDescription(DriverDesc& description, FakeDriver& driver, uint32_t busKind, uint32_t classCode,
        uint32_t vendorId, uint32_t deviceId) {
        description = DriverDesc{};
        description.bytes = sizeof(DriverDesc);
        description.driveSdkVersion = kDriveSdkVersion;
        description.opsKind = DrvLoader::kOpsKindBlockDevice;
        description.busKind = busKind;
        description.classCode = classCode;
        description.vendorId = vendorId;
        description.deviceId = deviceId;
        description.start = &FakeStart;
        description.stop = &FakeStop;
        description.ops = &g_fakeOps;
        description.context = &driver;
    }

    // 指向夹具状态的一组 Dev.Model 槽：与装载器回填后的形态一致
    DevModel::State* g_slotState = nullptr;

    // 槽转发：全部打到当前夹具状态
    KernelError SlotEnumerate(uint32_t* cursor, DeviceFacts* facts) { return DevModel::Enumerate(*g_slotState, cursor, facts); }
    KernelError SlotResources(uint32_t handle, DeviceResources* resources) { return DevModel::Resources(*g_slotState, handle, resources); }
    KernelError SlotClaim(uint32_t handle, uint32_t flags, uint32_t* claim) { return DevModel::Claim(*g_slotState, handle, flags, claim); }
    KernelError SlotRelease(uint32_t handle) { return DevModel::Release(*g_slotState, handle); }
    KernelError SlotBootCandidates(uint32_t* cursor, uint32_t* handle) { return DevModel::BootCandidates(*g_slotState, cursor, handle); }
    KernelError SlotBinding(uint32_t handle, BindingFacts* facts) { return DevModel::Binding(*g_slotState, handle, facts); }

    // 组装 Dev.Model 槽表
    DevModelSlotsV1 SlotsOf(DevModel::State& state) {
        g_slotState = &state;
        DevModelSlotsV1 slots{};
        slots.Enumerate = &SlotEnumerate;
        slots.Resources = &SlotResources;
        slots.Claim = &SlotClaim;
        slots.Release = &SlotRelease;
        slots.BootCandidates = &SlotBootCandidates;
        slots.Binding = &SlotBinding;
        return slots;
    }

    // DriverLoader 夹具：设备模型夹具 + 驱动加载器状态 + 日志
    struct LoaderFixture {
        Fixture device;                 // 设备模型夹具
        DrvLoader::State loader;        // 驱动加载器状态
        DevModelSlotsV1 slots;          // Dev.Model 槽表
        std::vector<std::string> logs;  // 驱动加载器日志
    };

    // 驱动加载器日志回调
    void LoaderLogOf(void* context, const char* tag, const char* text) {
        LoaderFixture* fixture = static_cast<LoaderFixture*>(context);
        fixture->logs.push_back(std::string(tag != nullptr ? tag : "") + " " + std::string(text != nullptr ? text : ""));
    }

    // 初始化驱动加载器夹具
    void InitLoader(LoaderFixture& fixture) {
        InitFixture(fixture.device);
        fixture.slots = SlotsOf(fixture.device.state);
        Require(DrvLoader::Init(fixture.loader, &fixture.slots, &LoaderLogOf, &fixture) == KernelError::kOk,
            "the driver loader fixture must initialize");
    }

    // —— DriverLoader：注册与匹配 ——
    // Init：ready 标记与参数校验
    void CaseLoaderInit() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        Require(fixture.logs.size() == 1 && fixture.logs[0] == std::string("INFO ") + DrvLoader::kMarkerReady,
            "the ready marker must be the first line");
        Require(DrvLoader::Init(fixture.loader, nullptr, &LoaderLogOf, &fixture) == KernelError::kInvalidArgument,
            "a null Dev.Model table must be rejected");
        DevModelSlotsV1 broken{};
        broken.Enumerate = &SlotEnumerate;
        broken.Claim = &SlotClaim;
        broken.Release = &SlotRelease;
        Require(DrvLoader::Init(fixture.loader, &broken, &LoaderLogOf, &fixture) == KernelError::kCorrupt,
            "a table without Binding must be rejected");
    }

    // RegisterDriver：合法描述与逐项负例
    void CaseRegisterDriver() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "a valid driver must register");
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kAlreadyExists, "a duplicate driver must be rejected");
        Require(DrvLoader::RegisterDriver(fixture.loader, nullptr) == KernelError::kInvalidArgument, "a null description must be rejected");
        DriverDesc broken = description;
        broken.bytes = 8;
        Require(DrvLoader::RegisterDriver(fixture.loader, &broken) == KernelError::kInvalidArgument, "a short structure must be rejected");
        broken = description;
        broken.driveSdkVersion = static_cast<uint16_t>(kDriveSdkVersion + 1);
        Require(DrvLoader::RegisterDriver(fixture.loader, &broken) == KernelError::kVersionMismatch, "a DriveSDK mismatch must be rejected");
        broken = description;
        broken.opsKind = 2;
        Require(DrvLoader::RegisterDriver(fixture.loader, &broken) == KernelError::kNotSupported, "an unknown ops kind must be rejected");
        broken = description;
        broken.flags = 1;
        Require(DrvLoader::RegisterDriver(fixture.loader, &broken) == KernelError::kInvalidArgument, "nonzero flags must be rejected");
        broken = description;
        broken.start = nullptr;
        Require(DrvLoader::RegisterDriver(fixture.loader, &broken) == KernelError::kInvalidArgument, "a missing start callback must be rejected");
        broken = description;
        broken.classCode = 0x01000000;
        Require(DrvLoader::RegisterDriver(fixture.loader, &broken) == KernelError::kInvalidArgument, "a class code beyond 24 bits must be rejected");
        broken = description;
        broken.vendorId = 0x10000;
        Require(DrvLoader::RegisterDriver(fixture.loader, &broken) == KernelError::kInvalidArgument, "a vendor id beyond 16 bits must be rejected");
    }

    // RegisterDriver：容量上限
    void CaseRegisterCapacity() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver drivers[DrvLoader::kMaxDrivers + 1];
        for (uint32_t index = 0; index < DrvLoader::kMaxDrivers; ++index) {
            DriverDesc description{};
            MakeDescription(description, drivers[index], DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010 + index);
            Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "registration below capacity must succeed");
        }
        DriverDesc overflow{};
        MakeDescription(overflow, drivers[DrvLoader::kMaxDrivers], DevModel::kBusKindPci, 0x010180, 0x8086, 0x9999);
        Require(DrvLoader::RegisterDriver(fixture.loader, &overflow) == KernelError::kOutOfResources, "registration beyond capacity must fail");
    }

    // Bind：匹配、通配、重复绑定与未知设备
    void CaseBindMatching() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kOk, "the matching bind must succeed");
        Require(bindHandle != 0, "the bind handle must be nonzero");
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kBusy, "binding the same device twice must report busy");
        Require(DrvLoader::Bind(fixture.loader, 99, &description, &bindHandle) == KernelError::kNotFound, "an unknown device must be rejected");
        DriverDesc unknown{};
        FakeDriver other;
        MakeDescription(unknown, other, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7011);
        Require(DrvLoader::Bind(fixture.loader, 1, &unknown, &bindHandle) == KernelError::kNotFound, "an unregistered driver must be rejected");
        DriverDesc mismatch{};
        FakeDriver mismatchDriver;
        MakeDescription(mismatch, mismatchDriver, DevModel::kBusKindPci, 0x030000, 0x1234, 0x1111);
        Require(DrvLoader::RegisterDriver(fixture.loader, &mismatch) == KernelError::kOk, "the VGA driver must register");
        Require(DrvLoader::Bind(fixture.loader, 1, &mismatch, &bindHandle) == KernelError::kNotFound, "a class mismatch must not bind");
        Require(DrvLoader::Bind(fixture.loader, 2, &mismatch, &bindHandle) == KernelError::kOk, "the VGA driver must bind the VGA device");
        Require(DrvLoader::Bind(fixture.loader, 1, nullptr, &bindHandle) == KernelError::kInvalidArgument, "a null description must be rejected");
    }

    // Bind：全通配驱动按枚举顺序匹配任意设备
    void CaseBindWildcard() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DrvLoader::kBusKindAny, DrvLoader::kClassCodeAny,
            DrvLoader::kVendorIdAny, DrvLoader::kDeviceIdAny);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the wildcard driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 5, &description, &bindHandle) == KernelError::kOk, "the wildcard driver must bind any device");
        Require(DrvLoader::Bind(fixture.loader, 6, &description, &bindHandle) == KernelError::kOk, "the wildcard driver must bind a second device");
        Require(bindHandle != 0, "the second bind handle must be nonzero");
    }

    // —— DriverLoader：启停与记录 ——
    // Start：调用 start 回调、写回 ops / context、状态推进
    void CaseStartBinding() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kOk, "the bind must succeed");
        BindingFacts bound{};
        Require(DevModel::Binding(fixture.device.state, 1, &bound) == KernelError::kOk, "the bound facts must be registered");
        Require(bound.state == DrvLoader::kBindingStateBound && bound.ops == nullptr, "a bound driver must not expose ops yet");
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.device.state, 1, 0, &claimHandle) == KernelError::kBusy, "the device must stay claimed");
        Require(DrvLoader::Start(fixture.loader, bindHandle) == KernelError::kOk, "the start must succeed");
        Require(driver.startCalls == 1, "the start callback must run once");
        BindingFacts started{};
        Require(DevModel::Binding(fixture.device.state, 1, &started) == KernelError::kOk, "the started facts must be registered");
        Require(started.state == DrvLoader::kBindingStateStarted, "the binding state must be started");
        Require(started.ops == &g_fakeOps && started.context == &driver, "the started facts must carry ops and context");
        Require(DrvLoader::Start(fixture.loader, bindHandle) == KernelError::kState, "starting twice must report state");
        Require(DrvLoader::Start(fixture.loader, 99) == KernelError::kNotFound, "an unknown handle must be rejected");
    }

    // Start 失败：映射返回码并回滚绑定与认领
    void CaseStartFailure() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        driver.startResult = Storage::Error::Io;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kOk, "the bind must succeed");
        Require(DrvLoader::Start(fixture.loader, bindHandle) == KernelError::kIo, "a failed start must map to kIo");
        BindingFacts facts{};
        Require(DevModel::Binding(fixture.device.state, 1, &facts) == KernelError::kNotFound, "a failed start must clear the binding");
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.device.state, 1, 0, &claimHandle) == KernelError::kOk, "a failed start must release the claim");
        Require(DevModel::Release(fixture.device.state, 1) == KernelError::kOk, "the released claim must be re-releasable");
    }

    // Stop：调用 stop 回调、清除绑定、归还认领、句柄失效
    void CaseStopBinding() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kOk, "the bind must succeed");
        Require(DrvLoader::Start(fixture.loader, bindHandle) == KernelError::kOk, "the start must succeed");
        Require(DrvLoader::Stop(fixture.loader, bindHandle) == KernelError::kOk, "the stop must succeed");
        Require(driver.stopCalls == 1, "the stop callback must run once");
        BindingFacts facts{};
        Require(DevModel::Binding(fixture.device.state, 1, &facts) == KernelError::kNotFound, "the stop must clear the binding");
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.device.state, 1, 0, &claimHandle) == KernelError::kOk, "the stop must release the claim");
        Require(DevModel::Release(fixture.device.state, 1) == KernelError::kOk, "the released claim must be re-releasable");
        Require(DrvLoader::Stop(fixture.loader, bindHandle) == KernelError::kNotFound, "a stopped binding must be invalid");
    }

    // Stop：只绑定未启动时直接回收，不调用 stop 回调
    void CaseStopUnstarted() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kOk, "the bind must succeed");
        Require(DrvLoader::Stop(fixture.loader, bindHandle) == KernelError::kOk, "stopping a bound driver must succeed");
        Require(driver.stopCalls == 0, "an unstarted driver must not get a stop callback");
        BindingFacts facts{};
        Require(DevModel::Binding(fixture.device.state, 1, &facts) == KernelError::kNotFound, "the binding must be cleared");
    }

    // 绑定记录同步：绑定后 DevModel 可查到驱动句柄与状态
    void CaseBindingRecordSync() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kOk, "the bind must succeed");
        BindingFacts facts{};
        Require(DevModel::Binding(fixture.device.state, 1, &facts) == KernelError::kOk, "the binding must be queryable");
        Require(facts.bindHandle == bindHandle, "the binding handle must match the returned handle");
        Require(facts.deviceHandle == 1, "the binding device handle is wrong");
        Require(facts.driverModule.bytes[0] == 0 && facts.driverModule.bytes[15] == 0, "the v1 driver module id stays zero");
    }

    // .drv 装载：返回 kNotSupported 并写固定 WARN 行
    void CaseExternalDriver() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        Require(DrvLoader::LoadExternal(fixture.loader, "/Drivers/Ata.drv") == KernelError::kNotSupported, ".drv loading must be unsupported");
        Require(fixture.loader.externalAttempts == 1, "the external attempt counter must advance");
        Require(fixture.logs.back() == std::string("WARN ") + DrvLoader::kMarkerExternalUnsupported, "the WARN marker is missing");
    }

    // Abort：停止全部绑定、归还认领、清空表
    void CaseLoaderAbort() {
        LoaderFixture fixture{};
        InitLoader(fixture);
        FakeDriver driver;
        DriverDesc description{};
        MakeDescription(description, driver, DevModel::kBusKindPci, 0x010180, 0x8086, 0x7010);
        Require(DrvLoader::RegisterDriver(fixture.loader, &description) == KernelError::kOk, "the driver must register");
        uint32_t bindHandle = 0;
        Require(DrvLoader::Bind(fixture.loader, 1, &description, &bindHandle) == KernelError::kOk, "the bind must succeed");
        Require(DrvLoader::Start(fixture.loader, bindHandle) == KernelError::kOk, "the start must succeed");
        DrvLoader::Abort(fixture.loader);
        Require(driver.stopCalls == 1, "Abort must stop a started driver");
        Require(fixture.loader.bindingCount == 0 && fixture.loader.driverCount == 0, "Abort must clear both tables");
        BindingFacts facts{};
        Require(DevModel::Binding(fixture.device.state, 1, &facts) == KernelError::kNotFound, "Abort must clear the binding facts");
        uint32_t claimHandle = 0;
        Require(DevModel::Claim(fixture.device.state, 1, 0, &claimHandle) == KernelError::kOk, "Abort must release the claim");
    }

    // —— 夹具自检：契约常量与模块标识 ——
    // 生成头的槽数与接口标识与实现一致
    void CaseContractFacts() {
        Require(kDevModelSlotCountV1 == 6, "Dev.Model v1 must have six slots");
        Require(kDrvLoaderSlotCountV1 == 4, "Drv.Loader v1 must have four slots");
        Require(kDriveSdkVersion == 1, "the DriveSDK version must be one");
        Require(sizeof(DeviceFacts) == 32 && sizeof(DeviceResources) == 192 && sizeof(BindingFacts) == 48,
            "the contract structure sizes must match v1");
        Require(sizeof(DriverDesc) == 0x40, "DriverDesc v1 must be 64 bytes");
        Require(kDevModelSlotCountV2 == 7 && kDevModelCurrentVersion == 2, "Dev.Model v2 must have seven slots");
        Require(kDevModelExportCount == 2, "DevModel must export two interface versions");
    }

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // —— 伪 PCI 总线：枚举与 BAR ——
        cases.push_back({ "PCI：默认树枚举与首条事实", CaseEnumerateDefaultTree });
        cases.push_back({ "PCI：多功能展开与单功能限制", CaseMultiFunction });
        cases.push_back({ "PCI：总线号编码", CaseBusNumber });
        cases.push_back({ "PCI：容量截断与零容量", CaseTruncatedEnumeration });
        cases.push_back({ "PCI：头类型 1 与 2 的 BAR 数量", CaseHeaderTypes });
        cases.push_back({ "PCI：32 位内存 BAR 解码", CaseMemoryBar });
        cases.push_back({ "PCI：I/O BAR 解码", CaseIoBar });
        cases.push_back({ "PCI：64 位 BAR 成对解码", CaseMemoryBar64 });
        cases.push_back({ "PCI：未分配 BAR 标志", CaseUnassignedBar });
        cases.push_back({ "PCI：未实现与保留类型 BAR 跳过", CaseSkippedBars });
        cases.push_back({ "PCI：BAR 探测后恢复原值", CaseBarProbeRestores });
        // —— DevModel：初始化与记录 ——
        cases.push_back({ "DevModel：初始化记录与线索事实", CaseInitRecords });
        cases.push_back({ "DevModel：重复初始化幂等", CaseInitIdempotent });
        cases.push_back({ "DevModel：平台回调缺失", CaseInitBadPlatform });
        cases.push_back({ "DevModel：BootInfo 缺失", CaseInitMissingBootInfo });
        cases.push_back({ "DevModel：UEFI 路径与无线索回退", CaseClueFallbackKinds });
        cases.push_back({ "DevModel：线索不可读回退", CaseClueUnreadable });
        cases.push_back({ "DevModel：光盘线索排除 NVMe", CaseCdromCandidates });
        cases.push_back({ "DevModel：设备表截断标记", CaseDeviceTableFull });
        // —— DevModel：六槽 ——
        cases.push_back({ "DevModel：Enumerate 句柄与结束", CaseEnumerateSlot });
        cases.push_back({ "DevModel：Resources 资源事实", CaseResourcesSlot });
        cases.push_back({ "DevModel：Claim / Release 唯一控制者", CaseClaimRelease });
        cases.push_back({ "DevModel：BootCandidates 候选序列", CaseBootCandidates });
        cases.push_back({ "DevModel：Binding 登记 / 更新 / 清除", CaseBindingSlot });
        cases.push_back({ "DevModel：绑定期间拒绝归还控制权", CaseReleaseWhileBound });
        // —— DevModel v2：Register ——
        cases.push_back({ "DevModel v2：Register 登记与 Enumerate 可见", CaseRegisterDevice });
        cases.push_back({ "DevModel v2：Register 重复总线地址拒绝", CaseRegisterDuplicate });
        cases.push_back({ "DevModel v2：Register 字段与状态校验", CaseRegisterValidation });
        cases.push_back({ "DevModel v2：Register 设备表满", CaseRegisterTableFull });
        cases.push_back({ "DevModel v2：登记后认领与绑定闭环", CaseRegisterBindingLoop });
        // —— DriverLoader ——
        cases.push_back({ "DriverLoader：初始化与参数校验", CaseLoaderInit });
        cases.push_back({ "DriverLoader：注册合法与逐项负例", CaseRegisterDriver });
        cases.push_back({ "DriverLoader：注册容量上限", CaseRegisterCapacity });
        cases.push_back({ "DriverLoader：匹配绑定与重复绑定", CaseBindMatching });
        cases.push_back({ "DriverLoader：全通配匹配", CaseBindWildcard });
        cases.push_back({ "DriverLoader：Start 调用与状态推进", CaseStartBinding });
        cases.push_back({ "DriverLoader：Start 失败回滚", CaseStartFailure });
        cases.push_back({ "DriverLoader：Stop 回收与句柄失效", CaseStopBinding });
        cases.push_back({ "DriverLoader：未启动绑定的 Stop", CaseStopUnstarted });
        cases.push_back({ "DriverLoader：绑定记录同步", CaseBindingRecordSync });
        cases.push_back({ "DriverLoader：.drv 返回 NotSupported 与 WARN", CaseExternalDriver });
        cases.push_back({ "DriverLoader：Abort 回收", CaseLoaderAbort });
        // —— 契约自检 ——
        cases.push_back({ "契约：槽数与结构尺寸", CaseContractFacts });
        return cases;
    }
}

// 入口：跑全部用例并汇总；失败返回 1
int main() {
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckDevModel: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckDevModel: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckDevModel: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
