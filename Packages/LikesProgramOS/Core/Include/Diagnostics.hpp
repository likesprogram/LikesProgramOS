/* Diagnostics.hpp
    内核侧失败诊断的稳定编号：编号与文本的配对，以及带编号诊断行的写法

    编号是工单、日志聚合与自动化回归引用的稳定标识：同一编号的含义一经发布不再改动，
    文本可以润色；前缀为 E-KRN，号段按下文分配
    本文件只做数据与写法：不判断何时打印，也不停机；等级的文本与语义沿用 Kernel::Tags
    单个失败点的现场用 detail 附在括号里；现场由多个字段组成时用 WriteDiagnosticPrefix
    先写编号行的开头，再由调用方追加字段并收尾（见 Core.cpp 的异常处理与启动来源 WARN）

    50–99 号段留给内核集身份/模块装载/设备/文件系统/调度与停机恢复：
    50–59 已分配（清单副本、预装记录、闭包完整性、BuildId 与主版本门、容器、摘要、
    必需模块初始化），60–79 已分配（设备模型/驱动、文件系统/VFS），
    80–83 已分配（运行期模块装载与可选降级），84–89 与 92–99 待分配；
    分配编号时在本文件追加常量并按号段顺序登记
*/

#pragma once
#include <stdint.h>

#include <Tags.hpp>

namespace Kernel {
    namespace Diagnostics {
        // 失败诊断：编号是稳定标识，文本是给人看的英文原因
        // code 为空表示成功；按值返回，文本允许指向运行期字符串（如共享实现给出的字段原因）
        struct Diagnostic {
            const char* code;   // 编号文本，含前缀与两位序号，如 "E-KRN-07"
            const char* text;   // 原因文本，不含标点；为空时诊断行只输出编号
        };

        // 入口与交接事实（00–09）
        constexpr Diagnostic kFailBootInfoNull{"E-KRN-01", "The BootInfo pointer is null"};
        constexpr Diagnostic kFailBootInfoVersion{"E-KRN-02", "The BootInfo version isn't supported"};
        constexpr Diagnostic kFailBootInfoBytes{"E-KRN-03", "The BootInfo is shorter than the fields the kernel reads"};
        constexpr Diagnostic kFailBootInfoFlags{"E-KRN-04", "The BootInfo flags aren't supported"};
        constexpr Diagnostic kFailImageBootInfoVersion{"E-KRN-05", "The kernel image asks for a BootInfo version this kernel doesn't build"};
        constexpr Diagnostic kFailFirmwareState{"E-KRN-06", "The firmware state isn't supported"};
        constexpr Diagnostic kFailUefiServicesActive{"E-KRN-07", "The UEFI boot services are still active and exiting them isn't implemented"};
        constexpr Diagnostic kFailCpuid{"E-KRN-08", "The CPU doesn't support CPUID"};

        // 内核镜像数据与链接布局一致性（10–19）
        constexpr Diagnostic kFailImageData{"E-KRN-10", "The kernel image data is invalid"};
        constexpr Diagnostic kFailImageBytes{"E-KRN-11", "The kernel image size doesn't match the linked image"};
        constexpr Diagnostic kFailImageMemory{"E-KRN-12", "The kernel memory span doesn't match the linked image"};
        constexpr Diagnostic kFailImageLoadAddress{"E-KRN-13", "Where the kernel runs doesn't match the image data load address"};
        constexpr Diagnostic kFailImageEntry{"E-KRN-14", "The kernel entry doesn't match the image data entry offset"};

        // BootInfo 启动来源与字段一致性（20–29；WARN 也可编号）
        constexpr Diagnostic kWarnBootInfoKernelPath{"E-KRN-20", "The BootInfo kernel path is inconsistent"};
        constexpr Diagnostic kWarnBootInfoDevicePath{"E-KRN-21", "The BootInfo device path is inconsistent"};
        constexpr Diagnostic kWarnBootInfoVolume{"E-KRN-22", "The BootInfo volume is inconsistent"};
        constexpr Diagnostic kWarnBootInfoStateArea{"E-KRN-23", "The BootInfo state area is inconsistent"};

        // 内存、页帧分配与页表（30–39）
        constexpr Diagnostic kFailBootPaging{"E-KRN-30", "Installing the boot page tables failed"};

        // 描述符表、中断与 CPU 异常（40–49）
        constexpr Diagnostic kFailDescriptorTables{"E-KRN-40", "Installing the descriptor tables failed"};
        constexpr Diagnostic kException{"E-KRN-41", "Exception"};

        // 内核集身份与模块装载（50–59）：交接包、清单副本、预装记录、闭包完整性、版本门、容器、摘要与模块初始化
        constexpr Diagnostic kFailHandoffMissing{"E-KRN-50", "The kernel set handoff package is missing"};
        constexpr Diagnostic kFailManifestCopy{"E-KRN-51", "The kernel set manifest copy is unreadable, out of range or has an invalid length"};
        constexpr Diagnostic kFailManifestFormat{"E-KRN-52", "The kernel set manifest copy format isn't supported"};
        constexpr Diagnostic kFailPreloadRecords{"E-KRN-53", "The preload record array is unreadable, has an invalid count or a bad record"};
        constexpr Diagnostic kFailPreloadEntry{"E-KRN-54", "A preload record doesn't match its manifest entry or the module load list"};
        constexpr Diagnostic kFailBootClosure{"E-KRN-55", "The boot closure isn't complete or the module load list is invalid"};
        constexpr Diagnostic kFailBuildId{"E-KRN-56", "The BuildId or the module major version gate failed"};
        constexpr Diagnostic kFailOsmContainer{"E-KRN-57", "The preloaded module container is invalid"};
        constexpr Diagnostic kFailModuleDigest{"E-KRN-58", "A module digest or its source doesn't match"};
        constexpr Diagnostic kFailModuleInit{"E-KRN-59", "A required module failed to initialize"};

