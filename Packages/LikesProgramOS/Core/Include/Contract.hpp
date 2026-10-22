/* Contract.hpp
    内核侧契约常量聚合：只 include 这里的生成头，内核源码不散落契约头

    生成头由 Packages/Common/Out/Contract 提供（MakeContract 从 Interfaces.def 与
    BootClosure.def 生成），构建顺序由顶层 contract-generated 保证
    本头同时把内核依赖的契约形状用 static_assert 钉住：生成头变化时先在这里失败，
    不让错误形状流进注册表与装载器
*/

#pragma once
#include <stdint.h>

#include <Abi.hpp>
#include <BootClosure.hpp>
#include <Entry.hpp>
#include <Exports.hpp>
#include <Imports.hpp>
#include <InterfaceId.hpp>
#include <ModuleId.hpp>
#include <Slots.hpp>
#include <Types.hpp>

namespace Kernel {
    // 内核核心导出条目数下限：v1 的 10 个 Core.* 接口；新增接口不抬 ABI，只增条目
    static_assert(kKernelExportCount >= 10, "内核核心至少导出 v1 的 10 个 Core.* 接口");
    // 闭包单例数量下限：Dev.Model/Drv.Loader/Block.Registry/Fs.Vfs/Boot.Volume 五个 v1 单例；
    // 另加 Usb.Host 与 Dev.Model v2 后共 7 个，此后新增单例只增下限
    static_assert(kClosureProviderCount >= 5, "闭包单例必须至少覆盖 5 个");
    // 引导闭包成员固定 13 个：期望集与预装顺序同源
    static_assert(kBootClosureCount == 13, "引导闭包成员必须是 13 个");
    // 两个版本由生成头单点给出，内核不另写数字
    static_assert(kKernelAbiVersion >= 1, "内核 ABI 版本必须从生成头取得");
    static_assert(kDriveSdkVersion >= 1, "驱动契约版本必须从生成头取得");
    // Core.Abi 版本块必须至少含槽 0/1/2（生成头已逐槽固定，这里钉住内核依赖的形状）
    static_assert(kCoreAbiCurrentVersion >= 1, "Core.Abi 必须在 Interfaces.def 中定义");
    static_assert(kCoreAbiSlotCountV1 >= 3, "Core.Abi v1 的槽数必须不少于 3");
}
