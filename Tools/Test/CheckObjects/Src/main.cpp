/* main.cpp
    CheckObjects：内核对象与归属登记最小面的宿主侧检查，手工执行，不构成默认构建依赖

    检查对象是内核交付实现 Packages/LikesProgramOS/Core/Src/Objects.cpp：固定对象表、句柄与
    代际、资源区间冲突检查、按所有者回收，以及 Core.Object v1 四个槽的实现表与错误码映射
    Objects.cpp 不依赖页表与汇编，宿主侧直接编译交付源码即可，不需要替身头
    槽表签名由生成头 CoreObjectSlotsV1 决定：本工具的用例经同一张表调用，不另写一份签名
*/

#include <Objects.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    using Kernel::Objects::ObjectResult;
    using Kernel::Objects::ResourceRange;
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 10;
    // 已跑用例数
    unsigned g_ran = 0;

    // Core.Object v1 的槽实现表：类型必须就是生成头的 CoreObjectSlotsV1，签名不手写第二份
    constexpr bool kSlotsAreFourV1 = sizeof(Kernel::Objects::kCoreObjectSlots) == 4 * sizeof(void*);
    static_assert(kSlotsAreFourV1, "Core.Object v1 必须是四个槽的实现表");

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

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

    // 模块标识文本：生成头的 Text 写 32 位小写十六进制
    std::string IdText(const ModuleId& id) {
        char text[33] = {};
        Text(id, text);
        return std::string(text);
    }

    // 两个模块标识是否相同
    bool SameId(const ModuleId& left, const ModuleId& right) { return left == right; }

    // 句柄的槽号（低 8 位，编码见 Objects.hpp）
    uint32_t SlotOf(ObjectHandle handle) { return handle & Kernel::Objects::kHandleIndexMask; }

    // 句柄的代际（高 24 位，编码见 Objects.hpp）
    uint32_t GenerationOf(ObjectHandle handle) { return handle >> Kernel::Objects::kHandleIndexBits; }

    // 每个用例先复位：对象表是模块级静态状态
    void Reset() { Kernel::Objects::Reset(); }

    // 登记一个对象并断言成功，返回句柄
    ObjectHandle MustRegister(uint32_t kind, uint64_t value, const ResourceRange& range = {}) {
        ObjectHandle handle = 0;
        const ObjectResult result = Kernel::Objects::Register(kind, value, range, &handle);
        Require(result == ObjectResult::Ok, "登记必须成功（kind=" + Hex(kind) + "，error=" + Hex(static_cast<uint32_t>(result)) + "）");
        Require(handle != 0, "成功登记的句柄必须非零");
        return handle;
    }

    // 登记成功：句柄非零、计入对象数、代际与归属可查；内核核心的归属是全零标识
    void CaseRegisterBasic() {
        Reset();
        Require(Kernel::Objects::Count() == 0, "复位后对象表必须为空");
        Require(SameId(Kernel::Objects::CurrentOwner(), kModuleKernelCore), "默认所有者必须是内核核心");
        const ObjectHandle handle = MustRegister(1, 0x1000, ResourceRange{0x20000, 0x1000});
        Require(Kernel::Objects::Count() == 1, "登记后对象数必须为 1");
        Require(SlotOf(handle) < Kernel::Objects::kMaxObjects, "句柄槽号必须在表内：" + Hex(handle));
        Require(GenerationOf(handle) == 1, "首次登记的代际必须从 1 开始：" + Hex(GenerationOf(handle)));
        Require(Kernel::Objects::Generations(handle) == 1, "Generations 必须返回当前代际");
        ModuleId owner = {};
        Require(Kernel::Objects::Owner(handle, &owner) == ObjectResult::Ok, "查询归属必须成功");
        Require(SameId(owner, kModuleKernelCore), "内核核心登记的归属必须是全零标识：" + IdText(owner));
        // 参数与区间溢出：被拒绝的登记不得改表
        ObjectHandle none = 0;
        Require(Kernel::Objects::Register(Kernel::Objects::kKindInvalid, 1, ResourceRange{}, &none) == ObjectResult::InvalidArgument, "kind 为 0 必须拒绝");
        Require(Kernel::Objects::Register(1, 2, ResourceRange{}, nullptr) == ObjectResult::InvalidArgument, "空句柄指针必须拒绝");
        Require(Kernel::Objects::Register(2, 3, ResourceRange{0xFFFFFFFFFFFFFFF0ull, 0x20}, &none) == ObjectResult::InvalidArgument, "资源区间加法溢出必须拒绝");
        Require(Kernel::Objects::Count() == 1, "被拒绝的登记不得改变对象表");
    }

    // 代际递增：槽位复用时代际加一，旧句柄在三个查询口上都按无效处理
    void CaseGenerationIncrements() {
        Reset();
        const ObjectHandle first = MustRegister(1, 0x10);
        Require(Kernel::Objects::Release(first) == ObjectResult::Ok, "释放必须成功");
        Require(Kernel::Objects::Count() == 0, "释放后对象数必须归零");
        Require(Kernel::Objects::Generations(first) == 0, "已释放句柄的代际必须为 0");
        Require(Kernel::Objects::Release(first) == ObjectResult::InvalidHandle, "重复释放必须拒绝");
        ModuleId owner = {};
        Require(Kernel::Objects::Owner(first, &owner) == ObjectResult::InvalidHandle, "已释放句柄的归属查询必须拒绝");
        // 同一槽位重新登记：槽号不变、代际加一，旧句柄失效
        const ObjectHandle second = MustRegister(1, 0x10);
        Require(SlotOf(second) == SlotOf(first), "同一槽位复用后槽号必须相同");
        Require(GenerationOf(second) == GenerationOf(first) + 1, "槽位复用时句柄代际必须递增");
        Require(second != first, "代际递增后句柄必须变化");
        Require(Kernel::Objects::Generations(first) == 0, "陈旧句柄的代际必须为 0");
        Require(Kernel::Objects::Release(first) == ObjectResult::InvalidHandle, "陈旧句柄不得释放新对象");
        // 连续复用的代际按次递增
        ObjectHandle previous = second;
        for (uint32_t round = 0; round < 3; ++round) {
            Require(Kernel::Objects::Release(previous) == ObjectResult::Ok, "复用轮的释放必须成功");
            const ObjectHandle next = MustRegister(1, 0x10);
            Require(GenerationOf(next) == GenerationOf(previous) + 1, "每轮复用代际必须加一");
            previous = next;
        }
        Require(Kernel::Objects::Count() == 1, "复用链结束后只应剩一条登记");
    }

    // 无效句柄：0、代际为 0、槽号越界、代际不符都按 InvalidHandle 处理，代际查询返回 0
    void CaseInvalidHandles() {
        Reset();
        const ObjectHandle live = MustRegister(1, 0x20);
        Require(Kernel::Objects::Generations(0) == 0, "句柄 0 的代际必须为 0");
        Require(Kernel::Objects::Release(0) == ObjectResult::InvalidHandle, "句柄 0 必须无效");
        Require(Kernel::Objects::Generations(0x00000005u) == 0, "代际 0 的句柄必须无效");
        const ObjectHandle outOfRange = static_cast<ObjectHandle>((static_cast<uint32_t>(1) << Kernel::Objects::kHandleIndexBits) | 0x80u);
        Require(Kernel::Objects::Generations(outOfRange) == 0, "槽号越界的句柄必须无效");
        Require(Kernel::Objects::Release(outOfRange) == ObjectResult::InvalidHandle, "槽号越界的句柄释放必须拒绝");
        const ObjectHandle stale = static_cast<ObjectHandle>((static_cast<uint32_t>(2) << Kernel::Objects::kHandleIndexBits) | SlotOf(live));
        Require(Kernel::Objects::Generations(stale) == 0, "代际不符的句柄必须无效");
        ModuleId owner = {};
        Require(Kernel::Objects::Owner(stale, &owner) == ObjectResult::InvalidHandle, "代际不符的归属查询必须拒绝");
        Require(Kernel::Objects::Owner(live, nullptr) == ObjectResult::InvalidArgument, "空归属指针必须拒绝");
    }

    // 重复注册：同一 kind 与 value 只允许一条；不同 value 或不同 kind 可以共存
    void CaseDuplicateRegistration() {
        Reset();
        MustRegister(7, 0xDEAD);
        ObjectHandle handle = 0;
        Require(Kernel::Objects::Register(7, 0xDEAD, ResourceRange{}, &handle) == ObjectResult::DuplicateObject, "同 kind 同 value 重复登记必须拒绝");
        Require(handle == 0, "失败的登记不得写句柄");
        const ObjectHandle otherValue = MustRegister(7, 0xBEEF);
        const ObjectHandle otherKind = MustRegister(8, 0xDEAD);
        Require(otherValue != 0 && otherKind != 0, "不同 value 与不同 kind 必须允许");
        Require(Kernel::Objects::Count() == 3, "对象数必须为 3");
        // 释放后可以重新登记同一 kind 与 value：登记事实已撤销
        Require(Kernel::Objects::Release(otherValue) == ObjectResult::Ok, "释放必须成功");
        MustRegister(7, 0xBEEF);
        Require(Kernel::Objects::Count() == 3, "重新登记后对象数必须回到 3");
    }

    // 资源区间：相交拒绝、相邻允许、零长度不参与检查、区间加法溢出拒绝
    void CaseRangeConflict() {
        Reset();
        MustRegister(1, 0x100, ResourceRange{0x10000, 0x4000});
        ObjectHandle handle = 0;
        Require(Kernel::Objects::Register(2, 0x101, ResourceRange{0x12000, 0x4000}, &handle) == ObjectResult::RangeConflict, "相交区间必须拒绝");
        Require(Kernel::Objects::Register(2, 0x101, ResourceRange{0x8000, 0x10000}, &handle) == ObjectResult::RangeConflict, "跨起点的区间必须拒绝");
        Require(Kernel::Objects::Register(2, 0x101, ResourceRange{0x13000, 0x2000}, &handle) == ObjectResult::RangeConflict, "跨终点的区间必须拒绝");
        Require(Kernel::Objects::Register(2, 0x101, ResourceRange{0, 0}, &handle) == ObjectResult::Ok, "零长度区间不参与检查");
        Require(Kernel::Objects::Register(3, 0x102, ResourceRange{0x14000, 0x1000}, &handle) == ObjectResult::Ok, "相邻区间必须允许");
        Require(Kernel::Objects::Register(4, 0x103, ResourceRange{0xC000, 0x4000}, &handle) == ObjectResult::Ok, "终点相接的区间必须允许");
        Require(Kernel::Objects::Register(5, 0x104, ResourceRange{0x20000, 0}, &handle) == ObjectResult::Ok, "长度为 0 的区间不参与检查");
        Require(Kernel::Objects::Count() == 5, "对象数必须为 5");
    }

    // 按所有者回收：只回收该模块的登记，别家的对象保留；重复回收返回 0
    void CaseOwnerReclaim() {
        Reset();
        const ModuleId ownerA = kModAtaPio;
        const ModuleId ownerB = kModVfs;
        ObjectHandle handlesA[3] = {};
        {
            Kernel::Objects::OwnerScope scope(ownerA);
            Require(SameId(Kernel::Objects::CurrentOwner(), ownerA), "作用域内当前所有者必须是 A");
            for (uint32_t index = 0; index < 3; ++index) handlesA[index] = MustRegister(1, 0x200 + index, ResourceRange{0x30000 + index * 0x1000ull, 0x1000});
        }
        {
            Kernel::Objects::OwnerScope scope(ownerB);
            for (uint32_t index = 0; index < 2; ++index) MustRegister(1, 0x300 + index);
        }
        Require(SameId(Kernel::Objects::CurrentOwner(), kModuleKernelCore), "作用域退出后必须恢复内核核心");
        Require(Kernel::Objects::Count() == 5, "两个所有者的对象必须共存");
        ModuleId owner = {};
        Require(Kernel::Objects::Owner(handlesA[0], &owner) == ObjectResult::Ok, "A 的对象必须可查");
        Require(SameId(owner, ownerA), "A 的对象归属必须是 A：" + IdText(owner));
        Require(Kernel::Objects::ReleaseByOwner(ownerA) == 3, "按所有者 A 回收的条数不符");
        Require(Kernel::Objects::Count() == 2, "回收 A 后只应剩 B 的 2 条");
        Require(Kernel::Objects::Generations(handlesA[0]) == 0, "被回收对象的句柄必须失效");
        Require(Kernel::Objects::ReleaseByOwner(ownerA) == 0, "重复回收必须返回 0");
        Require(Kernel::Objects::ReleaseByOwner(ownerB) == 2, "按所有者 B 回收的条数不符");
        Require(Kernel::Objects::Count() == 0, "全部回收后对象表必须为空");
    }

    // 所有者作用域：嵌套切换与退出恢复；显式设置与查询同源
    void CaseOwnerScopeNested() {
        Reset();
        const ModuleId ownerA = kModAtaPio;
        const ModuleId ownerB = kModVfs;
        {
            Kernel::Objects::OwnerScope outer(ownerA);
            Require(SameId(Kernel::Objects::CurrentOwner(), ownerA), "外层作用域内必须是 A");
            {
                Kernel::Objects::OwnerScope inner(ownerB);
                Require(SameId(Kernel::Objects::CurrentOwner(), ownerB), "内层作用域内必须是 B");
            }
            Require(SameId(Kernel::Objects::CurrentOwner(), ownerA), "内层退出后必须回到 A");
        }
        Require(SameId(Kernel::Objects::CurrentOwner(), kModuleKernelCore), "外层退出后必须回到内核核心");
        Kernel::Objects::SetCurrentOwner(ownerB);
        Require(SameId(Kernel::Objects::CurrentOwner(), ownerB), "显式设置当前所有者必须生效");
        Kernel::Objects::SetCurrentOwner(kModuleKernelCore);
    }

    // 对象表容量：kMaxObjects 条都成功，下一步 TableFull；释放一条后可以再登记
    void CaseTableFull() {
        Reset();
        ObjectHandle first = 0;
        for (uint32_t index = 0; index < Kernel::Objects::kMaxObjects; ++index) {
            const ObjectHandle handle = MustRegister(1, index);
            if (index == 0) first = handle;
        }
        Require(Kernel::Objects::Count() == Kernel::Objects::kMaxObjects, "对象数必须等于表容量");
        ObjectHandle handle = 0;
        Require(Kernel::Objects::Register(1, 0xFFFF, ResourceRange{}, &handle) == ObjectResult::TableFull, "表满必须返回 TableFull");
        Require(handle == 0, "表满时不得写句柄");
        Require(Kernel::Objects::Release(first) == ObjectResult::Ok, "释放第一条登记必须成功");
        MustRegister(2, 0xFFFF);
        Require(Kernel::Objects::Count() == Kernel::Objects::kMaxObjects, "复用空槽后对象数必须回到表容量");
    }

    // Core.Object v1 四个槽：经生成头的槽表调用，错误码按固定映射返回
    void CaseSlotTable() {
        Reset();
        const CoreObjectSlotsV1& slots = Kernel::Objects::kCoreObjectSlots;
        // 槽表是 inline constexpr，四个函数指针在编译期已确定；用例从调用行为上复核实现存在
        ObjectHandle handle = 0;
        Require(slots.Register(1, 0x1000, &handle) == KernelError::kOk, "槽 Register 必须成功");
        Require(handle != 0, "槽 Register 的句柄必须非零");
        Require(slots.Generations(handle) == Kernel::Objects::Generations(handle), "槽 Generations 必须与内部查询一致");
        ModuleId owner = {};
        Require(slots.Owner(handle, &owner) == KernelError::kOk, "槽 Owner 必须成功");
        Require(SameId(owner, kModuleKernelCore), "内核核心登记的槽 Owner 必须是全零标识");
        // 错误映射：参数非法 → kInvalidArgument；重复 → kAlreadyExists
        Require(slots.Register(1, 0x1000, &handle) == KernelError::kAlreadyExists, "重复登记必须映射为 kAlreadyExists");
        Require(slots.Register(Kernel::Objects::kKindInvalid, 1, &handle) == KernelError::kInvalidArgument, "kind 0 必须映射为 kInvalidArgument");
        Require(slots.Register(2, 2, nullptr) == KernelError::kInvalidArgument, "空句柄指针必须映射为 kInvalidArgument");
        Require(slots.Owner(handle, nullptr) == KernelError::kInvalidArgument, "空归属指针必须映射为 kInvalidArgument");
        Require(slots.Release(0) == KernelError::kInvalidArgument, "无效句柄拒绝必须映射为 kInvalidArgument");
        Require(slots.Release(handle) == KernelError::kOk, "槽 Release 必须成功");
        Require(slots.Generations(handle) == 0, "释放后槽 Generations 必须为 0");
        // 槽登记跟随当前所有者
        const ModuleId ownerA = kModAtaPio;
        {
            Kernel::Objects::OwnerScope scope(ownerA);
            ObjectHandle scoped = 0;
            Require(slots.Register(3, 0x2000, &scoped) == KernelError::kOk, "作用域内槽 Register 必须成功");
            Require(slots.Owner(scoped, &owner) == KernelError::kOk, "作用域内槽 Owner 必须成功");
            Require(SameId(owner, ownerA), "作用域内登记的归属必须是 A：" + IdText(owner));
        }
        // 表满映射：槽位没有区间参数，失败路径只用表满覆盖
        for (uint32_t index = Kernel::Objects::Count(); index < Kernel::Objects::kMaxObjects; ++index) MustRegister(4, index);
        Require(slots.Register(5, 0x3000, &handle) == KernelError::kOutOfResources, "表满必须映射为 kOutOfResources");
    }

    // 复位：清空全部登记、当前所有者回到内核核心，复位前的句柄全部失效
    void CaseResetClearsTable() {
        Reset();
        ObjectHandle handle = 0;
        {
            Kernel::Objects::OwnerScope scope(kModAtaPio);
            handle = MustRegister(1, 0x1000);
            Kernel::Objects::Reset();
            Require(Kernel::Objects::Count() == 0, "复位后对象表必须为空");
            Require(SameId(Kernel::Objects::CurrentOwner(), kModuleKernelCore), "复位后当前所有者必须回到内核核心");
        }
        Require(SameId(Kernel::Objects::CurrentOwner(), kModuleKernelCore), "作用域退出后仍是内核核心");
        Require(Kernel::Objects::Generations(handle) == 0, "复位前登记的句柄必须失效");
        Require(Kernel::Objects::Release(handle) == ObjectResult::InvalidHandle, "复位前登记的句柄不得释放");
        Require(Kernel::Objects::Count() == 0, "旧句柄操作后对象表仍为空");
    }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckObjects: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
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
        Case("登记与归属查询", CaseRegisterBasic);
        Case("代际递增与槽位复用", CaseGenerationIncrements);
        Case("无效与陈旧句柄拒绝", CaseInvalidHandles);
        Case("重复登记拒绝", CaseDuplicateRegistration);
        Case("资源区间冲突拒绝", CaseRangeConflict);
        Case("按所有者回收", CaseOwnerReclaim);
        Case("所有者作用域切换", CaseOwnerScopeNested);
        Case("对象表容量边界", CaseTableFull);
        Case("Core.Object 槽实现表", CaseSlotTable);
        Case("复位清空对象表", CaseResetClearsTable);
        Require(g_ran == kCaseCount, "内部回归用例数量不符");
        std::cout << "CheckObjects: " << g_ran << " 项用例全部通过" << std::endl;
    }
}

// 入口：跑全部用例
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string arg = argv[1];
            if (arg == "-h" || arg == "--help") {
                std::cout <<
                    "CheckObjects — 内核对象与归属登记最小面（T4.0）的宿主侧检查\n"
                    "\n"
                    "用法：CheckObjects\n"
                    "\n"
                    "检查对象是 Packages/LikesProgramOS/Core/Src/Objects.cpp：固定对象表、句柄与代际、\n"
                    "资源区间冲突、按所有者回收，以及 Core.Object v1 四个槽的实现表与错误码映射\n";
                return 0;
            }
            Fail("无法识别的参数：" + arg);
        }
        Execute();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CheckObjects: " << error.what() << '\n';
        return 1;
    }
}
