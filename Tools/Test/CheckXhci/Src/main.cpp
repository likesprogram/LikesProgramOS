/* main.cpp
    CheckXhci：xHCI 驱动的纯编码/解析函数检查

    Xhci.cpp 的硬件路径（寄存器读写、DMA、命令/事件环）要真机验证；可在宿主上钉死的是
    位域与换算：CAP 解析、TRB 构造（Setup/Data/Status/Normal/Link/四类命令）、PORTSC 解析
    与速度映射、命令 TRB 的槽位/端点字段；这些错了会让真机失败点不可见，因此逐项断言
    最后一行打印固定格式的用例计数
*/

#include <Xhci.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    // 用例总数：末尾与实跑数核对
    constexpr unsigned kCaseCount = 13;

    // 已跑用例数
    unsigned g_ran = 0;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 假 MMIO：一段 32 位寄存器数组，CAP 区按 xHCI 布局摆放
    struct FakeMmio {
        uint32_t registers[1024] = {};
        // 写字节
        void PutByte(uint32_t offset, uint8_t value) {
            uint8_t* bytes = reinterpret_cast<uint8_t*>(registers);
            bytes[offset] = value;
        }
        // 写 32 位字
        void Put32(uint32_t offset, uint32_t value) { registers[offset / 4] = value; }
        // 只读视图
        volatile const uint32_t* View() const { return registers; }
    };

    // 造一份 CAP 区：CAPLENGTH=0x40、HCIVERSION=0x0110、MaxSlots=16、MaxPorts=8、
    // AC64=1、CSZ=0、DBOFF=0x2000、RTSOFF=0x1000
    FakeMmio MakeCapabilities() {
        FakeMmio mmio;
        mmio.PutByte(0x00, 0x40);
        mmio.PutByte(0x02, 0x10);
        mmio.PutByte(0x03, 0x01);
        mmio.Put32(0x04, 0x08000010u);   // MaxPorts=8（bit24..31）、MaxSlots=16（bit0..7）
        mmio.Put32(0x10, 0x1u);          // AC64=1、CSZ=0
        mmio.Put32(0x14, 0x2000u);       // DBOFF
        mmio.Put32(0x18, 0x1000u);       // RTSOFF
        return mmio;
    }

    // CAP 解析：关键字段与偏移
    void CaseReadCapabilities() {
        FakeMmio mmio = MakeCapabilities();
        Xhci::Capabilities caps;
        Require(Xhci::ReadCapabilities(mmio.View(), caps), "合法 CAP 必须解析成功");
        Require(caps.caplength == 0x40, "CAPLENGTH 解析错误");
        Require(caps.version == 0x0110, "HCIVERSION 解析错误");
        Require(caps.maxSlots == 16, "MaxSlots 解析错误");
        Require(caps.maxPorts == 8, "MaxPorts 解析错误");
        Require(caps.ac64 == 1, "AC64 解析错误");
        Require(caps.contextBytes == 32, "CSZ=0 时上下文大小必须是 32");
        Require(caps.dboff == 0x2000, "DBOFF 解析错误");
        Require(caps.rtsoff == 0x1000, "RTSOFF 解析错误");
        // CSZ=1 → 上下文 64 字节
        mmio.Put32(0x10, 0x5u);
        Require(Xhci::ReadCapabilities(mmio.View(), caps) && caps.contextBytes == 64, "CSZ=1 时上下文大小必须是 64");
    }

    // CAP 非法：空指针、CAPLENGTH 过小、MaxSlots/MaxPorts 为 0
    void CaseReadCapabilitiesInvalid() {
        Xhci::Capabilities caps;
        Require(!Xhci::ReadCapabilities(nullptr, caps), "空 ABAR 必须拒绝");
        FakeMmio mmio = MakeCapabilities();
        mmio.PutByte(0x00, 0x10);
        Require(!Xhci::ReadCapabilities(mmio.View(), caps), "CAPLENGTH 小于 0x20 必须拒绝");
        FakeMmio noSlots = MakeCapabilities();
        noSlots.Put32(0x04, 0x08000000u);
        Require(!Xhci::ReadCapabilities(noSlots.View(), caps), "MaxSlots 为 0 必须拒绝");
        FakeMmio noPorts = MakeCapabilities();
        noPorts.Put32(0x04, 0x10u);
        Require(!Xhci::ReadCapabilities(noPorts.View(), caps), "MaxPorts 为 0 必须拒绝");
    }

    // Setup TRB：类型、IDT、TRT、8 字节长度与请求字
    void CaseSetupTrb() {
        const Xhci::Trb trb = Xhci::MakeSetupTrb(0x11223344u, 0x55667788u, 3u, 1u);
        Require(((trb.dw3 >> 10) & 0x3F) == Xhci::kTrbSetup, "Setup TRB 类型错误");
        Require((trb.dw3 & (1u << 6)) != 0, "Setup TRB 必须置 IDT（数据在 TRB 内）");
        Require(((trb.dw3 >> 16) & 0x3) == 3u, "Setup TRB 的 TRT 必须是 IN");
        Require(trb.dw2 == 8, "Setup TRB 的传输长度必须是 8");
        Require(trb.dw0 == 0x11223344u && trb.dw1 == 0x55667788u, "Setup TRB 必须承载 8 字节请求");
        Require((trb.dw3 & 1u) == 1u, "Setup TRB 的 cycle 位设置错误");
    }

    // Data TRB：方向位、类型与长度
    void CaseDataTrb() {
        const Xhci::Trb in = Xhci::MakeDataTrb(0x123456789ABCDEF0ull, 18, true, 0);
        Require(((in.dw3 >> 10) & 0x3F) == Xhci::kTrbData, "Data TRB 类型错误");
        Require((in.dw3 & (1u << 16)) != 0, "IN 方向必须置 DIR 位");
        Require(in.dw2 == 18, "Data TRB 长度错误");
        Require(in.dw0 == 0x9ABCDEF0u && in.dw1 == 0x12345678u, "Data TRB 物理地址拆分错误");
        const Xhci::Trb out = Xhci::MakeDataTrb(0, 512, false, 1);
        Require((out.dw3 & (1u << 16)) == 0, "OUT 方向不得置 DIR 位");
        Require((out.dw3 & 1u) == 1u, "Data TRB 的 cycle 位设置错误");
    }

    // Status TRB：IOC 与方向（读传输的 status 是 OUT，写传输是 IN）
    void CaseStatusTrb() {
        const Xhci::Trb outStatus = Xhci::MakeStatusTrb(false, 1);
        Require(((outStatus.dw3 >> 10) & 0x3F) == Xhci::kTrbStatus, "Status TRB 类型错误");
        Require((outStatus.dw3 & (1u << 5)) != 0, "Status TRB 必须置 IOC");
        Require((outStatus.dw3 & (1u << 16)) == 0, "读传输的 Status 阶段方向必须是 OUT");
        const Xhci::Trb inStatus = Xhci::MakeStatusTrb(true, 1);
        Require((inStatus.dw3 & (1u << 16)) != 0, "写传输的 Status 阶段方向必须是 IN");
    }

    // Normal TRB：类型、IOC 与长度
    void CaseNormalTrb() {
        const Xhci::Trb trb = Xhci::MakeNormalTrb(0x1000, 512, true, 1);
        Require(((trb.dw3 >> 10) & 0x3F) == Xhci::kTrbNormal, "Normal TRB 类型错误");
        Require((trb.dw3 & (1u << 5)) != 0, "Normal TRB 必须置 IOC");
        Require(trb.dw2 == 512, "Normal TRB 长度错误");
        Require(trb.dw0 == 0x1000 && trb.dw1 == 0, "Normal TRB 物理地址错误");
    }

    // Link TRB：TC 位、类型与环首指针
    void CaseLinkTrb() {
        const Xhci::Trb trb = Xhci::MakeLinkTrb(0x4000, 1);
        Require(((trb.dw3 >> 10) & 0x3F) == Xhci::kTrbLink, "Link TRB 类型错误");
        Require((trb.dw3 & (1u << 1)) != 0, "Link TRB 必须置 TC（回绕后切换 cycle）");
        Require(trb.dw0 == 0x4000 && trb.dw1 == 0, "Link TRB 必须指向环首");
    }

    // 命令 TRB：四类命令的类型、槽位与输入上下文指针
    void CaseCommandTrbs() {
        const Xhci::Trb enable = Xhci::MakeEnableSlotTrb(1);
        Require(((enable.dw3 >> 10) & 0x3F) == Xhci::kTrbEnableSlot, "Enable Slot 类型错误");
        const Xhci::Trb disable = Xhci::MakeDisableSlotTrb(3, 1);
        Require(((disable.dw3 >> 10) & 0x3F) == Xhci::kTrbDisableSlot, "Disable Slot 类型错误");
        Require(Xhci::TrbSlot(disable) == 3, "Disable Slot 的槽位字段错误");
        const Xhci::Trb address = Xhci::MakeAddressDeviceTrb(0x8000, 2, 1);
        Require(((address.dw3 >> 10) & 0x3F) == Xhci::kTrbAddressDevice, "Address Device 类型错误");
        Require(Xhci::TrbSlot(address) == 2, "Address Device 的槽位字段错误");
        Require(address.dw0 == 0x8000 && address.dw1 == 0, "Address Device 的输入上下文指针错误");
        const Xhci::Trb configure = Xhci::MakeConfigureEndpointTrb(0x9000, 4, 1);
        Require(((configure.dw3 >> 10) & 0x3F) == Xhci::kTrbConfigureEndpoint, "Configure Endpoint 类型错误");
        Require(Xhci::TrbSlot(configure) == 4, "Configure Endpoint 的槽位字段错误");
    }

    // PORTSC 解析：连接/启用/复位/速度与变化位
    void CasePortStatus() {
        const uint32_t present = Xhci::kPortscCcs | Xhci::kPortscPed | (3u << Xhci::kPortscSpeedShift);
        const uint32_t status = Xhci::PortStatusFromPortsc(present);
        Require((status & 0x1u) != 0, "CCS 必须映射为 Present");
        Require((status & 0x2u) == 0, "无变化位时不得映射 Changed");
        Require((status & 0x4u) != 0, "PED 必须映射为 Enabled");
        Require(((status >> 4) & 0xF) == Xhci::kSpeedHigh, "速度 3 必须映射为高速");
        const uint32_t changed = Xhci::PortStatusFromPortsc(Xhci::kPortscCsc | Xhci::kPortscCcs);
        Require((changed & 0x2u) != 0, "CSC 必须映射为 Changed");
        const uint32_t resetting = Xhci::PortStatusFromPortsc(Xhci::kPortscPr);
        Require((resetting & 0x8u) != 0, "PR 必须映射为复位中");
    }

    // 速度字段映射与端口寄存器偏移
    void CaseSpeedAndOffset() {
        Require(Xhci::SpeedFromPortsc(1u << Xhci::kPortscSpeedShift) == Xhci::kSpeedFull, "PSIV=1 必须映射为全速");
        Require(Xhci::SpeedFromPortsc(2u << Xhci::kPortscSpeedShift) == Xhci::kSpeedLow, "PSIV=2 必须映射为低速");
        Require(Xhci::SpeedFromPortsc(3u << Xhci::kPortscSpeedShift) == Xhci::kSpeedHigh, "PSIV=3 必须映射为高速");
        Require(Xhci::SpeedFromPortsc(4u << Xhci::kPortscSpeedShift) == Xhci::kSpeedSuper, "PSIV=4 必须映射为超速");
        Require(Xhci::SpeedFromPortsc(0) == 0, "PSIV=0 必须映射为未知");
        Require(Xhci::PortscOffset(1) == 0x400, "端口 1 的寄存器偏移错误");
        Require(Xhci::PortscOffset(3) == 0x420, "端口 3 的寄存器偏移错误");
    }

    // TRB 字段访问器：槽位/端点来自 DW3，完成码来自 DW2 bit31:24（事件 TRB 的布局）
    void CaseTrbAccessors() {
        Xhci::Trb trb;
        trb.dw3 = (5u << 24) | (3u << 16) | (1u << 10);
        Require(Xhci::TrbSlot(trb) == 5, "槽位字段解码错误");
        Require(Xhci::TrbEndpoint(trb) == 3, "端点字段解码错误");
        Xhci::Trb completion;
        completion.dw2 = 1u << 24;   // 完成码在 DW2 的 bit31:24
        completion.dw3 = 1u << 24;   // 槽位也在 DW3 的 bit31:24，不能被误当完成码
        Require(Xhci::TrbCompletionCode(completion) == 1, "完成码必须从 DW2 解码");
        Require(Xhci::TrbCompletionCode(Xhci::Trb{}) == 0, "全零 TRB 的完成码必须是 0");
    }

    // Ops 表：八项回调都非空
    void CaseOpsTable() {
        const UsbControllerOps* ops = Xhci::Ops();
        Require(ops != nullptr, "Ops 表不得为空");
        Require(ops->SubmitControl != nullptr && ops->SubmitBulk != nullptr && ops->Poll != nullptr,
                "传输/轮询回调不得为空");
        Require(ops->PortCount != nullptr && ops->PortStatus != nullptr && ops->PortReset != nullptr,
                "端口回调不得为空");
        Require(ops->EnableDevice != nullptr && ops->DisableDevice != nullptr, "设备启停回调不得为空");
    }

    // 输入上下文布局：上下文偏移与 Add Context Flags 位（QEMU 实测踩过：Add 位是 DCI 不是 DCI+1）
    void CaseInputContextLayout() {
        Require(Xhci::ContextOffset(0, 32) == 0, "输入控制上下文偏移必须是 0");
        Require(Xhci::ContextOffset(1, 32) == 32, "槽上下文偏移错误（CSZ=0）");
        Require(Xhci::ContextOffset(2, 32) == 64, "EP0 上下文偏移错误（CSZ=0）");
        Require(Xhci::ContextOffset(6, 32) == 192, "DCI 5 的上下文偏移错误（CSZ=0）");
        Require(Xhci::ContextOffset(1, 64) == 64, "槽上下文偏移错误（CSZ=1）");
        Require(Xhci::ContextOffset(6, 64) == 384, "DCI 5 的上下文偏移错误（CSZ=1）");
        Require(Xhci::kAddSlotContextFlag == 0x1, "A0 必须是槽上下文");
        Require(Xhci::kAddEp0ContextFlag == 0x2, "A1 必须是 EP0 上下文");
        Require(Xhci::EndpointAddFlag(1) == 0x2, "DCI 1 的 Add 位错误（EP0 固定 A1）");
        Require(Xhci::EndpointAddFlag(5) == 0x20, "DCI 5 的 Add 位必须是 bit5");
        Require((Xhci::kAddSlotContextFlag | Xhci::EndpointAddFlag(5)) == 0x21, "槽+端点 Add 位组合错误");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckXhci: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 依次跑全部用例
    void Execute() {
        Case("CAP 解析", CaseReadCapabilities);
        Case("CAP 非法输入拒绝", CaseReadCapabilitiesInvalid);
        Case("Setup TRB 编码", CaseSetupTrb);
        Case("Data TRB 编码", CaseDataTrb);
        Case("Status TRB 编码", CaseStatusTrb);
        Case("Normal TRB 编码", CaseNormalTrb);
        Case("Link TRB 编码", CaseLinkTrb);
        Case("命令 TRB 编码", CaseCommandTrbs);
        Case("PORTSC 解析", CasePortStatus);
        Case("速度映射与端口偏移", CaseSpeedAndOffset);
        Case("TRB 字段与完成码", CaseTrbAccessors);
        Case("输入上下文布局与 Add 位", CaseInputContextLayout);
        Case("Ops 表非空", CaseOpsTable);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckXhci: " << g_ran << "/" << kCaseCount << " cases passed" << std::endl;
    }
}

// 入口：跑全部用例
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string argument = argv[1];
            if (argument == "-h" || argument == "--help") {
                std::cout << "CheckXhci — xHCI 驱动纯编码/解析函数的宿主侧检查\n\n用法：CheckXhci\n";
                return 0;
            }
            Fail("无法识别的参数：" + argument);
        }
        Execute();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckXhci: " << error.what() << '\n';
        return 1;
    }
}
