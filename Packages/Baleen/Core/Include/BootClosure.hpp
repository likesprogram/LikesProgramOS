/* BootClosure.hpp
    引导闭包预装的共享机制：列表规划与期望集核对、落点搜索、逐模块核对与读入、记录数组填写

    Baleen 在所选集的清单验签通过之后，
    解析 Modules.list（内核侧书写的完整模块加载列表），只把其中命中引导闭包期望集的成员逐个
    读进低 4GiB 内存并记录落点；只做最小读取面（头 0x00..0x14 的核对 + 定向读 0x66 的主版本门）、
    整文件 SHA-256 与清单核对，不解析容器内部、不重定位、不执行、不初始化模块、不写系统卷

    列表里的非闭包条目（运行期模块）由内核挂卷后按同一份列表装载：本模块跳过它们，既不预装、
    也不当作列表非法；闭包成员漏项或重复仍按列表非法拒绝（Baleen 交不出完整闭包就不能交接）

    本文件是 BIOS 与 UEFI 两侧共用的同一份实现：读文件走 Storage::FileSystem 抽象，
    落点分配由平台注入 SpanAllocator（BIOS 用内存图搜索，UEFI 用固件页分配），
    共享实现里不出现平台分支，两侧的差别只在注入哪一种分配器

    任何失败都返回非空 Diagnostic 且不留下可用的记录数组（不交接半套），
    调用方按所选集失败处理：候选集回退与终止启停由选择器负责

    记录数组与定宽布局：8 字节头（count u32 + 保留 u32）+ 每条 16 字节的
    phys / bytes / artifactIndex / reserved；count 等于引导闭包成员数（当前 13），
    数组静态留 32 槽冗余，count 上限按 30 校验
    清单副本与列表副本都由调用方静态持有并保持到交权：本模块只读，不复制、不改写
*/

#pragma once
#include <stdint.h>

#include <BootInfo.hpp>
#include <Diagnostics.hpp>
#include <Storage/FileSystem.hpp>

namespace Baleen {
    namespace Core {
        // Modules.list 的容量上限：按 30 条最长路径（255 字节）计算
        constexpr uint32_t kBootListMaxBytes = 0x4000;
        // 预装条目上限：清单 Type=2 条目的静态上限，记录数组按 32 槽预留、条目上限按 30 校验
        constexpr uint32_t kPreloadMaxEntries = 30;
        // 记录数组的静态槽位：数组留 32 槽冗余，count 上限另按 30 校验
        constexpr uint32_t kPreloadSlots = 32;
        // 记录数组的定宽布局：8 字节头 + 每条 16 字节记录
        constexpr uint32_t kPreloadArrayHeaderBytes = 8;
        constexpr uint32_t kPreloadRecordBytes = 16;
        constexpr uint32_t kPreloadArrayBytes = kPreloadArrayHeaderBytes + kPreloadSlots * kPreloadRecordBytes;
        // 模块落点与地址边界：4 KiB 对齐、低 4 GiB、避开低 1 MiB
        constexpr uint64_t kPreloadPageBytes = 0x1000;
        constexpr uint64_t kPreloadAddressLimit = 0x100000000ull;
        constexpr uint64_t kPreloadLowLimit = 0x100000ull;
        // 模块路径组合上限：集目录至多 254 字节 + '/' + 相对路径至多 255 字节 + 结尾零
        constexpr uint32_t kPreloadPathBytes = 512;
        // 最小读取面：头 0x00..0x14 与主版本门定向读取的 0x66
        constexpr uint32_t kModuleHeaderBytes = 0x14;
        constexpr uint16_t kModuleHeaderSize = 0x80;
        constexpr uint16_t kModuleFormatVersion = 1;
        constexpr uint32_t kModuleMajorOffset = 0x66;
        constexpr uint32_t kModuleMagicBytes = 8;
        // 容器文件头标记：字节 "LPOSMOD" 加终止零
        inline constexpr uint8_t kModuleMagic[kModuleMagicBytes] = { 0x4C, 0x50, 0x4F, 0x53, 0x4D, 0x4F, 0x44, 0x00 };

