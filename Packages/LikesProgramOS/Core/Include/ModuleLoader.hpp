/* ModuleLoader.hpp
    模块装载编排：把容器字节、已验证清单条目与平台能力编排成 11 步装载流程

    装载流程共 11 步，顺序与编号映射如下：
      1  摘要与来源    文件来自已验证清单 Type=2 条目、长度一致、SHA-256 相符；闭包模块还要有预装记录   E-KRN-58
      2  头与边界      容器头固定字段、表区边界与重叠、溢出检查（容器解析器负责）                      E-KRN-57
      3  段表          段类型/权限/对齐/内存与文件区间、ImageBytes 覆盖                                  E-KRN-57
      4  标识与版本    ModuleId 非零且未重复、模块版本合法；主版本硬门；KernelAbi 区间                    E-KRN-57 / 56 / 59
      5  导出/导入/依赖/重定位表结构与提供者唯一                                                         E-KRN-57 / 59
      6  映射          按 ImageBytes 取连续页作 Base，逐段拷贝、清零 BSS                                   E-KRN-57 / 59
      7  重定位        RELATIVE64 / RELATIVE32 写入 Base + Addend 并查溢出                                 E-KRN-57
      8  导入解析      按版本区间选择提供者并把槽位回填进模块指针槽                                          E-KRN-59
      9  依赖检查      依赖模块必须已 Initialized 且版本落在声明区间内                                      E-KRN-59
      10 归属上下文    Objects 把当前所有者切成该模块，模块登记的资源自动归它                                 E-KRN-59
      11 初始化        调用 Init；成功后发布导出并登记模块；失败且有 Abort 时调用 Abort 再回滚                 E-KRN-59

    映射与重定位的编号裁决：页帧分配不足（ImageError::NoPages，资源不够而不是容器有错）归 E-KRN-59；
    映射阶段的其余失败（ImageBytes、基址、段区间）与全部重定位失败归 E-KRN-57——E-KRN-57 的判定面
    就是容器、段、入口与重定位本身不合法
    本表把「归属上下文」单列为第 10 步：入口执行前必须建立归属，所以 Init 之前先切换
    Objects 所有者；第 11 步「激活」（线程、IRQ）不在当前范围，留给后续阶段

    任何模块代码在步骤 11 之前都不执行：入口只经 LoaderPlatform::CallEntry 调用，宿主检查用计数器验证
    失败不留下半成品：已登记导出按检查点回滚、模块登记的对象按所有者回收、映像页归还；
    注册表里保留一条 Failed 记录供诊断，不含可调用的导出

    平台差异（物理地址换算、页帧分配、入口调用）经 LoaderPlatform 注入：内核侧用
    KernelLoaderPlatform（直映射 + 页帧分配器 + 直接调用），宿主检查用替身实现
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <DependencyGraph.hpp>
#include <Diagnostics.hpp>
#include <Entry.hpp>
#include <Handoff.hpp>
#include <InterfaceRegistry.hpp>
#include <ManifestView.hpp>
#include <ModuleContainer.hpp>
#include <ModuleImage.hpp>
#include <ModuleRegistry.hpp>
#include <ModuleRelocation.hpp>

namespace Kernel {
    // 装载平台：把物理地址换算、页帧分配与入口调用从编排里分离
    // 映像映射与重定位不在这里实现：第 6/7 步用三个回调把它桥接成 ImagePlatform，
    // 内核与宿主检查因此跑的是同一份 MapModuleImage / ApplyModuleRelocations
    class LoaderPlatform {
    public:
        // 物理地址→可写指针；不可映射返回空
        virtual void* MapPhysical(uint64_t physical) = 0;
        // 分配 count 个连续页帧，返回物理起址；失败返回 0
        virtual uint64_t AllocatePages(uint32_t count) = 0;
        // 归还从 physical 起的 count 个页帧
        virtual void FreePages(uint64_t physical, uint32_t count) = 0;
        // 调用模块入口（base + entryOffset）；入口签名见 Entry.hpp
        virtual KernelError CallEntry(uint64_t base, uint32_t entryOffset, const KernelAbiContext& context) = 0;
    protected:
        // 实例由所在二进制静态持有，不经基类指针销毁
        ~LoaderPlatform() = default;
    };

    // 内核侧平台：直映射窗口 + 页帧分配器 + 直接按物理地址调用入口
    class KernelLoaderPlatform final : public LoaderPlatform {
    public:
        // 绑定页帧分配器；未绑定时分配失败（返回 0）
        void Bind(PageAllocator* pages) { m_pages = pages; }
        void* MapPhysical(uint64_t physical) override;
        uint64_t AllocatePages(uint32_t count) override;
        void FreePages(uint64_t physical, uint32_t count) override;
        KernelError CallEntry(uint64_t base, uint32_t entryOffset, const KernelAbiContext& context) override;

    private:
        PageAllocator* m_pages = nullptr;   // 页帧分配器（不拥有）
    };

    // 装载上下文：平台、注册表、模块表与本次启动的 .os 主版本
    struct LoaderContext {
        LoaderPlatform* platform = nullptr;       // 装载平台（不拥有）
        InterfaceRegistry* registry = nullptr;    // 接口注册表（不拥有）
        ModuleRegistry* modules = nullptr;        // 模块注册表（不拥有）
        uint16_t osModuleMajor = 0;               // .os 的 ModuleMajor，主版本硬门基准
        const Boot::Info* bootInfo = nullptr;     // Core.Boot 绑定用；可为空
    };

    // 单模块装载请求
    struct LoadRequest {
        const uint8_t* bytes = nullptr;               // 模块文件字节（容器起点）
        uint32_t bytesCount = 0;                      // 文件字节数
        const ManifestEntry* manifestEntry = nullptr; // 已验证清单条目：来源、长度与摘要的基准
        const HandoffFacts* handoff = nullptr;        // 引导闭包来源核对（列表与预装记录）；运行期模块为空
        bool closureModule = false;                   // 引导闭包模块：还必须在预装记录里
        uint32_t preloadOrder = 0;                    // Modules.list 预装顺序；0 = 未提供
    };

    // 装载结果：成功时 diagnostic.code 为空，state 为 Initialized
    struct LoadOutcome {
        Diagnostics::Diagnostic diagnostic = {};   // 失败编号；空表示成功
        const char* reason = nullptr;              // detail 文本（机制层原因，不含标点）
        uint64_t base = 0;                         // 映像基址
        uint32_t imageBytes = 0;                   // 映像跨度
        uint32_t pageCount = 0;                    // 映像占用的页数
        ModuleState state = ModuleState::Empty;    // 结束状态：Initialized 或 Failed
    };

    // 11 步装载：成功返回空编号并把模块登记为 Initialized；失败返回编号与原因
    // 失败时映像页、导出登记与对象归属都已回滚，注册表只留一条 Failed 记录
    Diagnostics::Diagnostic LoadModule(const LoaderContext& context, const LoadRequest& request, LoadOutcome& outcome);

    // 依赖拓扑排序：把 DependencyGraph 的拒绝分类映射到装载编号
    // 结构类（模块列表/依赖表非法）归 E-KRN-57，依赖解析类（缺提供者/版本不匹配/环）归 E-KRN-59
    Diagnostics::Diagnostic SortModulesForLoad(const Dependency::GraphInput& input, Dependency::TopologyResult& order, const char*& reason);
}