        // 设备模型与驱动（60–69）：内核核心的设备/驱动调用点已接线；
        // 当前内核侧只有存储栈调用（70–79），本段先登记，触发点在各调用点注明
        constexpr Diagnostic kFailDevModelInterface{"E-KRN-60", "The device model interface is unavailable"};
        constexpr Diagnostic kFailDriverLoaderInterface{"E-KRN-61", "The driver loader interface is unavailable"};
        constexpr Diagnostic kFailBootDeviceDriver{"E-KRN-62", "The driver for the boot device isn't ready"};
        constexpr Diagnostic kFailDeviceFacts{"E-KRN-63", "The device facts are unavailable"};

        // 文件系统与 VFS（70–79）：内核核心挂载系统卷与读取清单的失败点
        // 存储层与 .osm 模块只回报 Storage::Error，编号由内核核心在调用点选
        constexpr Diagnostic kFailVolumeInterface{"E-KRN-70", "The filesystem or volume interface is unavailable"};
        constexpr Diagnostic kFailVolumeMount{"E-KRN-71", "Mounting the system volume failed"};
        constexpr Diagnostic kFailVolumeIdentity{"E-KRN-72", "The mounted volume doesn't match the boot volume"};
        constexpr Diagnostic kFailManifestPath{"E-KRN-73", "The kernel set manifest path is unavailable"};
        constexpr Diagnostic kFailManifestOpen{"E-KRN-74", "Opening the kernel set manifest failed"};
        constexpr Diagnostic kFailManifestRead{"E-KRN-75", "Reading the kernel set manifest failed"};
        constexpr Diagnostic kFailManifestMismatch{"E-KRN-76", "The manifest read from the system volume doesn't match the boot copy"};

        // 运行期模块装载与可选降级（80–83）
        // 运行期模块由内核核心在挂卷后按 Modules.list 读取：列表里不属于引导闭包期望集的条目即运行期模块；
        // 必需/可选按夹具约定用集内路径前缀承载（Modules/ = 必需，Modules/Optional/ = 可选），
        // 正式清单的 Type 语义留后续阶段；修订切换失败不单占新号（按 51/52/56 归属，detail 标修订号）
        constexpr Diagnostic kFailRuntimeLoad{"E-KRN-80", "Loading a runtime module failed"};
        constexpr Diagnostic kFailRuntimeInit{"E-KRN-81", "A required runtime module failed to initialize"};
        constexpr Diagnostic kFailRuntimeEntry{"E-KRN-82", "A runtime module entry is invalid"};
        // ISO9660 第二来源核对：光盘档从 ISO9660 目录项独立推出系统卷区段，与 BootInfo 事实比对
        // 两处结论不一致即拒绝挂载；光盘档找不到第二来源只 WARN，不改变挂载结论
        constexpr Diagnostic kFailIsoSourceMismatch{"E-KRN-77", "The ISO9660 second source doesn't match the boot volume facts"};
        constexpr Diagnostic kWarnIsoSourceMissing{"E-KRN-78", "The ISO9660 second source is unavailable"};

        constexpr Diagnostic kWarnRuntimeOptional{"E-KRN-83", "An optional runtime module failed and was skipped"};
        // 84–89 预留：运行期更新 / 热替换 / 更新器自身失败按失败点追加

        // 停机、重启与恢复要求本身（90–99）：等待时基与平台复位
        // 等待时基不可用、只做了有界退避：调用方按 WARN 报出，不改变停机与复位判定
        constexpr Diagnostic kWarnWaitFallback{"E-KRN-90", "The fatal wait time base is unavailable; the wait fell back to a bounded busy loop"};
        // 平台复位请求未生效：调用方按 FATAL 报出后停机，保留屏幕与串口上的诊断
        constexpr Diagnostic kFailReset{"E-KRN-91", "The platform reset requested by the fatal path didn't take effect"};

        // 写编号行的开头：标签 + 编号 + 文本，不跟换行；文本为空时只写编号
        // 现场由多个字段组成时（异常的向量与出错位置、启动来源的不一致字段），调用方随后用
        // Print 追加字段，最后自行写 "\r\n" 收尾；单个 detail 用下面的整行写法
        void WriteDiagnosticPrefix(Tags::Tag tag, const Diagnostic& diagnostic);

        // 写一整行带编号的诊断：标签 + 编号 + 文本；detail 非空时在文本后补 " (detail)"
        // detail 用于补充稳定枚举一类的现场（如字段原因、机制层回报的原因），不替代编号
        // 何时打印、打印后是否停机由调用方决定：本函数只负责写法
        void WriteDiagnostic(Tags::Tag tag, const Diagnostic& diagnostic, const char* detail = nullptr);
    }
}