        // 预装模块记录：BootInfo 交接包里每条 16 字节
        struct PreloadRecord {
            uint64_t phys;           // 模块文件字节的物理地址（低 4GiB）
            uint32_t bytes;          // 模块文件字节数
            uint16_t artifactIndex;  // 清单产物条目下标（必须是 Type=2 的 .osm）
            uint16_t reserved;       // 必须为 0
        };

        // 记录数组头：随后是 count 条记录；count 等于引导闭包成员数，保留必须为 0
        struct PreloadArrayHeader {
            uint32_t count;     // 记录条数
            uint32_t reserved;  // 必须为 0
        };

        // 记录数组的静态形态：头 + 冗余槽位，由本模块静态持有并随 BootInfo 交接
        struct PreloadArray {
            PreloadArrayHeader header;              // 数组头
            PreloadRecord records[kPreloadSlots];   // 记录，前 count 条有效
        };

        // 清单 Type=2 产物条目视图：路径与摘要指向清单副本，索引是产物数组下标
        struct PreloadArtifact {
            uint32_t artifactIndex;  // 清单产物数组下标（0 起）
            uint16_t pathBytes;      // 相对路径字节数
            uint64_t dataBytes;      // 清单登记的字节数
            const uint8_t* path;     // 集内相对路径，指向清单副本，不以空字符结尾
            const uint8_t* digest;   // 32 字节 SHA-256，指向清单副本
        };

        // 预装来源：所选集的清单副本、列表副本与清单 Type=2 条目视图
        // 清单副本与列表副本都是清单验签 / 摘要核对时使用的那一份字节，使用期间须保持有效
        struct PreloadSource {
            const char* setDirectory;          // 集目录（相对卷根），模块路径 = 集目录 + '/' + 相对路径
            const uint8_t* manifest;           // 清单副本（签名值 64 字节已归零）
            uint32_t manifestBytes;            // 清单副本字节数
            const uint8_t* list;               // Modules.list 副本字节
            uint32_t listBytes;                // 列表副本字节数
            uint16_t moduleMajor;              // .os 发布线的 ModuleMajor，主版本硬门的基准
            const PreloadArtifact* artifacts;  // 清单 Type=2 条目视图，按产物数组顺序
            uint32_t artifactCount;            // 清单 Type=2 条目数
        };

        // 预装计划条目：一个引导闭包成员在列表条目与清单 Type=2 条目里的对应关系
        struct PreloadEntry {
            uint16_t artifactIndex;  // 清单产物条目下标
            uint16_t pathBytes;      // 相对路径字节数
            const uint8_t* path;     // 集内相对路径，指向清单副本，不以空字符结尾
            uint64_t dataBytes;      // 清单登记的字节数
            const uint8_t* digest;   // 32 字节 SHA-256，指向清单副本
        };

        // 预装计划：解析与核对后的引导闭包子集，按闭包成员在 Modules.list 中出现的顺序
        struct PreloadPlan {
            const uint8_t* manifest;                   // 清单副本
            uint32_t manifestBytes;                    // 清单副本字节数
            const uint8_t* list;                       // 列表副本
            uint32_t listBytes;                        // 列表副本字节数
            const char* setDirectory;                  // 集目录
            uint16_t moduleMajor;                      // 模块主版本硬门的基准
            uint64_t revision;                         // 列表登记的模块修订号
            uint32_t count;                            // 预装闭包成员数，等于记录数
            uint64_t moduleBytes;                      // 全部预装模块文件字节之和
            PreloadEntry entries[kPreloadMaxEntries];  // 闭包成员条目，按列表顺序
        };

        // 预装结果：交给 BootInfo 交接包的事实，三类缓冲地址都在低 4GiB
        struct PreloadResult {
            uint64_t manifestPhys;   // 清单副本物理地址
            uint32_t manifestBytes;  // 清单副本字节数
            uint64_t listPhys;       // 列表副本物理地址
            uint32_t listBytes;      // 列表副本字节数
            uint64_t recordsPhys;    // 记录数组物理地址（8 字节头 + count 条记录）
            uint32_t recordCount;    // 记录条数
            uint64_t moduleBytes;    // 全部模块字节之和，预装摘要行用
        };

