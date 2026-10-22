/* Objects.cpp
    对象与归属登记的实现：固定对象表、句柄代际、资源区间冲突检查与按所有者回收

    对象表是静态数组，不动态分配；句柄不单独存表——低 8 位是槽号、高 24 位是代际，
    校验句柄就是取槽号后比对代际，陈旧句柄自然失效
    当前所有者是模块级静态状态：装载器用 OwnerScope 成对切换，模块的登记与装载器自己的
    登记共用同一张表；资源区间只在记录层面做冲突检查，本层不碰内存
    代际只有 24 位编码：同一槽登记次数超过 0xFFFFFF 次后从 1 重新开始，理论上旧句柄可能
    重新有效；当前生命周期内同一槽远到不了这个量级，完整 Objects 换 64 位句柄时再收敛
*/

#include <Objects.hpp>

namespace Kernel {
    namespace Objects {
        namespace {
            // 对象表项：登记成功时写入，释放时清空；generation 只增不减
            struct Entry {
                uint64_t generation = 0;   // 最近一次登记的代际；0 表示从未使用过
                uint64_t value = 0;        // 对象值，由登记方解释
                ResourceRange range = {};  // 资源区间；bytes 为 0 表示没有
                ModuleId owner = {};       // 归属模块标识，全零为内核核心
                uint32_t kind = 0;         // 对象类型，0 表示槽位空闲
            };

            // 对象表与当前所有者：静态存储，启动早期为零初始化的干净状态
            Entry g_entries[kMaxObjects];
            ModuleId g_currentOwner = {};

            // 解码句柄：句柄合法、槽位在用且代际一致时返回槽号，否则返回 kMaxObjects
            uint32_t Resolve(ObjectHandle handle) {
                const uint32_t index = handle & kHandleIndexMask;
                const uint32_t generation = handle >> kHandleIndexBits;
                if (generation == 0 || index >= kMaxObjects) return kMaxObjects;
                if (g_entries[index].kind == kKindInvalid) return kMaxObjects;
                if (g_entries[index].generation != generation) return kMaxObjects;
                return index;
            }

            // 清空一个槽位的登记内容：代际保留，旧句柄因此继续按代际不匹配失效
            void ClearEntry(Entry& entry) {
                entry.kind = kKindInvalid;
                entry.value = 0;
                entry.range = {};
                entry.owner = {};
            }

            // 资源区间是否与既有登记重叠；任一方 bytes 为 0 视为没有区间，不参与检查
            bool Overlaps(uint64_t begin, uint64_t end, const ResourceRange& other) {
                if (other.bytes == 0) return false;
                return begin < other.begin + other.bytes && other.begin < end;
            }

            // 内部结果到契约错误码的固定映射：KernelError 的取值由生成头定义
            KernelError ToKernelError(ObjectResult result) {
                switch (result) {
                    case ObjectResult::Ok: return KernelError::kOk;
                    case ObjectResult::InvalidArgument: return KernelError::kInvalidArgument;
                    case ObjectResult::InvalidHandle: return KernelError::kInvalidArgument;
                    case ObjectResult::TableFull: return KernelError::kOutOfResources;
                    case ObjectResult::DuplicateObject: return KernelError::kAlreadyExists;
                    case ObjectResult::RangeConflict: return KernelError::kAlreadyExists;
                }
                return KernelError::kInvalidArgument;   // 枚举扩展后未同步本表时的兜底，漏项正常由 -Wswitch 拦下
            }
        }

