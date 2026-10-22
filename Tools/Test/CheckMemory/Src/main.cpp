/* main.cpp
    CheckMemory：引导期内存图判定、弹跳窗口搜索与内核页帧预留的宿主侧检查，手工执行，不构成默认构建依赖

    前半检查 Baleen 共用的那一份判定（Packages/Baleen/Common/Src/BootMemory.cpp）：
    Stub 用它挑装载 BaleenCore 的低地址弹跳窗口，BaleenCore 用它核对装载区，
    两阶段对同一张内存图必须给出同一个结论，所以边界与合并规则要在宿主侧钉死
    后半检查内核页帧分配器（Packages/LikesProgramOS/Core/Src/Memory.cpp）的交接区间预留与
    MarkRange 取整方向：预装模块字节与清单、列表、记录缓冲必须在首次 Allocate 之前保留住，
    装载器才不会把它们分配出去；标占用方向必须盖住区间碰到的每一页（含未对齐的内核 BSS 尾页），
    放行方向只放整页。内核源码按交付实现直接编译，直映射由 Include/Paging.hpp 的宿主替身头换算
    内存图条目的属性位过滤在 E820 读入侧完成（Stub 的 INT 15h 例程丢弃 bit0 为 0 的条目），
    本工具按判定拿到的形态构造条目：只判 type，不重复做那一层过滤
*/

#include <BootMemory.hpp>
#include <BootInfo.hpp>
#include <Memory.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Kernel {
    // 宿主侧 PhysToVirt 的偏移：0 表示物理地址就是宿主指针（见 Include/Paging.hpp）
    uintptr_t g_hostPhysBias = 0;
}

