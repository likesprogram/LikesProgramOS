/* Handoff.hpp
    内核交接包的 9 步核对：清单副本、Modules.list 副本、预装记录与内核镜像事实的一致性

    核对顺序如下，全部解析有界（清单条目 ≤ 32、
    记录 ≤ 30、路径 ≤ 255 字节、列表 ≤ 16 KiB），不按内容长度分配：
      1  BootInfo 自身可读（指针、版本、长度、标志）——由 Core.cpp 在调用前完成，E-KRN-01..04
      2  存在性一致：三类缓冲的地址与长度都要给出，缺任何一项即交接包缺失（E-KRN-50）
      3  越界与溢出：三类缓冲与每个模块区间都要整段落在低 4GiB 直映射窗口内，64 位加法先查回绕
      4  清单副本结构：LPMS 头、保留字段、条目区与签名区边界（E-KRN-51/52，解析在 ManifestView）
      5  列表与清单对应：列表条目集合必须与清单 Type=2 条目集合逐一相等（E-KRN-54）；
         路径主名派生 ModuleId，命中生成期望集的是引导闭包成员，其余是运行期模块；
         预装记录只覆盖闭包条目，记录数 = 闭包成员数（E-KRN-53/54/55）
      6  主版本硬门：.os / 列表 / 每个预装模块的 ModuleVersionMajor 三者相等且非零（E-KRN-56）
      7  BuildId 一致：.os 的 BuildId 与清单 BuildId 逐字节相等（E-KRN-56）
      8  期望集完整性：每个预装模块的容器 ModuleId 命中生成期望集，缺 / 重 / 未知都报出（E-KRN-55）
      9  摘要策略：闭包摘要信任 Baleen 交接，本阶段只确认条目带摘要字段，不复算；装载器统一复算

    身份来源：引导闭包成员的判定来自「列表路径主名 → ModuleId」与生成期望集，运行期模块
    不需要预装字节；预装模块的容器 ModuleId（固定偏移 0x50）与列表路径派生的 ModuleId
    交叉核对，两条规则同源（Common::ModulePath）

    失败返回 E-KRN 编号与原因文本；不打印、不停机，打印与停机由 Core.cpp 决定
    交接区间的标占与复制由调用方按 facts 里的区间执行（本层只报告事实）
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <Diagnostics.hpp>
#include <ManifestView.hpp>

namespace Kernel {
    // 预装记录条数上限、单条记录长度与记录数组头长度：与交接包的字节布局一致
    constexpr uint32_t kHandoffMaxRecords = 30;
    constexpr uint32_t kHandoffRecordBytes = 16;
    constexpr uint32_t kHandoffHeaderBytes = 8;
    // Modules.list 副本容量上限：16 KiB（Baleen 侧按同一上限构造缓冲）
    constexpr uint32_t kHandoffListMaxBytes = 0x4000;
    // 模块容器里本阶段读的两个固定字段：ModuleId 与 ModuleVersionMajor
    constexpr uint32_t kModuleIdOffset = 0x50;
    constexpr uint32_t kModuleIdBytes = 16;
    constexpr uint32_t kModuleMajorOffset = 0x66;
    constexpr uint32_t kModuleFixedFieldsEnd = 0x68;

    // 预装模块记录：与交接包的 16 字节布局一一对应
    struct HandoffRecord {
        uint64_t phys = 0;           // 模块文件字节的物理地址（低 4GiB）
        uint32_t bytes = 0;          // 模块文件字节数
        uint16_t artifactIndex = 0;  // 清单产物条目下标（必须是 Type=2 的 .osm）
        uint16_t reserved = 0;       // 必须为 0
    };

    // 交接区间：起址与字节数，供页帧分配器标占与复制接管
    struct HandoffRange {
        uint64_t phys = 0;    // 物理起址
        uint32_t bytes = 0;   // 字节数
    };

    // 闭包成员下标哨兵：列表条目不是引导闭包成员（即运行期模块）
    constexpr uint16_t kNotClosureMember = 0xFFFF;

    // 模块列表视图：固定上界，路径指向列表副本内部的字节，不以空字符结尾
    // artifactIndex / closureMember 由 CheckHandoff 在列表与清单对应后填
    struct BootListView {
        uint16_t major = 0;                               // 列表主版本，必须等于 .os 的 ModuleMajor
        uint64_t revision = 0;                            // 模块修订号，非零
        uint32_t count = 0;                               // module 条目数，0..30
        const uint8_t* paths[kHandoffMaxRecords] = {};    // 每条条目的相对路径起点
        uint16_t pathBytes[kHandoffMaxRecords] = {};      // 每条条目的相对路径字节数
        uint16_t artifactIndex[kHandoffMaxRecords] = {};  // 对应的清单 Type=2 条目下标
        uint16_t closureMember[kHandoffMaxRecords] = {};  // 命中的闭包期望集下标；kNotClosureMember = 运行期条目
    };

    // 交接包事实：核对通过后交给后续阶段（标占、复制、状态播报与修订身份）
    struct HandoffFacts {
        ManifestView manifest;                                    // 清单副本视图
        BootListView list;                                        // 列表副本视图
        uint32_t closureCount = 0;                                // 列表里的引导闭包成员数，等于预装记录数
        uint16_t closureListIndex[kHandoffMaxRecords] = {};       // 闭包成员在列表中的下标，按列表顺序
        uint16_t closureRecordIndex[kHandoffMaxRecords] = {};     // 各闭包成员对应的预装记录下标
        uint32_t runtimeCount = 0;                                // 列表里的运行期模块数
        uint16_t runtimeListIndex[kHandoffMaxRecords] = {};       // 运行期条目在列表中的下标，按列表顺序
        uint32_t recordCount = 0;                                 // 预装记录条数
        HandoffRecord records[kHandoffMaxRecords] = {};           // 预装记录，前 recordCount 条有效
        HandoffRange manifestRange = {};                          // 清单副本区间
        HandoffRange listRange = {};                              // 列表副本区间
        HandoffRange recordsRange = {};                           // 记录数组区间（8 字节头 + 记录）
        HandoffRange moduleRanges[kHandoffMaxRecords] = {};       // 每个模块字节的区间
        uint8_t buildId[32] = {};                                 // .os 与清单一致的集标识
        uint16_t moduleMajor = 0;                                 // .os / 列表 / 模块一致的主版本
        uint64_t moduleRevision = 0;                              // 列表登记的模块修订号
        uint64_t moduleBytes = 0;                                 // 预装模块字节合计
        // 内核自有副本：复制接管之后由调用方填（复制到内核自有的 4 KiB 帧）
        // 副本区间非零时，manifest / list 视图应重新指向副本（字节相同，解析结果一致）
        HandoffRange manifestCopy = {};                           // 清单副本的内核自有区间
        HandoffRange listCopy = {};                               // 列表副本的内核自有区间
        HandoffRange recordsCopy = {};                            // 记录数组的内核自有区间
        HandoffRange moduleCopies[kHandoffMaxRecords] = {};       // 每个模块字节的内核自有区间
    };

    // 物理区间到可读指针的换算：内核按直映射窗口，宿主夹具直接返回测试缓冲的指针
    // 返回空表示区间不可读；调用方保证 bytes 是本次要读的字节数
    using PhysicalMap = const uint8_t* (*)(uint64_t physical, uint64_t bytes);

    // 内核侧的换算：区间必须非零、整段落在低 4GiB 直映射窗口内且不回绕，否则返回空
    const uint8_t* MapPhysical(uint64_t physical, uint64_t bytes);

    // 有界解析 Modules.list 副本：固定头 LPML 1 / major / revision 与 module 条目
    // 成功返回空指针；失败返回原因文本（不打印、不停机，编号由调用方按失败点选）
    const char* ParseBootList(const uint8_t* bytes, uint32_t length, BootListView& view);

    // 9 步核对（第 1 步由调用方完成）：成功返回空编号，失败返回对应 E-KRN 编号
    // osBuildId 是内核镜像数据里的 32 字节 BuildId，osModuleMajor 是 .os 的发布线主版本
    // reason 非空时接收失败原因文本（给 Fail 的 detail 用），成功时置空
    // 成功时 facts 填好视图、记录、区间与身份事实；失败时 facts 不保证可用
    Diagnostics::Diagnostic CheckHandoff(const Boot::Info& info, const uint8_t osBuildId[32], uint16_t osModuleMajor, HandoffFacts& facts, const char** reason, PhysicalMap map = MapPhysical);
}
