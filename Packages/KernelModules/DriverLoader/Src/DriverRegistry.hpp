/* DriverRegistry.hpp
    DriverLoader 模块的驱动注册、设备匹配、绑定与启停记录：Drv.Loader v1 四槽的实现

    平台能力经 SetPlatform 注入：Dev.Model 的导入槽表（枚举设备、认领、归还、绑定事实）
    与 Core.Log 的行写槽；宿主检查用假的 Dev.Model 槽表与日志收集器，目标侧由 Module.cpp
    接到真实导入回填区，本文件不包含内核私有头、不执行 IN/OUT、不动态分配
    匹配规则（DriverDesc v1）：busKind 0、classCode 0xFFFFFFFF、vendorId 0xFFFF、
    deviceId 0xFFFFFFFF 为通配；全部非通配字段相等才算匹配；一格设备同一时刻只允许
    一条绑定，绑定前先经 Dev.Model.Claim 认领唯一控制权
    绑定记录是模块内部事实，登记 / 更新 / 清除经 Dev.Model.Binding 槽的登记约定同步给
    DevModel（见 DevModel 的 DeviceModel.hpp），Vfs 的块设备层据此取绑定事实
    .drv（运行期驱动文件）当前不实现：LoadExternal 返回 kNotSupported 并写固定 WARN 行
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

namespace DrvLoader {
    // 驱动描述容量与绑定记录容量：定长表，超限返回 kOutOfResources
    constexpr uint32_t kMaxDrivers = 16;
    constexpr uint32_t kMaxBindings = 32;
    // DriveSDK 能力类别：当前只定义块设备
    constexpr uint32_t kOpsKindBlockDevice = 1;
    // 匹配通配值：与 DriverDesc v1 的字节表一致
    constexpr uint32_t kBusKindAny = 0;
    constexpr uint32_t kClassCodeAny = 0xFFFFFFFF;
    constexpr uint32_t kVendorIdAny = 0xFFFF;
    constexpr uint32_t kDeviceIdAny = 0xFFFFFFFF;
    // 绑定状态位：与 BindingFacts.state 约定一致，0 表示清除
    constexpr uint32_t kBindingStateBound = 0x1;
    constexpr uint32_t kBindingStateStarted = 0x2;
    // 认领标志：v1 只接受 0
    constexpr uint32_t kClaimFlagsNone = 0;
    // 串口断言用的固定英文标记：baleen-integration 的启动矩阵按整行匹配
    inline constexpr char kMarkerReady[] = "drvloader: driver loader ready";
    inline constexpr char kMarkerExternalUnsupported[] = "drvloader: external .drv loading is not supported";

    // 一条已登记的驱动描述
    struct DriverRecord {
        DriverDesc desc;      // 注册时复制的驱动描述
        uint32_t used;        // 1 表示有效
    };

    // 一条绑定记录：绑定句柄、设备句柄与驱动下标
    struct BindingRecord {
        uint32_t bindHandle;    // 绑定句柄，0 表示无效
        uint32_t deviceHandle;  // 设备句柄
        uint32_t driverIndex;   // 驱动记录下标
        uint32_t state;         // kBindingStateBound / kBindingStateStarted
        ModuleId driverModule;  // 驱动模块标识；v1 的 DriverDesc 不带模块标识，当前写零
        uint32_t used;          // 1 表示有效
    };

    // DriverLoader 模块状态：驱动与绑定表、句柄计数器与注入的平台回调
    struct State {
        DriverRecord drivers[kMaxDrivers];      // 驱动描述表
        BindingRecord bindings[kMaxBindings];   // 绑定记录表
        uint32_t driverCount;                   // 已登记驱动数
        uint32_t bindingCount;                  // 有效绑定数
        uint32_t nextBindHandle;                // 下一个绑定句柄，从 1 起
        uint32_t externalAttempts;              // .drv 装载尝试次数，供诊断
        const DevModelSlotsV1* devModel;        // Dev.Model 导入槽表
        void (*logLine)(void* context, const char* tag, const char* text);  // Core.Log 槽 1
        void* context;                          // 透传给日志回调的上下文
    };

    // 注入平台回调并清空表；重复调用等价于重新开始（Init 里只调一次）
    KernelError Init(State& state, const DevModelSlotsV1* devModel,
        void (*logLine)(void* context, const char* tag, const char* text), void* context);
    // 撤销：停止并清除全部绑定、归还全部认领；Init 失败与模块回滚都走这里
    void Abort(State& state);

    // Drv.Loader 槽 0：登记驱动描述；版本 / 类别 / 标志 / 回调缺失按对应错误拒绝
    KernelError RegisterDriver(State& state, const DriverDesc* description);
    // Drv.Loader 槽 1：把设备与已登记驱动匹配绑定；绑定前认领设备唯一控制权
    KernelError Bind(State& state, uint32_t deviceHandle, const DriverDesc* description, uint32_t* bindHandle);
    // Drv.Loader 槽 2：调用驱动的 start 回调并把绑定置为已启动
    KernelError Start(State& state, uint32_t bindHandle);
    // Drv.Loader 槽 3：调用驱动的 stop 回调、清除绑定并归还认领
    KernelError Stop(State& state, uint32_t bindHandle);
    // .drv 运行期驱动装载：当前不支持，返回 kNotSupported 并写固定 WARN 行
    KernelError LoadExternal(State& state, const char* path);
}
