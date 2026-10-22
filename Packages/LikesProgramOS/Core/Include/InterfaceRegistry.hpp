/* InterfaceRegistry.hpp
    内核接口注册表：导出登记、提供者唯一、导入版本选择与槽位回填

    各操作的判据：
      导出登记  内核核心的 Core.*（生成头 kKernelExports）与已装载模块的导出进入同一张表，
                同一 (InterfaceId, Version) 只能有一个提供者；重复即拒绝且不覆盖先登记者
      导入选择  在已登记导出里取 MinVersion <= Version <= MaxVersion 的最高版本；使用槽数
                不得超过所选版本的 SlotCount，超过即拒绝，不回退到低版本
      槽位回填  把提供者槽表的前 SlotCount 个函数指针写进消费者的回填区，调用点在装载编排
      ABI 版本  两个版本常量只从生成头 Abi.hpp 取；VerifyAbi 在运行时核对 Core.Abi 槽 0/2
                的实际返回值

    容量固定、无动态分配：导出条目最多 kRegistryMaxExports 条，模块 Abort 时用 Mark/Rollback
    撤销本次登记。注册表只回报 KernelError 与固定英文原因文本，不产生诊断编号
    宿主 CheckRegistry 复用同一份源码，用替身导出表驱动
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

namespace Kernel {
    // 注册表导出条目上限：内核核心 10 + 闭包模块 5 + 运行期模块余量
    constexpr uint32_t kRegistryMaxExports = 128;

    // 槽数上限与版本上限：与契约的槽数范围、版本哨兵一致
    constexpr uint16_t kRegistryMaxSlots = 64;
    constexpr uint16_t kRegistryUnlimitedVersion = 0xFFFF;

    // 注册表操作结果：错误码 + 固定英文原因文本（机制层不产生诊断编号）
    struct RegistryResult {
        KernelError error;    // 失败码，kOk 表示成功
        const char* reason;   // 固定英文原因，调用方作 detail；成功时为空
    };

    // 已登记的导出一条：条目字段与提供者模块标识
    struct RegisteredExport {
        ModuleExportEntry entry;  // 接口标识、版本、槽数与槽位表指针
        ModuleId provider;        // 提供者：内核核心为全零 kModuleKernelCore
    };

    // 接口注册表：固定静态容量，内核初始化与装载期单线程使用
    class InterfaceRegistry {
    public:
        // 清空注册表：内核初始化时调用一次
        void Reset();
        // 登记一组导出：整组原子，任一条非法或重复时整组拒绝且注册表保持调用前状态
        // provider 是提供者模块标识；闭包单例的提供者必须与生成头 kClosureProviders 一致
        RegistryResult RegisterExports(const ModuleExportEntry* entries, uint16_t count, const ModuleId& provider);
        // 导入版本选择：取区间内最高版本的导出条目；参数非法或没有满足区间的版本时失败
        RegistryResult SelectImport(const InterfaceId& id, uint16_t minVersion, uint16_t maxVersion, ModuleExportEntry& selected) const;
        // 槽位回填：把导出条目槽表的前 slotCount 个指针复制进消费者回填区
        static RegistryResult FillSlots(const ModuleExportEntry& selected, void** targetSlots, uint16_t slotCount);
        // 一站式导入解析：选择最高版本、校验槽数并把槽位指针回填到导入条目的回填区
        RegistryResult ResolveImport(const ModuleImportEntry& import) const;
        // 运行时核对 Core.Abi 导出：槽 0 返回值等于 kKernelAbiVersion、槽 2 等于 kDriveSdkVersion
        RegistryResult VerifyAbi() const;
        // 登记检查点：模块装载开始时取，Abort 时回滚本模块登记的导出
        uint32_t Mark() const;
        // 回滚到检查点：撤销 mark 之后登记的导出
        void Rollback(uint32_t mark);
        // 已登记导出条数
        uint32_t Count() const;
        // 按接口标识与版本查找：找不到返回空指针
        const RegisteredExport* Find(const InterfaceId& id, uint16_t version) const;
        // 内核 ABI 版本：只从生成头 Abi.hpp 取
        static constexpr uint16_t AbiVersion() { return kKernelAbiVersion; }
        // 驱动契约版本：只从生成头 Abi.hpp 取
        static constexpr uint16_t DriveSdkVersion() { return kDriveSdkVersion; }
    private:
        RegisteredExport m_exports[kRegistryMaxExports] = {};  // 已登记导出，前 m_count 条有效
        uint32_t m_count = 0;                                  // 已登记导出条数
    };

    // 登记内核核心导出表（生成头 kKernelExports）：内核装载器初始化时调用
    // CheckRegistry 用替身表走同一个 RegisterExports，不需要链接内核核心的槽表符号
    inline RegistryResult RegisterKernelCoreExports(InterfaceRegistry& registry) { return registry.RegisterExports(kKernelExports, kKernelExportCount, kModuleKernelCore); }
}