        ObjectResult Register(uint32_t kind, uint64_t value, const ResourceRange& range, ObjectHandle* handleOut) {
            if (handleOut == nullptr || kind == kKindInvalid) return ObjectResult::InvalidArgument;
            // 资源区间加法先查溢出：begin + bytes 绕回时区间没有意义
            const bool hasRange = range.bytes != 0;
            if (hasRange && range.begin + range.bytes < range.begin) return ObjectResult::InvalidArgument;
            // 重复登记：同一 kind 与 value 只允许一条，避免同一资源被记两次
            for (uint32_t index = 0; index < kMaxObjects; ++index) {
                const Entry& entry = g_entries[index];
                if (entry.kind == kKindInvalid || entry.kind != kind) continue;
                if (entry.value == value) return ObjectResult::DuplicateObject;
            }
            // 资源区间冲突：与任一既有区间相交即拒绝，资源区间不跨对象复用
            if (hasRange) {
                const uint64_t end = range.begin + range.bytes;
                for (uint32_t index = 0; index < kMaxObjects; ++index) {
                    const Entry& entry = g_entries[index];
                    if (entry.kind == kKindInvalid) continue;
                    if (Overlaps(range.begin, end, entry.range)) return ObjectResult::RangeConflict;
                }
            }
            // 取第一个空闲槽位；表满时最后一步返回 TableFull，不做淘汰
            uint32_t index = 0;
            for (; index < kMaxObjects; ++index) if (g_entries[index].kind == kKindInvalid) break;
            if (index == kMaxObjects) return ObjectResult::TableFull;
            Entry& entry = g_entries[index];
            entry.generation += 1;
            if (entry.generation > kHandleGenerationMax) entry.generation = 1;   // 24 位代际绕回
            entry.kind = kind;
            entry.value = value;
            entry.range = range;
            entry.owner = g_currentOwner;
            *handleOut = static_cast<ObjectHandle>((entry.generation << kHandleIndexBits) | index);
            return ObjectResult::Ok;
        }

        ObjectResult Release(ObjectHandle handle) {
            const uint32_t index = Resolve(handle);
            if (index == kMaxObjects) return ObjectResult::InvalidHandle;
            ClearEntry(g_entries[index]);
            return ObjectResult::Ok;
        }

        uint64_t Generations(ObjectHandle handle) {
            const uint32_t index = Resolve(handle);
            if (index == kMaxObjects) return 0;
            return g_entries[index].generation;
        }

        ObjectResult Owner(ObjectHandle handle, ModuleId* ownerOut) {
            if (ownerOut == nullptr) return ObjectResult::InvalidArgument;
            const uint32_t index = Resolve(handle);
            if (index == kMaxObjects) return ObjectResult::InvalidHandle;
            *ownerOut = g_entries[index].owner;
            return ObjectResult::Ok;
        }

        uint32_t ReleaseByOwner(const ModuleId& owner) {
            uint32_t released = 0;
            for (uint32_t index = 0; index < kMaxObjects; ++index) {
                Entry& entry = g_entries[index];
                if (entry.kind == kKindInvalid) continue;
                if (!(entry.owner == owner)) continue;
                ClearEntry(entry);
                ++released;
            }
            return released;
        }

        void SetCurrentOwner(const ModuleId& owner) { g_currentOwner = owner; }

        ModuleId CurrentOwner() { return g_currentOwner; }

        uint32_t Count() {
            uint32_t count = 0;
            for (uint32_t index = 0; index < kMaxObjects; ++index) {
                if (g_entries[index].kind != kKindInvalid) ++count;
            }
            return count;
        }

        void Reset() {
            for (uint32_t index = 0; index < kMaxObjects; ++index) g_entries[index] = Entry{};
            g_currentOwner = {};
        }

        OwnerScope::OwnerScope(const ModuleId& owner) : m_previous(g_currentOwner) { g_currentOwner = owner; }

        OwnerScope::~OwnerScope() { g_currentOwner = m_previous; }

        KernelError CoreObjectRegister(uint32_t kind, uint64_t value, uint32_t* handleOut) {
            // 槽位签名没有区间参数，按无区间登记；资源区间只走内核内部的 Register
            return ToKernelError(Register(kind, value, ResourceRange{}, handleOut));
        }

        KernelError CoreObjectRelease(uint32_t handle) { return ToKernelError(Release(handle)); }

        uint64_t CoreObjectGenerations(uint32_t handle) { return Generations(handle); }

        KernelError CoreObjectOwner(uint32_t handle, ModuleId* ownerOut) { return ToKernelError(Owner(handle, ownerOut)); }
    }
}