        // 预装落点：物理地址与可直接写入的指针；引导期分页关闭或恒等映射，二者指向同一段内存
        struct PreloadSpan {
            uint64_t phys;   // 物理地址（低 4GiB）
            uint8_t* data;   // 可写指针
            uint32_t bytes;  // 落点长度
        };

        // 落点分配器：平台侧注入的分配策略，BIOS 用内存图搜索，UEFI 用固件页分配
        class SpanAllocator {
        public:
            // 分配一段 4 KiB 对齐、低 4 GiB、可写的落点；失败返回 false
            virtual bool Allocate(uint32_t bytes, PreloadSpan& span) { (void)bytes; (void)span; return false; }
            // 归还一段落点，用于失败回滚；内存图落点不可回收，空实现即可
            virtual void Release(const PreloadSpan& span) { (void)span; }
        protected:
            // 实例由平台侧静态持有，不经基类指针销毁，因此不设虚析构
            ~SpanAllocator() = default;
        };

        // 预装 sink：共享实现只通过它接触平台，不在共享代码里判断固件环境
        class PreloadSink {
        public:
            // 打开一个模块文件；成功时交出句柄
            virtual bool Open(const char* path, uint32_t& handle) { (void)path; (void)handle; return false; }
            // 查询文件字节数
            virtual bool Size(uint32_t handle, uint64_t& bytes) { (void)handle; (void)bytes; return false; }
            // 从 offset 起读至多 bytes 字节；失败返回 false，读不满时以 read 回报实际长度
            virtual bool Read(uint32_t handle, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) { (void)handle; (void)offset; (void)buffer; (void)bytes; (void)read; return false; }
            // 关闭一个已打开的模块文件
            virtual void Close(uint32_t handle) { (void)handle; }
            // 分配一段模块落点
            virtual bool Allocate(uint32_t bytes, PreloadSpan& span) { (void)bytes; (void)span; return false; }
            // 归还一段模块落点
            virtual void Release(const PreloadSpan& span) { (void)span; }
        protected:
            // 实例由平台侧静态持有，不经基类指针销毁，因此不设虚析构
            ~PreloadSink() = default;
        };

        // 存储抽象上的 sink：文件读取走 Storage::FileSystem，落点走注入的分配器
        // BIOS 与 UEFI 都构造它，两侧差异只在 SpanAllocator 的实现
        class StoragePreloadSink final : public PreloadSink {
        public:
            // fs 与 allocator 由调用方静态持有，使用期间须有效
            StoragePreloadSink(Storage::FileSystem& fs, SpanAllocator& allocator);
            // 打开：路径交给文件系统，并把文件事实缓存进句柄槽
            bool Open(const char* path, uint32_t& handle) override;
            // 文件字节数：返回打开时缓存的长度
            bool Size(uint32_t handle, uint64_t& bytes) override;
            // 读取：交给文件系统按字节读
            bool Read(uint32_t handle, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) override;
            // 关闭：清空句柄槽
            void Close(uint32_t handle) override;
            // 落点：交给注入的分配器
            bool Allocate(uint32_t bytes, PreloadSpan& span) override;
            // 归还：交给注入的分配器
            void Release(const PreloadSpan& span) override;
        private:
            // 打开的模块文件：句柄即槽位下标 + 1
            struct OpenFile {
                Storage::FileId id;  // 卷内对象标识
                uint64_t bytes;      // 文件字节数，打开时缓存
                bool used;           // 槽是否被占用
            };
            Storage::FileSystem& m_fs;       // 文件系统实现（不拥有）
            SpanAllocator& m_allocator;      // 落点分配器（不拥有）
            OpenFile m_files[kPreloadSlots]; // 打开的模块文件
        };

