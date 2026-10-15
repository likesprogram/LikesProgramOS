/* main.cpp
    CheckMemory：引导期内存图可用性判定与弹跳窗口搜索的宿主侧检查，手工执行，不构成默认构建依赖

    检查对象是 Baleen 共用的那一份判定（Packages/Baleen/Common/Src/BootMemory.cpp）：
    Stub 用它挑装载 BaleenCore 的低地址弹跳窗口，BaleenCore 用它核对装载区，
    两阶段对同一张内存图必须给出同一个结论，所以边界与合并规则要在宿主侧钉死
    内存图条目的属性位过滤在 E820 读入侧完成（Stub 的 INT 15h 例程丢弃 bit0 为 0 的条目），
    本工具按判定拿到的形态构造条目：只判 type，不重复做那一层过滤
*/

#include <BootMemory.hpp>
#include <BootInfo.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    using Entry = Boot::MemoryMapEntry;
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 14;
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
                    "CheckMemory — 引导期内存图判定与弹跳窗口搜索的宿主侧检查\n"
                    "\n"
                    "用法：CheckMemory\n"
                    "\n"
                    "检查对象是 Stub 与 BaleenCore 共用的 Packages/Baleen/Common/Src/BootMemory.cpp：\n"
                    "区间可用性判定（含相邻可用条目合并）与低地址弹跳窗口搜索\n";
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
