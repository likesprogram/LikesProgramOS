/* ControllerPath.hpp
    UEFI 自建控制器路径：块协议缺失时，经 PCI I/O 协议定位启动控制器并只读访问整盘

    只用固件提供的最小总线能力（EFI_PCI_IO_PROTOCOL 的配置空间与 BAR 查询），
    控制器读路径复用内核侧的 AHCI / NVMe 只读实现（Packages/KernelModules），
    不建立设备模型、不写介质、不申报刷新原语

    绑定顺序（调用方决定何时进入本路径）：
      1. 从本映像的设备路径找到 PCI 节点，按前缀定位启动控制器句柄
      2. 取 EFI_PCI_IO_PROTOCOL，读 PCI 类码判定 AHCI / NVMe
      3. 使能 PCI 内存与总线主控，取 BAR 基址与长度
      4. 分配 DMA 页并初始化控制器（AHCI 端口 / NVMe 队列），读出块大小与容量
    任一步失败都记下阶段与说明，调用方按 ControllerStage 映射 E-UEFI-100..104

    内存与 DMA 假设：UEFI 启动服务阶段 x86_64 固件对物理内存做恒等映射，
    AllocatePages 返回的物理地址可直接当 CPU 指针，也可交给设备作 DMA 地址；
    这与内核侧模块的“连续物理页 + 物理地址”模型一致，启动服务退出后本对象不再使用
*/

#pragma once
#include <stdint.h>

#include <Ahci.hpp>
#include <EfiTypes.hpp>
#include <Nvme.hpp>
#include <Storage/BlockDevice.hpp>

namespace Baleen {
    namespace Uefi {
        // 自建控制器路径的失败阶段：调用方按它选 E-UEFI-100..104
        enum class ControllerStage : uint32_t {
            None = 0,       // 没有失败
            DevicePath,     // 本映像设备路径缺失，或路径里没有 PCI 节点
            PciIo,          // 启动控制器没有 EFI_PCI_IO_PROTOCOL
            Class,          // 控制器不是受支持的 AHCI / NVMe
            Bar,            // BAR 不可用或寄存器区长度不足
            Init,           // 控制器初始化或读块失败
        };

        // 自建控制器路径的只读块设备：读整盘，块大小来自控制器
        class ControllerBlockDevice final : public Storage::BlockDevice {
        public:
            // 绑定并初始化启动控制器；services 与 deviceHandle 由固件入口给出
            // 成功返回 Ok；失败返回非 Ok，阶段与说明用 Stage() / Detail() 取
            Storage::Error Bind(BootServices* services, EfiHandle deviceHandle);
            // 失败阶段；成功时是 None
            ControllerStage Stage() const { return m_stage; }
            // 失败说明：静态文本，给诊断 detail 用；成功时为空
            const char* Detail() const { return m_detail; }
            // 控制器类型名："AHCI" / "NVME"；未绑定返回空
            const char* ControllerName() const;
            // 控制器 Identify 型号字符串；未绑定或没有型号返回空串
            const char* Model() const { return m_model; }

            // 逻辑块大小，字节；未绑定返回 0
            uint32_t BlockBytes() const override { return m_blockBytes; }
            // 设备容量，字节；未绑定返回 0
            uint64_t CapacityBytes() const override { return m_capacityBytes; }
            // 自建路径按只读实现：引导阶段只读介质
            bool Writable() const override { return false; }
            // 读 [offset, offset + bytes)：偏移与长度必须按块对齐
            Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override;
            // 只读设备：写一律拒绝
            Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override;
            // 只读设备没有写缓存要刷新
            Storage::Error Flush() override;
            // 不申报刷新原语：写路径不在当前阶段范围
            bool HasFlushPrimitive() const override { return false; }

        private:
            // 分配一段连续物理页并同时给出 CPU 指针与物理地址
            bool AllocatePages(BootServices* services, uint32_t pages, uint8_t*& pointer, uint64_t& physical);
            // 从设备路径里找 PCI 节点并定位启动控制器句柄
            // 成功返回 None；失败返回 DevicePath（路径缺失 / 无 PCI 节点）或 PciIo（控制器没有 PCI I/O 协议）
            ControllerStage FindPciDevice(BootServices* services, EfiHandle deviceHandle, PciIoProtocol*& pciIo);
            // 读 BAR 资源描述符：成功时给出基址与长度
            bool ReadBar(PciIoProtocol* pciIo, uint32_t barIndex, uint64_t minimumBytes, uint64_t& begin, uint64_t& bytes);
            // 初始化 AHCI：分配 DMA 页、填 Controller、调用共享读路径的 Initialize
            bool InitializeAhci(BootServices* services);
            // 初始化 NVMe：分配队列与数据页、填 RegisterAccess 与 Controller、调用 Initialize
            bool InitializeNvme(BootServices* services);
            // 失败收尾：记录阶段与说明
            Storage::Error Fail(ControllerStage stage, const char* detail);

            BootServices* m_services = nullptr;   // 固件启动服务（不拥有）
            PciIoProtocol* m_pciIo = nullptr;     // PCI I/O 协议（不拥有）
            volatile uint32_t* m_bar = nullptr;   // 控制器 BAR 的 MMIO 指针
            uint64_t m_barBytes = 0;              // BAR 寄存器区长度
            uint32_t m_blockBytes = 0;            // 逻辑块大小
            uint64_t m_capacityBytes = 0;         // 设备容量
            ControllerStage m_stage = ControllerStage::None; // 失败阶段
            const char* m_detail = nullptr;       // 失败说明
            char m_model[41] = {};                // Identify 型号
            // 控制器运行期事实：AHCI 与 NVMe 只会有一个被初始化
            Ahci::Controller m_ahci = {};
            Nvme::Controller m_nvme = {};
        };
    }
}