        // 落点搜索约束：内存图与必须避开的跨度
        struct SpanConstraints {
            const Boot::MemoryMapEntry* memoryMap;  // 固件内存图
            uint32_t memoryMapCount;                // 内存图条目数
            uint64_t loaderBegin;                   // 引导器自身装载跨度起址
            uint64_t loaderBytes;                   // 引导器自身装载跨度长度，0 表示不避开
            uint64_t kernelBegin;                   // 内核装载跨度起址
            uint64_t kernelBytes;                   // 内核装载跨度长度，0 表示不避开
            const PreloadSpan* taken;               // 已分配的落点，后续搜索必须避开
            uint32_t takenCount;                    // 已分配落点数量
        };

        // 内存图落点分配器：BIOS 侧注入，逐个模块在内存图里找低 4 GiB 的连续可用区间
        class MemoryMapAllocator final : public SpanAllocator {
        public:
            // 内存图与两个装载跨度由调用方给出，使用期间须有效；跨度为 0 表示调用方没有该事实
            MemoryMapAllocator(const Boot::MemoryMapEntry* memoryMap, uint32_t memoryMapCount, uint64_t loaderBegin, uint64_t loaderBytes, uint64_t kernelBegin, uint64_t kernelBytes);
            // 找落点：4 KiB 对齐、低 4 GiB，避开低 1 MiB、引导器、内核与已分配落点
            bool Allocate(uint32_t bytes, PreloadSpan& span) override;
            // 归还：内存图不回收，只从已分配表里移除
            void Release(const PreloadSpan& span) override;
        private:
            SpanConstraints m_constraints;      // 搜索约束
            PreloadSpan m_taken[kPreloadSlots]; // 已分配的落点
            uint32_t m_takenCount = 0;          // 已分配落点数量
        };

        // 解析 Modules.list 副本并核对清单条目与引导闭包期望集，产出预装计划
        // 列表结构、路径与清单不对应、闭包成员漏 / 重返回 E-96；主版本不符返回 E-95
        // 非闭包条目（运行期模块）不预装、不报错；成功时 plan 只含闭包成员
        // 成功时 plan 有效；失败时不产出可用的计划
        Diagnostic PlanPreload(const PreloadSource& source, PreloadPlan& plan);

        // 在内存图里为 bytes 找一段落点：4 KiB 对齐、低 4 GiB，避开低 1 MiB 与给定跨度
        // 相邻的可用条目按同一段连续内存合并，空洞不跨；条目按固件给出的顺序扫描
        // （E820 与 UEFI 内存图都按物理起址升序）；找不到返回 0
        // bytes 为 0、超出低 4 GiB 可容纳范围、内存图为空都返回 0
        uint64_t FindModuleSpan(const SpanConstraints& constraints, uint64_t bytes);

        // 逐模块核对并读入：最小读取面、主版本门、整文件 SHA-256，落在 sink 分配的落点上并填记录
        // 失败返回非空 Diagnostic，且记录数组头回到 0 条、已分配的落点已归还（不交接半套）
        // 成功时 result 给出的清单 / 列表 / 记录三类地址都在低 4 GiB
        Diagnostic PreloadModules(const PreloadPlan& plan, PreloadSink& sink, PreloadResult& result);

        // 按预装结果取记录数组视图：头之后的第一条记录，条数见 result.recordCount
        // 供诊断行与宿主检查读取，不做校验
        inline const PreloadRecord* Records(const PreloadResult& result) {
            const PreloadArrayHeader* header = reinterpret_cast<const PreloadArrayHeader*>(static_cast<uintptr_t>(result.recordsPhys));
            return reinterpret_cast<const PreloadRecord*>(header + 1);
        }

        // 记录格式是 BootInfo 交接包的一部分，长度与记录数在编译期固定
        static_assert(sizeof(PreloadRecord) == kPreloadRecordBytes, "预装记录必须是 16 字节");
        static_assert(sizeof(PreloadArrayHeader) == kPreloadArrayHeaderBytes, "记录数组头必须是 8 字节");
        static_assert(sizeof(PreloadArray) == kPreloadArrayBytes, "记录数组长度必须是 8 + 32 × 16 字节");
    }
}
