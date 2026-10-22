/* SelectSet.hpp
    内核集选版：读引导状态记录、按状态与模块修订选出目标集、验证清单与签名、按需写回记录

    两阶段选版：BeginSetSelection 完成四态状态机决策、目标修订的清单来源解析、清单/签名/
    防回滚验证与候选 Trying 写入，带回装载与预装需要的全部事实；装载或预装失败时由
    SelectAfterLoadFailure 写 Failed 并按记录回退已知良好集或上一修订，已知良好集失败终止。
    状态机按四态推进，记录形态与修订选择取记录里的回退目标修订号；
    只读介质与写失败按降级路径处理：不进行状态转换，选已知良好集，没有则启动缺省集
    缺省集与其它集一样验证清单；布局描述的内核路径推不出 KernelSets/<BuildId>/ 形态一律拒绝，
    装载路径取自验签通过的清单 .os 条目，不存在未验证的直启回退
    返回空 Diagnostic 表示已定下目标；损坏记录、布局路径非法与集验证失败返回致命原因
*/

#pragma once
#include <stdint.h>

#include <Diagnostics.hpp>
#include <ManifestVerify.hpp>
#include <Revocation.hpp>
#include <Storage/BlockDevice.hpp>
#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        // 路径缓冲长度：集根（约 40 字节）+ 集名（64）+ 产物名（16）充裕
        constexpr uint32_t kSetPathBytes = 512;

        // 交接事实：交给预装机制与 BootInfo 的选版材料
        // manifest 里的清单副本与列表副本是验证时的静态缓冲，交权前一直有效
        // 吊销事实按原始字段存放、不用 Revocation::Facts 本身：那个类型有默认成员初始化器
        // （非平凡默认构造），放进 SelectedSet 会让引导侧的全局 SelectedSet 需要动态初始化，
        // 而 BaleenCore 的链接布局不接受 .init_array——两者信息等价，转换在 FillSelected 里完成
        struct HandoffFacts {
            ManifestFacts manifest;                              // 清单副本、列表副本、列表产物视图、修订与主版本
            uint64_t revocationEpoch;                            // 本次选版合并后的吊销纪元
            uint64_t revocationFloor;                            // 本次选版合并后的最低允许安全版本
            uint8_t revokedKeyMask[Revocation::kKeyMaskBytes];   // 本次选版合并后的已吊销密钥位图
        };

        // 选版结果：目标集的路径、身份与交接事实
        struct SelectedSet {
            char kernelPath[kSetPathBytes];      // 内核镜像路径：集目录 + 验签通过的清单 .os 条目相对路径
            char setDirectory[kSetPathBytes];    // 集目录（清单与产物所在）
            uint8_t buildId[32];                 // 集标识；全零表示没有集身份
            bool verified;                       // 集清单与签名已验证通过（选版成功时恒为真）
            uint64_t attemptId;                  // 试启动身份；0 表示本次不是试启动
            uint64_t moduleRevision;             // 本次选中的模块修订号（所选列表的 revision，非零）
            bool snapshot;                       // 修订来自旧修订快照（Manifest.<rev>.mft）时为真
            HandoffFacts handoff;                // 交给预装与 BootInfo 的事实
        };

        // 第一阶段选版：读状态记录、按四态状态机决策、验证目标集的清单/签名/防回滚，
        // 候选通过时写 Trying（剩余次数减一、尝试身份取写入代次），再带回装载与预装所需事实
        // 目标修订按记录里的回退目标修订号解析清单来源：等于当前列表修订（或记录为 0）时取
        // 当前 Manifest.mft / Modules.list，否则取 Manifest.<rev>.mft / Modules.<rev>.list；
        // 两种来源都不可用即目标不可选，不猜、不跨修订拼接
        // layoutKernelPath 是布局描述给出的缺省集内核路径，必须是 KernelSets/<BuildId>/<相对路径> 形态，
        // 且相对路径与清单登记的 .os 条目一致；推不出形态一律拒绝，没有未验证的直启回退
        // layoutFlags 是布局描述的标志位：位 0 置位时，记录损坏也退到缺省集（默认不置位，见 LayoutDescriptor）
        Diagnostic BeginSetSelection(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* layoutKernelPath, uint32_t layoutFlags, SelectedSet& selected);

        // 第二阶段：第一阶段选中的集在装载或预装时失败后的回退
        // failed 是失败的那一次选版结果；preloadFailed 表示失败发生在预装阶段：
        // 候选集预装失败回退时输出 WARN E-CORE-98；failure 是失败的原始原因，已知良好集失败时
        // 原样返回给调用方停机（本函数不重复判定“为什么没起来”）
        // 候选失败：写 Failed（写不成只报 WARN 并按降级路径继续），回退已知良好集或上一修订；
        // 已知良好集失败、回退目标与失败候选是同一组合、回退目标不可验证时终止并返回原因
        // 成功时 selected 填好新的目标集（调用方据此重新装载），返回空 Diagnostic
        Diagnostic SelectAfterLoadFailure(Storage::FileSystem& fs, Storage::BlockDevice& medium, uint64_t stateOffset, uint32_t stateBytes, const char* layoutKernelPath, uint32_t layoutFlags, const SelectedSet& failed, bool preloadFailed, const Diagnostic& failure, SelectedSet& selected);
    }
}