namespace {
    using Entry = Boot::MemoryMapEntry;
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 27;
    // 已跑用例数
    unsigned g_ran = 0;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }
    // 内存图条目：base、长度与类型，属性位按「应保留」填 1
    Entry Map(uint64_t base, uint64_t length, uint32_t type = 1) { return Entry{base, length, type, 1}; }
    // 把条目拼成一张图
    std::vector<Entry> Graph(std::initializer_list<Entry> entries) { return std::vector<Entry>(entries); }
    // 用一张图判定区间可用性
    bool Usable(const std::vector<Entry>& map, uint64_t begin, uint64_t bytes) {
        return Baleen::SpanUsable(map.data(), static_cast<uint32_t>(map.size()), begin, bytes);
    }
    // 用一张图找弹跳窗口
    uint32_t Window(const std::vector<Entry>& map, uint32_t begin = Baleen::kBounceBegin, uint32_t end = Baleen::kBounceEnd, uint32_t bytes = Baleen::kBounceBytes) {
        return Baleen::FindBounceWindow(map.data(), static_cast<uint32_t>(map.size()), begin, end, bytes);
    }
    // 地址的十六进制写法
    std::string Hex(uint64_t value) {
        static const char kDigits[] = "0123456789ABCDEF";
        std::string text = "0x";
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const unsigned digit = static_cast<unsigned>((value >> shift) & 0xF);
            if (digit != 0 || started || shift == 0) { text += kDigits[digit]; started = true; }
        }
        return text;
    }
    // 实机形态的内存图：低 64KiB 可用、0x9FC00 起保留、1MiB 起可用（其余按需补）
    std::vector<Entry> MachineGraph() {
        return Graph({
            Map(0x000000, 0x09FC00),        // 常规内存
            Map(0x09FC00, 0x000400, 2),     // EBDA
            Map(0x0F0000, 0x010000, 2),     // 固件区
            Map(0x100000, 0x3F00000),       // 1MiB 起的可用内存
        });
    }

    // 单条可用条目整段覆盖才算可用；两端各差一个字节都不可用
    void CaseSingleEntry() {
        const auto map = Graph({Map(0x10000, 0x20000)});
        Require(Usable(map, 0x10000, 0x20000), "整段覆盖必须可用");
        Require(Usable(map, 0x10000, 1), "区间起点与条目起点相同必须可用");
        Require(Usable(map, 0x2FFFF, 1), "区间落到条目最后一个字节必须可用");
        Require(!Usable(map, 0x0FFFF, 2), "区间起点在条目之前必须不可用");
        Require(!Usable(map, 0x2FFFF, 2), "区间末端超出条目必须不可用");
    }

    // 相邻的可用条目合并覆盖同一个区间；不相邻的条目拼不起来
    void CaseAdjacentMerge() {
        const auto adjacent = Graph({Map(0x10000, 0x10000), Map(0x20000, 0x10000)});
        Require(Usable(adjacent, 0x10000, 0x20000), "相邻两条必须合并覆盖");
        const auto gap = Graph({Map(0x10000, 0x10000), Map(0x20001, 0x10000)});
        Require(!Usable(gap, 0x10000, 0x20001), "中间差一个字节的空洞不得被跳过");
        const auto reserved = Graph({Map(0x10000, 0x10000), Map(0x20000, 0x10000, 2), Map(0x30000, 0x10000)});
        Require(!Usable(reserved, 0x10000, 0x30000), "中间夹保留条目不得合并");
    }

    // 条目顺序不作假定：倒序排列也要合并出同一结论
    void CaseOrderIndependent() {
        const auto forward = Graph({Map(0x30000, 0x10000), Map(0x40000, 0x10000), Map(0x50000, 0x10000)});
        const auto backward = Graph({Map(0x50000, 0x10000), Map(0x40000, 0x10000), Map(0x30000, 0x10000)});
        Require(Usable(forward, 0x30000, 0x30000), "顺序正序必须合并覆盖");
        Require(Usable(backward, 0x30000, 0x30000), "顺序倒序必须合并覆盖");
    }

    // 只有 type 为 1 的条目算可用，其他类型一律不参与合并
    void CaseOnlyTypeOneUsable() {
        const auto map = Graph({Map(0x10000, 0x10000, 0), Map(0x20000, 0x10000, 2), Map(0x30000, 0x10000, 3), Map(0x40000, 0x10000, 1)});
        Require(!Usable(map, 0x10000, 0x40000), "非 1 类型不得参与合并");
        Require(Usable(map, 0x40000, 0x10000), "类型 1 的条目必须可用");
    }

    // 退化输入：空图、零长度区间、空指针一律不可用
    void CaseDegenerateInputs() {
        const auto map = Graph({Map(0x10000, 0x10000)});
        Require(!Usable(map, 0x10000, 0), "零长度区间必须不可用");
        Require(!Baleen::SpanUsable(map.data(), 0, 0x10000, 1), "空图必须不可用");
        Require(!Baleen::SpanUsable(nullptr, 1, 0x10000, 1), "空指针必须不可用");
    }

    // 回绕：区间末端溢出与条目长度溢出都不得被当成覆盖
    void CaseWraparound() {
        const auto map = Graph({Map(0xFFFFFFFFFFFF0000ull, 0x20000)});
        Require(!Usable(map, 0xFFFFFFFFFFFF0000ull, 0x20000), "区间末端溢出必须不可用");
        const auto badEntry = Graph({Map(0xFFFFFFFFFFFF0000ull, 0x20000), Map(0x10000, 0x10000)});
        Require(!Usable(badEntry, 0x10000, 0x20000), "长度回绕的条目不得参与合并");
    }

    // 实机形态：返回第一段整段可用的窗口，即搜索起点
    void CaseMachineGraphPicksFirstWindow() {
        const auto map = MachineGraph();
        Require(Window(map) == Baleen::kBounceBegin, "实机形态必须取到搜索起点那一段窗口");
        Require(Usable(map, Baleen::kBounceBegin, Baleen::kBounceBytes), "取到的窗口必须整段可用");
    }

    // 窗口搜索：整段可用的窗口按步进取第一段，部分覆盖的窗口被跳过
    void CaseWindowSteps() {
        const auto map = Graph({Map(0x10000, 0x8000), Map(0x20000, 0x10000)});
        Require(Window(map) == Baleen::kBounceBegin + Baleen::kBounceBytes, "第一段只覆盖一半时必须取下一段");
        const auto first = Graph({Map(0x10000, Baleen::kBounceBytes)});
        Require(Window(first) == Baleen::kBounceBegin, "第一段完整时必须取第一段");
    }

    // 一个窗口由两条相邻的可用条目拼成：合并规则对窗口搜索同样成立
    void CaseWindowAcrossEntries() {
        const auto map = Graph({Map(0x10000, 0x8000), Map(0x18000, 0x8000)});
        Require(Window(map) == Baleen::kBounceBegin, "跨两条相邻条目的窗口必须可用");
    }

    // 搜索范围内整段保留：找不到窗口，返回 0（Stub 的 E-STUB-36 就是这条路径）
    void CaseNoWindowFound() {
        const auto reserved = Graph({Map(0x00000, 0x10000), Map(0x10000, 0x70000, 2), Map(0x80000, 0x20000), Map(0x100000, 0x3F00000)});
        Require(Window(reserved) == 0, "搜索范围整段保留时必须返回 0");
        const auto empty = Graph({});
        Require(Window(empty) == 0, "空图必须返回 0");
    }

    // 搜索上端不含：只从 kBounceEnd 起可用的图找不到窗口
    void CaseSearchEndExclusive() {
        const auto map = Graph({Map(Baleen::kBounceEnd, Baleen::kBounceBytes)});
        Require(Window(map) == 0, "搜索上端不含，从 kBounceEnd 起可用的窗口不得被选中");
    }

    // 参数退化：范围倒置、容不下一个窗口、窗口大小为零都不搜索
    void CaseDegenerateWindowArguments() {
        const auto map = MachineGraph();
        Require(Window(map, 0x20000, 0x10000) == 0, "范围倒置必须返回 0");
        Require(Baleen::FindBounceWindow(map.data(), static_cast<uint32_t>(map.size()), 0x10000, 0x18000, 0x10000) == 0, "范围容不下一个窗口必须返回 0");
        Require(Baleen::FindBounceWindow(map.data(), static_cast<uint32_t>(map.size()), 0x10000, 0x20000, 0) == 0, "窗口大小为零必须返回 0");
    }

    // 返回值不变量：非零结果必须窗口对齐、落在搜索范围内且整段可用
    void CaseResultInvariant() {
        const auto map = Graph({
            Map(0x00000, 0x10000),
            Map(0x10000, 0x0C000),
            Map(0x28000, 0x30000),
            Map(0x70000, 0x10000),
            Map(0x100000, 0x3F00000),
        });
        const uint32_t found = Window(map);
        Require(found != 0, "这张图里必须能找到窗口");
        Require(found >= Baleen::kBounceBegin && found + Baleen::kBounceBytes <= Baleen::kBounceEnd, "窗口必须落在搜索范围内：" + Hex(found));
        Require((found - Baleen::kBounceBegin) % Baleen::kBounceBytes == 0, "窗口必须按步进对齐：" + Hex(found));
        Require(Usable(map, found, Baleen::kBounceBytes), "返回的窗口必须整段可用：" + Hex(found));
    }

    // 判定与搜索同源：搜索接受的窗口，逐段可用性判定也必须接受
    void CaseSearchMatchesJudgement() {
        const auto map = Graph({Map(0x00000, 0x10000), Map(0x10000, 0x70000, 2), Map(0x80000, 0x80000), Map(0x100000, 0x3F00000)});
        Require(Window(map) == 0, "保留区内的窗口不得被选中");
        Require(!Usable(map, Baleen::kBounceBegin, Baleen::kBounceBytes), "保留区内的窗口在判定上也不可用");
        Require(Usable(map, 0x80000, Baleen::kBounceBytes), "保留区外的可用段判定必须成立");
    }

    // —— 内核页帧分配器（Packages/LikesProgramOS/Core/Src/Memory.cpp）——
    // 夹具常数：内存图 0..32MiB 全可用；内核跨度占 16MiB 起 1MiB，引导器跨度占 18MiB 起 1MiB
    // Initialize 还会把内核起点以下的全部地址标占，所以可用页是 32MiB 减去这三段
    constexpr uint64_t kFixtureKernelBegin = 0x1000000;
    constexpr uint64_t kFixtureKernelEnd = 0x1100000;
    constexpr uint64_t kFixtureLoaderBegin = 0x1200000;
    constexpr uint64_t kFixtureLoaderBytes = 0x100000;
    // 夹具空闲页数：8192 总页 - 4096（内核起点以下）- 256（内核跨度）- 256（引导器跨度）
    constexpr uint64_t kFixtureFreePages = 3584;
    // 夹具里第一块自由区起点：内核跨度之后、引导器跨度之前
    constexpr uint64_t kFixtureFirstFree = kFixtureKernelEnd;

    // 宿主侧内存图：一条 0..32MiB 的可用条目；静态存储保证地址在整个检查期间不变
    std::vector<Entry>& HostGraph() {
        static std::vector<Entry> graph = Graph({Map(0x0000000, 0x2000000)});
        return graph;
    }

    // 按夹具填 BootInfo 并初始化分配器：内存图地址是宿主 vector 的"物理地址"（替身头恒等换算）
    // 宿主地址都高于低 4GiB 的管理范围，Initialize 对 BootInfo 与内存图数组自身的 MarkUsed 是空操作
    uint64_t InitAllocator(Kernel::PageAllocator& allocator, Boot::Info& info) {
        std::vector<Entry>& graph = HostGraph();
        Require(reinterpret_cast<uintptr_t>(graph.data()) >= Kernel::kManagedLimitBytes, "宿主内存图地址必须高于管理范围");
        info = Boot::Info{};
        info.memoryMap = reinterpret_cast<uint64_t>(graph.data());
        info.memoryMapCount = static_cast<uint32_t>(graph.size());
        info.loaderBegin = kFixtureLoaderBegin;
        info.loaderBytes = kFixtureLoaderBytes;
        return allocator.Initialize(info, kFixtureKernelBegin, kFixtureKernelEnd);
    }

    // 初始化按内存图放行可用页，扣掉内核起点以下、内核跨度与引导器跨度；首次分配从自由区起
    void CaseAllocatorInitialize() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        const uint64_t free = InitAllocator(allocator, info);
        Require(free == kFixtureFreePages, "放行的空闲页数不符：" + Hex(free));
        Require(allocator.FreePages() == kFixtureFreePages, "FreePages 与 Initialize 返回值不一致");
        Require(allocator.ManagedPages() == 0x100000000ull / 0x1000ull, "管理的页数应为低 4GiB 的页数");
        const uint64_t first = allocator.Allocate(1);
        Require(first == kFixtureFirstFree, "首次分配必须落在内核跨度之后：" + Hex(first));
        allocator.Free(first, 1);
        Require(allocator.FreePages() == kFixtureFreePages, "归还后空闲页数必须复原");
    }

    // 首次 Allocate 之前预留：分配跳过保留页、空闲页数同步扣减，重复预留被拒绝
    void CaseReserveBeforeFirstAllocate() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        Require(allocator.Reserve(kFixtureFirstFree, 3 * 0x1000) == Kernel::ReserveResult::Ok, "首次分配前预留必须成功");
        Require(allocator.FreePages() == kFixtureFreePages - 3, "预留后空闲页数必须扣减");
        const uint64_t first = allocator.Allocate(1);
        Require(first == kFixtureFirstFree + 3 * 0x1000, "分配必须跳过预留区间：" + Hex(first));
        Require(allocator.FreePages() == kFixtureFreePages - 4, "分配后空闲页数继续扣减");
        Require(allocator.Reserve(kFixtureFirstFree, 3 * 0x1000) == Kernel::ReserveResult::Occupied, "同一区间重复预留必须拒绝");
        Require(allocator.Allocate(3) == kFixtureFirstFree + 4 * 0x1000, "后续分配仍然跳过保留页");
    }

    // ReleaseReserved 之后页面重新可分配，重复释放被拒绝
    void CaseReleaseReservedMakesAvailable() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        Require(allocator.Reserve(kFixtureFirstFree, 0x3000) == Kernel::ReserveResult::Ok, "预留必须成功");
        Require(allocator.ReleaseReserved(kFixtureFirstFree, 0x3000) == Kernel::ReserveResult::Ok, "释放预留必须成功");
        Require(allocator.FreePages() == kFixtureFreePages, "释放后空闲页数必须复原");
        Require(allocator.Allocate(1) == kFixtureFirstFree, "释放后的页必须能被再次分配");
        Require(allocator.ReleaseReserved(kFixtureFirstFree, 0x3000) == Kernel::ReserveResult::NotFound, "重复释放必须拒绝");
    }

    // 退化参数与对齐：零长度、未对齐一律拒绝且不改变状态
    void CaseReserveRejectsDegenerate() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        Require(allocator.Reserve(kFixtureFirstFree, 0) == Kernel::ReserveResult::ZeroLength, "零长度必须拒绝");
        Require(allocator.Reserve(kFixtureFirstFree + 1, 0x1000) == Kernel::ReserveResult::Unaligned, "未对齐起址必须拒绝");
        Require(allocator.Reserve(kFixtureFirstFree, 0x800) == Kernel::ReserveResult::Unaligned, "未对齐长度必须拒绝");
        Require(allocator.FreePages() == kFixtureFreePages, "被拒绝的预留不得改变空闲页数");
        Require(allocator.ReleaseReserved(kFixtureFirstFree + 1, 0x1000) == Kernel::ReserveResult::Unaligned, "未对齐释放必须拒绝");
        Require(allocator.ReleaseReserved(kFixtureFirstFree, 0) == Kernel::ReserveResult::ZeroLength, "零长度释放必须拒绝");
    }

    // 管理范围与 64 位加法：越界、跨上限、溢出都拒绝；上限内但不可用的页按占用拒绝
    void CaseReserveRejectsRangeOverflow() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        Require(allocator.Reserve(Kernel::kManagedLimitBytes, 0x1000) == Kernel::ReserveResult::OutOfRange, "管理范围之外必须拒绝");
        Require(allocator.Reserve(Kernel::kManagedLimitBytes - 0x1000, 0x2000) == Kernel::ReserveResult::OutOfRange, "跨管理上限必须拒绝");
        Require(allocator.Reserve(Kernel::kManagedLimitBytes - 0x1000, 0x1000) == Kernel::ReserveResult::Occupied, "上限内的不可用页按占用拒绝");
        Require(allocator.Reserve(0xFFFFFFFFFFFFF000ull, 0x2000) == Kernel::ReserveResult::Overflow, "起址加长度溢 64 位必须拒绝");
        Require(allocator.Reserve(0x7FFFFFFFFFFFF000ull, 0x8000000000001000ull) == Kernel::ReserveResult::Overflow, "大长度溢出必须拒绝");
        Require(allocator.ReleaseReserved(Kernel::kManagedLimitBytes, 0x1000) == Kernel::ReserveResult::OutOfRange, "越界释放必须拒绝");
        Require(allocator.ReleaseReserved(0xFFFFFFFFFFFFF000ull, 0x2000) == Kernel::ReserveResult::Overflow, "溢出释放必须拒绝");
        Require(allocator.FreePages() == kFixtureFreePages, "被拒绝的操作不得改变空闲页数");
    }

    // 重叠拒绝：内核跨度、内核以下的保留区、引导器跨度、内存图外、已分配页、已有预留
    void CaseReserveRejectsOccupied() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        Require(allocator.Reserve(kFixtureKernelBegin, 0x1000) == Kernel::ReserveResult::Occupied, "与内核跨度重叠必须拒绝");
        Require(allocator.Reserve(0x100000, 0x1000) == Kernel::ReserveResult::Occupied, "内核以下的保留区必须拒绝");
        Require(allocator.Reserve(kFixtureLoaderBegin, kFixtureLoaderBytes) == Kernel::ReserveResult::Occupied, "与引导器跨度重叠必须拒绝");
        Require(allocator.Reserve(0x2000000, 0x1000) == Kernel::ReserveResult::Occupied, "内存图外的页必须拒绝");
        const uint64_t page = allocator.Allocate(2);
        Require(allocator.Reserve(page, 0x2000) == Kernel::ReserveResult::Occupied, "已分配页必须拒绝");
        const uint64_t reserved = 0x1400000;
        Require(allocator.Reserve(reserved, 0x3000) == Kernel::ReserveResult::Ok, "夹具预留必须成功");
        Require(allocator.Reserve(reserved, 0x3000) == Kernel::ReserveResult::Occupied, "重复预留必须拒绝");
        Require(allocator.Reserve(reserved + 0x2000, 0x3000) == Kernel::ReserveResult::Occupied, "跨预留终点的重叠必须拒绝");
        Require(allocator.Reserve(reserved - 0x1000, 0x2000) == Kernel::ReserveResult::Occupied, "跨预留起点的重叠必须拒绝");
        Require(allocator.Reserve(reserved + 0x1000, 0x1000) == Kernel::ReserveResult::Occupied, "落在预留区间内的重叠必须拒绝");
    }

    // ReleaseReserved 只认完全相同的登记项：长度不符、起址不符、从未预留、内核与引导器跨度都不释放
    void CaseReleaseReservedRejectsUnknown() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        Require(allocator.Reserve(0x1400000, 0x2000) == Kernel::ReserveResult::Ok, "夹具预留必须成功");
        Require(allocator.ReleaseReserved(0x1400000, 0x1000) == Kernel::ReserveResult::NotFound, "长度不符必须拒绝");
        Require(allocator.ReleaseReserved(0x1410000, 0x2000) == Kernel::ReserveResult::NotFound, "起址不符必须拒绝");
        Require(allocator.ReleaseReserved(0x1500000, 0x1000) == Kernel::ReserveResult::NotFound, "从未预留的区间必须拒绝");
        Require(allocator.ReleaseReserved(kFixtureKernelBegin, 0x1000) == Kernel::ReserveResult::NotFound, "内核跨度不是预留项");
        Require(allocator.ReleaseReserved(kFixtureLoaderBegin, kFixtureLoaderBytes) == Kernel::ReserveResult::NotFound, "引导器跨度不是预留项");
        Require(allocator.ReleaseReserved(0x1400000, 0x2000) == Kernel::ReserveResult::Ok, "登记项本身必须释放成功");
        Require(allocator.ReleaseReserved(0x1400000, 0x2000) == Kernel::ReserveResult::NotFound, "释放后登记项必须消失");
        Require(allocator.FreePages() == kFixtureFreePages, "释放后空闲页数必须复原");
    }

    // 预留表容量：16 项都成功，第 17 项返回 NoRoom；释放一项后可以再预留
    void CaseReservationTableFull() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        for (uint32_t index = 0; index < Kernel::kMaxReservations; ++index) {
            Require(allocator.Reserve(kFixtureFirstFree + index * 0x1000, 0x1000) == Kernel::ReserveResult::Ok, "预留表内的项必须成功");
        }
        Require(allocator.Reserve(0x1500000, 0x1000) == Kernel::ReserveResult::NoRoom, "预留表满必须返回 NoRoom");
        Require(allocator.ReleaseReserved(kFixtureFirstFree, 0x1000) == Kernel::ReserveResult::Ok, "释放一项必须成功");
        Require(allocator.Reserve(0x1500000, 0x1000) == Kernel::ReserveResult::Ok, "释放后必须能再预留");
        Require(allocator.FreePages() == kFixtureFreePages - Kernel::kMaxReservations, "空闲页数必须与有效预留项数一致");
    }

    // 重新 Initialize 清空预留表：旧预留失效，空闲页数按新的交接事实重置
    void CaseInitializeClearsReservations() {
        Kernel::PageAllocator allocator;
        Boot::Info info{};
        InitAllocator(allocator, info);
        Require(allocator.Reserve(0x1400000, 0x2000) == Kernel::ReserveResult::Ok, "夹具预留必须成功");
        Require(allocator.FreePages() == kFixtureFreePages - 2, "预留后的空闲页数不符");
        const uint64_t free = InitAllocator(allocator, info);
        Require(free == kFixtureFreePages, "重新初始化后空闲页数必须复位");
        Require(allocator.ReleaseReserved(0x1400000, 0x2000) == Kernel::ReserveResult::NotFound, "重新初始化后旧预留必须失效");
    }

    // —— MarkRange 取整方向：标占用方向盖住区间碰到的每一页（含未对齐的尾页）——

    // 用自定义内存图与交接事实初始化分配器，返回放行的空闲页数
    // mapAddress 为 0 时内存图地址取宿主 vector 的指针；非 0 时把它当低「物理地址」，由
    // g_hostPhysBias 把该地址映射到宿主 vector，用来构造尾页跨页且未对齐的内存图数组
    uint64_t InitAllocatorWith(std::vector<Entry>& map, Kernel::PageAllocator& allocator,
                               uint64_t kernelBegin, uint64_t kernelEnd,
                               uint64_t loaderBegin, uint64_t loaderBytes, uint64_t mapAddress) {
        Boot::Info info{};
        info.memoryMap = mapAddress != 0 ? mapAddress : reinterpret_cast<uint64_t>(map.data());
        info.memoryMapCount = static_cast<uint32_t>(map.size());
        info.loaderBegin = loaderBegin;
        info.loaderBytes = loaderBytes;
        Kernel::g_hostPhysBias = mapAddress != 0 ? reinterpret_cast<uintptr_t>(map.data()) - mapAddress : 0;
        const uint64_t free = allocator.Initialize(info, kernelBegin, kernelEnd);
        Kernel::g_hostPhysBias = 0;
        return free;
    }

    // 内核 BSS 终址未对齐（0x103C018）：所在页 0x103C000 必须整个标占，不能被 Allocate 返回
    void CaseUsedDirectionCoversUnalignedKernelEnd() {
        std::vector<Entry> map = Graph({Map(0x0000000, 0x2000000)});
        Kernel::PageAllocator allocator;
        // 空闲页 = 8192 - 4096（内核起点以下）- 61（0x1000000..0x103D000 上取整后的内核跨度）
        const uint64_t free = InitAllocatorWith(map, allocator, 0x1000000, 0x103C018, 0, 0, 0);
        Require(free == 8192 - 4096 - 61, "内核跨度按终址上取整后空闲页数不符：" + Hex(free));
        Require(allocator.FreePages() == 8192 - 4096 - 61, "FreePages 与 Initialize 返回值不一致");
        // 若尾页漏标，首次适配会先返回 0x103C000
        Require(allocator.Allocate(1) == 0x103D000, "内核 BSS 尾页不得被分配出去");
    }

    // 引导器跨度两端都未对齐：首尾碰到的页都必须标占，中间页不得被误标
    void CaseUsedDirectionCoversUnalignedLoaderSpan() {
        std::vector<Entry> map = Graph({Map(0x100000, 0x100000)});
        Kernel::PageAllocator allocator;
        // 跨度 0x100FF8..0x101008（16 字节）：覆盖页 0x100000 与 0x101000 两页，其余 254 页仍空闲
        const uint64_t free = InitAllocatorWith(map, allocator, 0, 0, 0x100FF8, 0x10, 0);
        Require(free == 254, "引导器跨度按首尾取整后空闲页数不符：" + Hex(free));
        Require(allocator.Allocate(1) == 0x102000, "引导器跨度的首尾尾页都不得被分配出去");
    }

    // 内存图数组自身未对齐且跨页：数组碰到的两页都必须标占
    void CaseUsedDirectionCoversUnalignedMemoryMap() {
        // 内存图内容：一条 0x100000 起 1MiB 的可用条目；数组地址被摆到 0x100FF8 附近
        std::vector<Entry> map = Graph({Map(0x100000, 0x100000)});
        Kernel::PageAllocator allocator;
        // 数组 24 字节从 0x100FF8 起跨到 0x101010：覆盖页 0x100000 与 0x101000 两页
        const uint64_t free = InitAllocatorWith(map, allocator, 0, 0, 0, 0, 0x100FF8);
        Require(free == 254, "内存图数组按首尾取整后空闲页数不符：" + Hex(free));
        Require(allocator.Allocate(1) == 0x102000, "内存图数组的首尾尾页都不得被分配出去");
    }

    // 放行方向不受影响：只放整页，未对齐的起址与终址都把半页留在占用侧
    void CaseFreeDirectionRoundsToWholePages() {
        // 条目 0x100100..0x102100 只完整覆盖页 0x101000 一页
        std::vector<Entry> map = Graph({Map(0x100100, 0x2000)});
        Kernel::PageAllocator allocator;
        const uint64_t free = InitAllocatorWith(map, allocator, 0, 0, 0, 0, 0);
        Require(free == 1, "放行方向只放整页，空闲页数不符：" + Hex(free));
        Require(allocator.Allocate(1) == 0x101000, "唯一放行的整页必须是 0x101000");
        Require(allocator.Allocate(1) == 0, "未对齐的首尾半页不得被放行");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckMemory: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 依次跑全部用例
    void Execute() {
        Case("单条条目整段覆盖", CaseSingleEntry);
        Case("相邻条目合并且不跨空洞", CaseAdjacentMerge);
        Case("条目顺序不影响结论", CaseOrderIndependent);
        Case("只有类型 1 算可用", CaseOnlyTypeOneUsable);
        Case("退化输入不可用", CaseDegenerateInputs);
        Case("回绕不得被当成覆盖", CaseWraparound);
        Case("实机形态取第一段窗口", CaseMachineGraphPicksFirstWindow);
        Case("窗口按步进取第一段完整者", CaseWindowSteps);
        Case("窗口可跨两条相邻条目", CaseWindowAcrossEntries);
        Case("搜索范围整段保留时找不到窗口", CaseNoWindowFound);
        Case("搜索上端不含", CaseSearchEndExclusive);
        Case("窗口参数退化不搜索", CaseDegenerateWindowArguments);
        Case("窗口结果不变量", CaseResultInvariant);
        Case("搜索与判定同源", CaseSearchMatchesJudgement);
        Case("页帧初始化放行与扣除", CaseAllocatorInitialize);
        Case("首次分配前预留生效", CaseReserveBeforeFirstAllocate);
        Case("释放预留后重新可分配", CaseReleaseReservedMakesAvailable);
        Case("预留退化参数与对齐拒绝", CaseReserveRejectsDegenerate);
        Case("预留越界与 64 位溢出拒绝", CaseReserveRejectsRangeOverflow);
        Case("预留重叠拒绝", CaseReserveRejectsOccupied);
        Case("释放未知预留拒绝", CaseReleaseReservedRejectsUnknown);
        Case("预留表容量边界", CaseReservationTableFull);
        Case("重新初始化清空预留", CaseInitializeClearsReservations);
        Case("内核 BSS 尾页未对齐不得分配", CaseUsedDirectionCoversUnalignedKernelEnd);
        Case("引导器跨度首尾未对齐不得分配", CaseUsedDirectionCoversUnalignedLoaderSpan);
        Case("内存图数组尾页不得分配", CaseUsedDirectionCoversUnalignedMemoryMap);
        Case("放行方向只放整页", CaseFreeDirectionRoundsToWholePages);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckMemory: " << g_ran << " 项用例全部通过" << std::endl;
    }
}

// 入口：跑全部用例
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string arg = argv[1];
            if (arg == "-h" || arg == "--help") {
                std::cout <<
                    "CheckMemory — 引导期内存图判定、弹跳窗口搜索与内核页帧预留的宿主侧检查\n"
                    "\n"
                    "用法：CheckMemory\n"
                    "\n"
                    "检查对象：\n"
                    "  Packages/Baleen/Common/Src/BootMemory.cpp：区间可用性判定（含相邻条目合并）\n"
                    "  与低地址弹跳窗口搜索，Stub 与 BaleenCore 共用同一份实现\n"
                    "  Packages/LikesProgramOS/Core/Src/Memory.cpp：页帧分配与交接区间预留，\n"
                    "  直映射由 Include/Paging.hpp 的宿主替身头恒等换算\n";
                return 0;
            }
            Fail("无法识别的参数：" + arg);
        }
        Execute();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckMemory: " << error.what() << '\n';
        return 1;
    }
}
