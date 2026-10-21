/* SelectSet.hpp
    内核集选版：读引导状态记录、按状态选出目标集、验证清单与签名、按需写回记录

    状态机语义见 Docs/Specs/内核集更新与回滚.md，记录形态见引导状态记录规格；
    只读介质与写失败按降级路径处理：不进行状态转换，选已知良好集，没有则启动缺省集
    缺省集与其它集一样验证清单；布局描述的内核路径推不出 KernelSets/<BuildId>/ 形态一律拒绝，
    装载路径取自验签通过的清单 .os 条目，不存在未验证的直启回退
    返回空 Diagnostic 表示已定下目标；损坏记录、布局路径非法与集验证失败返回致命原因
*/

#pragma once
#include <stdint.h>

#include <Diagnostics.hpp>
#include <Storage/BlockDevice.hpp>
#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        // 路径缓冲长度：集根（约 40 字节）+ 集名（64）+ 产物名（16）充裕
        constexpr uint32_t kSetPathBytes = 512;

        // 选版结果：目标集的路径与身份
        struct SelectedSet {
            char kernelPath[kSetPathBytes];      // 内核镜像路径：集目录 + 验签通过的清单 .os 条目相对路径
            char setDirectory[kSetPathBytes];    // 集目录（清单与产物所在）
            uint8_t buildId[32];                 // 集标识；全零表示没有集身份
            bool verified;                       // 集清单与签名已验证通过（选版成功时恒为真）
            uint64_t attemptId;                  // 试启动身份；0 表示本次不是试启动
        };

        // 选版：读状态记录、验证目标集、按需写回状态记录
        // layoutKernelPath 是布局描述给出的缺省集内核路径，必须是 KernelSets/<BuildId>/<相对路径> 形态，
        // 且相对路径与清单登记的 .os 条目一致；推不出形态一律拒绝，没有未验证的直启回退
        // layoutFlags 是布局描述的标志位：位 0 置位时，记录损坏也退到缺省集（默认不置位，见 LayoutDescriptor）
        Diagnostic SelectKernelSet(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* layoutKernelPath, uint32_t layoutFlags, SelectedSet& selected);
    }
}
