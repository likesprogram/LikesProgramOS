/* Paging.cpp
    内核引导页表的建立与切换：恒等映射与直映射窗口

    页表存储是未初始化区的固定数组：引导阶段不分配内存，大小由覆盖范围与页粒度在编译期定死
    页表结点的物理地址在恒等映射下就是数组地址本身；切换 CR3 后这些页仍按恒等映射可达，
    因此不需要在其中维护虚拟地址与物理地址两套写法
*/

#include <Paging.hpp>

namespace Kernel {
    namespace {
        // 每张页表 512 个结点
        constexpr uint32_t kEntriesPerTable = 512;
        // 结点标志：存在且可写；页目录结点另带大页位 PS
        constexpr uint64_t kPresentRw = 0x03;
        constexpr uint64_t kPresentRwLarge = 0x83;
        // 覆盖 kMapLimitBytes 需要的页目录数量：每张覆盖 512 × 2MiB = 1GiB
        constexpr uint32_t kPdTables = static_cast<uint32_t>(kMapLimitBytes / (static_cast<uint64_t>(kEntriesPerTable) * kLargePageBytes));
        // PML4 槽位：恒等映射占 0 号，直映射窗口取基址对应的槽位
        constexpr uint32_t kPml4IdentityIndex = 0;
        constexpr uint32_t kPml4PhysMapIndex = static_cast<uint32_t>((kPhysMapBase >> 39) & 0x1FF);

        // 页表存储：都在未初始化区，由引导器按数据的 MemoryBytes 清零
        alignas(4096) uint64_t s_pml4[kEntriesPerTable];
        alignas(4096) uint64_t s_pdptIdentity[kEntriesPerTable];
        alignas(4096) uint64_t s_pdptPhysMap[kEntriesPerTable];
        alignas(4096) uint64_t s_pdIdentity[kPdTables][kEntriesPerTable];
        alignas(4096) uint64_t s_pdPhysMap[kPdTables][kEntriesPerTable];

        // 把一张页目录填成 2MiB 大页映射：base 是这块虚拟范围对应的物理起始地址
        void FillLargePages(uint64_t* table, uint64_t base) {
            for (uint32_t entry = 0; entry < kEntriesPerTable; ++entry) {
                table[entry] = (base + static_cast<uint64_t>(entry) * kLargePageBytes) | kPresentRwLarge;
            }
        }
    }

    const char* InstallBootPaging() {
        static_assert(kPdTables > 0, "引导页表必须至少覆盖一张页目录的范围");
        // 上位表：恒等映射与直映射窗口各接一棵子树
        s_pml4[kPml4IdentityIndex] = reinterpret_cast<uintptr_t>(s_pdptIdentity) | kPresentRw;
        s_pml4[kPml4PhysMapIndex] = reinterpret_cast<uintptr_t>(s_pdptPhysMap) | kPresentRw;
        for (uint32_t index = 0; index < kPdTables; ++index) {
            const uint64_t base = static_cast<uint64_t>(index) * kEntriesPerTable * kLargePageBytes;
            s_pdptIdentity[index] = reinterpret_cast<uintptr_t>(s_pdIdentity[index]) | kPresentRw;
            s_pdptPhysMap[index] = reinterpret_cast<uintptr_t>(s_pdPhysMap[index]) | kPresentRw;
            FillLargePages(s_pdIdentity[index], base);
            FillLargePages(s_pdPhysMap[index], base);
        }
        // 装载 CR3：TLB 全部失效，紧随其后的取指仍落在恒等映射的同一物理页上
        const uintptr_t table = reinterpret_cast<uintptr_t>(s_pml4);
        asm volatile("MOV %0, %%CR3" : : "r"(table) : "memory");
        return nullptr;
    }
}
