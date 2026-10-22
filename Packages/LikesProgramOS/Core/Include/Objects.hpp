/* Objects.hpp
    内核对象与归属登记的最小面：对象表、句柄与代际、资源区间记录、按所有者回收

    本层只做装载器建立归属上下文所需的机制，不实现完整 Objects——命名空间、用户句柄、
    引用计数与析构回调都不在范围内：
      装载器在模块 Init 前用 OwnerScope 把当前所有者切成该模块，模块经导入的
      Core.Object.Register 登记的资源因此自动记在它名下；Init 返回失败时用 ReleaseByOwner
      回收该模块的全部登记，Init 成功则离开 OwnerScope 恢复上一个所有者
      当前所有者默认是内核核心（全零 ModuleId），内核核心自己的登记走同一张表
    本层只记元数据、不释放内存：对象记录里的资源区间是归属事实，内存与绑定由调用方自己
    撤销（Abort 的语义就是"模块撤销自己登记的资源与绑定"）
    句柄代际：低 8 位是槽号、高 24 位是代际，代际从 1 起、0 永远表示无效句柄；槽位复用
    时代际递增，陈旧句柄在 Release / Generations / Owner 上都按无效处理
*/

#pragma once
#include <stdint.h>

#include <ModuleId.hpp>
#include <Slots.hpp>
#include <Types.hpp>

namespace Kernel {
    namespace Objects {
        // 对象表容量：闭包 13 个模块各登记少量资源，加装载器自身与运行期模块留余量
        constexpr uint32_t kMaxObjects = 128;
        // 句柄编码：低 8 位槽号、高 24 位代际，校验句柄就是比对这 24 位
        constexpr uint32_t kHandleIndexBits = 8;
        constexpr uint32_t kHandleIndexMask = 0xFF;
        // 代际上限：24 位编码能表示的最大代际，绕回时从 1 重新开始
        constexpr uint32_t kHandleGenerationMax = 0xFFFFFF;
        // 对象类型 0 保留为无效；其余取值由登记方解释（物理区间、绑定、设备等）
        constexpr uint32_t kKindInvalid = 0;

        // 资源区间：登记时一并记下的归属事实；bytes 为 0 表示该对象没有资源区间
        struct ResourceRange {
            uint64_t begin;   // 资源起始地址，或登记方自解释的值
            uint64_t bytes;   // 资源长度，字节；0 表示没有资源区间
        };

        // 登记结果：Ok 为成功，失败值可区分原因；诊断编号由调用方按失败点选，本层不产生编号
        enum class ObjectResult : uint32_t {
            Ok = 0,            // 成功
            InvalidArgument,   // 参数非法：空指针、kind 为 0、区间加法溢出
            InvalidHandle,     // 句柄无效或代际不匹配（陈旧句柄）
            TableFull,         // 对象表已满
            DuplicateObject,   // 同一 kind 与 value 已登记
            RangeConflict,     // 资源区间与既有登记重叠
        };

        // 登记对象：归属取当前所有者；range.bytes 非零时记录资源区间并做重叠检查
        // 成功时把句柄写进 handleOut；失败不改变对象表
        ObjectResult Register(uint32_t kind, uint64_t value, const ResourceRange& range, ObjectHandle* handleOut);
        // 注销对象：不释放内存，释放仍归调用方；句柄无效或代际不匹配返回 InvalidHandle
        ObjectResult Release(ObjectHandle handle);
        // 句柄当前代际：无效或陈旧句柄返回 0
        uint64_t Generations(ObjectHandle handle);
        // 查询对象归属：内核核心登记返回全零 ModuleId；失败不写 ownerOut
        ObjectResult Owner(ObjectHandle handle, ModuleId* ownerOut);
        // 按所有者回收：释放该所有者的全部对象，返回回收条数（模块 Abort 回滚用）
        uint32_t ReleaseByOwner(const ModuleId& owner);
        // 当前所有者：全零表示内核核心；装载器在模块 Init 前设置，Init 后恢复
        void SetCurrentOwner(const ModuleId& owner);
        // 当前所有者
        ModuleId CurrentOwner();
        // 已登记对象数：诊断与宿主检查用
        uint32_t Count();
        // 复位对象表：清空全部登记、当前所有者回到内核核心；只在启动早期与宿主用例调用
        void Reset();

        // 当前所有者的作用域切换：构造时切换并记住进入时的所有者，析构恢复
        class OwnerScope {
        public:
            // 把当前所有者切换为 owner，并记住进入时的所有者
            explicit OwnerScope(const ModuleId& owner);
            // 恢复进入时的所有者
            ~OwnerScope();
            OwnerScope(const OwnerScope&) = delete;
            OwnerScope& operator=(const OwnerScope&) = delete;

        private:
            ModuleId m_previous;   // 进入时的所有者
        };

        // Core.Object v1 的四个槽实现：签名必须与生成头 CoreObjectSlotsV1 的成员逐一相同，
        // 下面用 static_assert 在编译期钉住（不手写槽位结构，槽表类型直接取生成头）
        KernelError CoreObjectRegister(uint32_t kind, uint64_t value, uint32_t* handleOut);
        KernelError CoreObjectRelease(uint32_t handle);
        uint64_t CoreObjectGenerations(uint32_t handle);
        KernelError CoreObjectOwner(uint32_t handle, ModuleId* ownerOut);

        // 槽 0..3 的签名复核：函数指针类型与生成头的成员一致才算通过
        static_assert(SameContractType<decltype(&CoreObjectRegister), decltype(CoreObjectSlotsV1::Register)>::kValue, "Core.Object 槽 0 的签名必须与生成头一致");
        static_assert(SameContractType<decltype(&CoreObjectRelease), decltype(CoreObjectSlotsV1::Release)>::kValue, "Core.Object 槽 1 的签名必须与生成头一致");
        static_assert(SameContractType<decltype(&CoreObjectGenerations), decltype(CoreObjectSlotsV1::Generations)>::kValue, "Core.Object 槽 2 的签名必须与生成头一致");
        static_assert(SameContractType<decltype(&CoreObjectOwner), decltype(CoreObjectSlotsV1::Owner)>::kValue, "Core.Object 槽 3 的签名必须与生成头一致");

        // Core.Object v1 的槽实现表；inline constexpr 让导出表可以常量初始化：
        //   extern "C" const CoreObjectSlotsV1 _Contract_Export_CoreObject_v1 = kCoreObjectSlots;
        // 不能在目标侧运行期拷贝本表——内核不允许静态构造，拷贝会落到 .init_array 且永不执行
        inline constexpr CoreObjectSlotsV1 kCoreObjectSlots = { &CoreObjectRegister, &CoreObjectRelease, &CoreObjectGenerations, &CoreObjectOwner };
    }
}
