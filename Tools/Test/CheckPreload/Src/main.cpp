/* main.cpp
    CheckPreload：引导闭包预装共享机制的宿主侧检查，手工执行，不构成默认构建依赖

    检查对象是 BaleenCore 与 UEFI 侧链接的同一份 Packages/Baleen/Core/Src/BootClosure.cpp：
    列表解析与期望集核对（PlanPreload）、内存图落点搜索（FindModuleSpan）、
    逐模块最小读取面 / 主版本门 / SHA-256 与记录填写（PreloadModules）；
    另核对 BIOS 侧屏幕事实的纯映射（Packages/Baleen/Core/Src/ScreenFacts.cpp 的 DecodeVgaScreenFacts）
    源码按相对路径直接编进本工具，不复制实现；宿主是 64 位，目标侧 32 位构建的差异由
    BootClosure.cpp 自身处理（不引 64 位除法的运行库辅助函数）

    平台侧由替身注入：文件来自内存缓冲，落点来自假的对齐分配器；
    判定所需的清单条目视图按 ManifestFacts 的形态构造，不解析清单字节
*/

#include <BootClosure.hpp>
#include <ScreenFacts.hpp>
#include <Sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    namespace Core = Baleen::Core;
    using Baleen::Diagnostic;

    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 30;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) {
        if (value) return;
        Fail(message);
    }
    // 成功判定：code 必须为空
    void RequireOk(const Diagnostic& diagnostic, const std::string& what) {
        if (diagnostic.code == nullptr) return;
        Fail(what + "：预期成功，实际 " + diagnostic.code + (diagnostic.text != nullptr ? " " + std::string(diagnostic.text) : ""));
    }
    // 编号判定
    void RequireCode(const Diagnostic& diagnostic, const char* code, const std::string& what) {
        if (diagnostic.code == nullptr) Fail(what + "：预期 " + code + "，实际成功");
        if (std::strcmp(diagnostic.code, code) != 0) Fail(what + "：预期 " + code + "，实际 " + diagnostic.code);
    }
    // 文本判定：期望集缺 / 重靠它区分
    void RequireText(const Diagnostic& diagnostic, const char* text, const std::string& what) {
        if (diagnostic.text == nullptr) Fail(what + "：诊断文本为空");
        if (std::strcmp(diagnostic.text, text) != 0) Fail(what + "：预期文本 " + text + "，实际 " + diagnostic.text);
    }
    // 跑一个用例：异常只终止当前用例并记失败，其余用例照跑
    void Case(const char* name, void (*function)()) {
        ++g_ran;
        try {
            function();
        } catch (const std::exception& error) {
            ++g_failed;
            std::cerr << "CheckPreload: 失败用例「" << name << "」：" << error.what() << std::endl;
        }
    }

    // 引导闭包 13 个成员的列表路径：顺序与 Contract/BootClosure.def 一致
    const char* const kClosurePaths[13] = {
        "Modules/DevModel.1.0.0.osm",
        "Modules/DriverLoader.1.0.0.osm",
        "Modules/AtaPio.1.0.0.osm",
        "Modules/Ahci.1.0.0.osm",
        "Modules/Nvme.1.0.0.osm",
        "Modules/UsbCore.1.0.0.osm",
        "Modules/Xhci.1.0.0.osm",
        "Modules/UsbHub.1.0.0.osm",
        "Modules/UsbStorage.1.0.0.osm",
        "Modules/Vfs.1.0.0.osm",
        "Modules/Ext4.1.0.0.osm",
        "Modules/ISO9660.1.0.0.osm",
        "Modules/FAT32File.1.0.0.osm",
    };
    constexpr uint32_t kClosureCount = 13;

    // 列表副本（容量按 16 KiB 的真实上限留）
    char g_list[Core::kBootListMaxBytes];
    // 列表字节数
    uint32_t g_listBytes = 0;
    // 清单副本占位：预装机制不解析清单内容，只把地址与长度交给交接包
    uint8_t g_manifest[0x100] = {};
    // 清单 Type=2 条目视图的存储：路径与摘要各留一份
    uint8_t g_artifactPaths[40][256] = {};
    uint8_t g_artifactDigests[40][32] = {};
    // 清单 Type=2 条目视图
    Core::PreloadArtifact g_artifacts[40] = {};
    // 清单 Type=2 条目数
    uint32_t g_artifactCount = 0;

    // 拼一份 Modules.list：固定三行加 count 条 module 行
    void BuildList(const char* const* paths, uint32_t count, uint32_t major, uint64_t revision) {
        const int header = std::snprintf(g_list, sizeof(g_list), "LPML 1\nmajor %u\nrevision %llu\n", major, static_cast<unsigned long long>(revision));
        g_listBytes = static_cast<uint32_t>(header);
        for (uint32_t i = 0; i < count; ++i) {
            const int written = std::snprintf(g_list + g_listBytes, sizeof(g_list) - g_listBytes, "module %s\n", paths[i]);
            g_listBytes += static_cast<uint32_t>(written);
        }
    }

    // 恢复成合法 13 条列表
    void BuildValidList() { BuildList(kClosurePaths, kClosureCount, 1, 7); }

    // 按路径表构造清单 Type=2 条目视图：摘要先填占位字节，正例由 ComputeDigest 覆盖
    void BuildArtifacts(const char* const* paths, uint32_t count, uint64_t dataBytes) {
        g_artifactCount = count;
        for (uint32_t i = 0; i < count && i < 40; ++i) {
            const uint32_t pathBytes = static_cast<uint32_t>(std::strlen(paths[i]));
            std::memcpy(g_artifactPaths[i], paths[i], pathBytes);
            std::memset(g_artifactDigests[i], static_cast<int>(0x10 + i), 32);
            g_artifacts[i].artifactIndex = i;
            g_artifacts[i].pathBytes = static_cast<uint16_t>(pathBytes);
            g_artifacts[i].dataBytes = dataBytes;
            g_artifacts[i].path = g_artifactPaths[i];
            g_artifacts[i].digest = g_artifactDigests[i];
        }
    }

    // 预装来源：清单占位、当前列表与清单条目视图
    Core::PreloadSource MakeSource(uint16_t moduleMajor) {
        Core::PreloadSource source{};
        source.setDirectory = "/Set";
        source.manifest = g_manifest;
        source.manifestBytes = sizeof(g_manifest);
        source.list = reinterpret_cast<const uint8_t*>(g_list);
        source.listBytes = g_listBytes;
        source.moduleMajor = moduleMajor;
        source.artifacts = g_artifacts;
        source.artifactCount = g_artifactCount;
        return source;
    }

    // 模块文件夹具：头 0x80 字节，最小读取面字段按取值填好
    uint8_t g_modules[13][0x20000] = {};
    // 模块主版本字段偏移：0x66
    constexpr uint32_t kModuleMajorField = 0x66;

    // 按小端写 16 位
    void Put16(uint8_t* field, uint16_t value) {
        field[0] = static_cast<uint8_t>(value & 0xFF);
        field[1] = static_cast<uint8_t>(value >> 8);
    }

    // 按小端写 32 位
    void Put32(uint8_t* field, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) field[i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // 构造一个模块文件：内容按序号填充，最小读取面与主版本字段写对
    void BuildModule(uint32_t index, uint32_t bytes, uint16_t major) {
        const uint8_t magic[8] = { 0x4C, 0x50, 0x4F, 0x53, 0x4D, 0x4F, 0x44, 0x00 };
        uint8_t* const data = g_modules[index];
        std::memset(data, static_cast<int>(0xA0 + index), bytes);
        std::memcpy(data, magic, sizeof(magic));
        Put16(data + 0x08, 1);            // Version
        Put16(data + 0x0A, 0x80);         // HeaderBytes
        Put32(data + 0x0C, 0);            // Flags
        Put32(data + 0x10, bytes);        // TotalBytes
        Put16(data + kModuleMajorField, major);
    }

    // 复算一段字节的 SHA-256：与清单登记值比对用
    void ComputeDigest(const uint8_t* data, uint32_t bytes, uint8_t* digest) {
        Baleen::Digest::Sha256 sha;
        sha.Begin();
        sha.Update(data, bytes);
        sha.Finish(digest);
    }

    // 替身 sink：文件来自内存，落点来自固定基址的 4 KiB 对齐分配
    class FakeSink final : public Core::PreloadSink {
    public:
        // 已登记文件
        struct File {
            char path[512];       // 完整路径
            const uint8_t* data;  // 文件字节
            uint32_t bytes;       // 文件字节数
        };
        // 已分配落点
        struct Allocation {
            uint64_t phys;   // 假物理地址
            uint8_t* data;   // 实际可写指针
            uint32_t bytes;  // 长度
        };

        File files[32] = {};                              // 已登记文件
        uint32_t fileCount = 0;                           // 已登记文件数
        alignas(4096) uint8_t arena[1 << 21] = {};        // 8 MiB 落点池
        uint32_t arenaUsed = 0;                           // 落点池已用字节
        Allocation allocations[32] = {};                  // 已分配落点
        uint32_t allocationCount = 0;                     // 已分配落点数
        uint32_t releaseCount = 0;                        // 归还次数
        uint32_t failOpenIndex = 0xFFFFFFFF;              // 打开到该文件时失败
        uint32_t failReadIndex = 0xFFFFFFFF;              // 读到该文件时失败
        uint32_t failAllocateIndex = 0xFFFFFFFF;          // 第几次分配失败

        // 登记一个文件
        void AddFile(const char* path, const uint8_t* data) {
            std::snprintf(files[fileCount].path, sizeof(files[fileCount].path), "%s", path);
            files[fileCount].data = data;
            files[fileCount].bytes = Core::kModuleHeaderBytes;
            ++fileCount;
        }

        // 登记一个文件并给出字节数
        void AddFile(const char* path, const uint8_t* data, uint32_t bytes) {
            AddFile(path, data);
            files[fileCount - 1].bytes = bytes;
        }

        // 取一段已分配落点的可写指针：测试用，找不到返回空
        const uint8_t* DataOf(uint64_t phys) const {
            for (uint32_t i = 0; i < allocationCount; ++i) if (allocations[i].phys == phys) return allocations[i].data;
            return nullptr;
        }

        bool Open(const char* path, uint32_t& handle) override {
            for (uint32_t i = 0; i < fileCount; ++i) {
                if (std::strcmp(files[i].path, path) != 0) continue;
                if (i == failOpenIndex) return false;
                handle = i + 1;
                return true;
            }
            return false;
        }

        bool Size(uint32_t handle, uint64_t& bytes) override {
            if (handle == 0 || handle > fileCount) return false;
            bytes = files[handle - 1].bytes;
            return true;
        }

        bool Read(uint32_t handle, uint64_t offset, void* buffer, uint32_t bytes, uint32_t& read) override {
            if (handle == 0 || handle > fileCount) return false;
            if (handle - 1 == failReadIndex) return false;
            const File& file = files[handle - 1];
            if (offset >= file.bytes) { read = 0; return true; }
            uint64_t size = file.bytes - offset;
            if (size > bytes) size = bytes;
            std::memcpy(buffer, file.data + offset, static_cast<size_t>(size));
            read = static_cast<uint32_t>(size);
            return true;
        }

        void Close(uint32_t handle) override { (void)handle; }

        bool Allocate(uint32_t bytes, Core::PreloadSpan& span) override {
            if (allocationCount == failAllocateIndex) return false;
            arenaUsed = (arenaUsed + static_cast<uint32_t>(Core::kPreloadPageBytes) - 1) & ~(static_cast<uint32_t>(Core::kPreloadPageBytes) - 1);
            span.data = arena + arenaUsed;
            span.phys = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(span.data));
            span.bytes = bytes;
            arenaUsed += bytes;
            if (allocationCount < 32) {
                allocations[allocationCount].phys = span.phys;
                allocations[allocationCount].data = span.data;
                allocations[allocationCount].bytes = bytes;
                ++allocationCount;
            }
            return true;
        }

        void Release(const Core::PreloadSpan& span) override {
            (void)span;
            ++releaseCount;
        }
    };

    // 构造 PreloadModules 的完整正例：列表、清单条目与模块文件一致
    void PreparePreloadFixture(FakeSink& sink, uint32_t moduleBytes, uint16_t moduleMajor) {
        BuildValidList();
        BuildArtifacts(kClosurePaths, kClosureCount, moduleBytes);
        for (uint32_t i = 0; i < kClosureCount; ++i) {
            BuildModule(i, moduleBytes, moduleMajor);
            ComputeDigest(g_modules[i], moduleBytes, g_artifactDigests[i]);
            g_artifacts[i].dataBytes = moduleBytes;
            char fullPath[512] = {};
            std::snprintf(fullPath, sizeof(fullPath), "/Set/%s", kClosurePaths[i]);
            sink.AddFile(fullPath, g_modules[i], moduleBytes);
        }
    }

    // 模块落点搜索用的内存图条目
    using Entry = Boot::MemoryMapEntry;

    // 构造一条内存图条目：属性位按「应保留」填 1
    Entry Map(uint64_t base, uint64_t length, uint32_t type = 1) { return Entry{base, length, type, 1}; }

    // 用一张图找模块落点：loader / kernel 跨度为空，无已分配落点
    uint64_t Span(const std::vector<Entry>& map, uint64_t bytes) {
        Core::SpanConstraints constraints{};
        constraints.memoryMap = map.data();
        constraints.memoryMapCount = static_cast<uint32_t>(map.size());
        return Core::FindModuleSpan(constraints, bytes);
    }

    // 用一张图与给定跨度找模块落点
    uint64_t SpanWith(const std::vector<Entry>& map, uint64_t bytes, uint64_t loaderBegin, uint64_t loaderBytes, uint64_t kernelBegin, uint64_t kernelBytes) {
        Core::SpanConstraints constraints{};
        constraints.memoryMap = map.data();
        constraints.memoryMapCount = static_cast<uint32_t>(map.size());
        constraints.loaderBegin = loaderBegin;
        constraints.loaderBytes = loaderBytes;
        constraints.kernelBegin = kernelBegin;
        constraints.kernelBytes = kernelBytes;
        return Core::FindModuleSpan(constraints, bytes);
    }

    // 用例：退化输入与找不到窗口
    void CaseFindSpanDegenerate() {
        Require(Span({}, 0x1000) == 0, "空内存图必须返回 0");
        const std::vector<Entry> empty = { Map(0x100000, 0x100000) };
        Core::SpanConstraints constraints{};
        constraints.memoryMap = empty.data();
        constraints.memoryMapCount = 1;
        Require(Core::FindModuleSpan(constraints, 0) == 0, "零长度必须返回 0");
        Require(Core::FindModuleSpan(constraints, 0x100000001ull) == 0, "超过低 4 GiB 必须返回 0");
        const std::vector<Entry> reserved = { Map(0x100000, 0x100000, 2) };
        Require(Span(reserved, 0x1000) == 0, "只有保留条目时必须返回 0");
        const std::vector<Entry> small = { Map(0x100000, 0x800) };
        Require(Span(small, 0x1000) == 0, "可用区间装不下时必须返回 0");
    }

    // 用例：相邻可用条目合并，空洞不跨
    void CaseFindSpanAdjacent() {
        const std::vector<Entry> adjacent = { Map(0x100000, 0x1000), Map(0x101000, 0x2000) };
        Require(Span(adjacent, 0x3000) == 0x100000, "相邻两条必须合并成一段");
        const std::vector<Entry> gap = { Map(0x100000, 0x1000), Map(0x102000, 0x2000) };
        Require(Span(gap, 0x3000) == 0, "中间有空洞时不得跨段");
        const std::vector<Entry> split = { Map(0x100000, 0x1000, 2), Map(0x101000, 0x2000) };
        Require(Span(split, 0x1000) == 0x101000, "保留条目之后才是可用段");
    }

    // 用例：避开引导器与内核装载跨度
    void CaseFindSpanAvoidSpans() {
        const std::vector<Entry> map = { Map(0x100000, 0x100000) };
        Require(SpanWith(map, 0x1000, 0, 0, 0x100000, 0x80000) == 0x180000, "内核跨度必须被跳过");
        Require(SpanWith(map, 0x1000, 0x180000, 0x1000, 0x100000, 0x80000) == 0x181000, "内核与引导器跨度都要跳过");
        Require(SpanWith(map, 0x1000, 0x100000, 0x1000, 0, 0) == 0x101000, "引导器跨度必须被跳过");
        const std::vector<Entry> tight = { Map(0x100000, 0x1000) };
        Require(SpanWith(tight, 0x1000, 0, 0, 0x100000, 0x1000) == 0, "唯一的可用段被内核占用时返回 0");
    }

    // 用例：避开低 1 MiB
    void CaseFindSpanLowMemory() {
        const std::vector<Entry> map = { Map(0x0, 0x200000) };
        Require(Span(map, 0x100000) == 0x100000, "低 1 MiB 必须被避开，落点从 1 MiB 起");
        Require(Span(map, 0x80000) == 0x100000, "低地址可用段仍从 1 MiB 起");
        const std::vector<Entry> below = { Map(0x0, 0x100000) };
        Require(Span(below, 0x1000) == 0, "低 1 MiB 之内没有可用落点");
    }

    // 用例：末端跨 4 GiB 不上送
    void CaseFindSpanAddressLimit() {
        const std::vector<Entry> crossing = { Map(0xFFFFF000ull, 0x2000) };
        Require(Span(crossing, 0x1000) == 0xFFFFF000ull, "4 GiB 下最后一个页可用");
        Require(Span(crossing, 0x2000) == 0, "末端跨 4 GiB 的落点不上送");
        const std::vector<Entry> beyond = { Map(0x100000000ull, 0x100000) };
        Require(Span(beyond, 0x1000) == 0, "4 GiB 以上的条目不参与搜索");
        const std::vector<Entry> nearLimit = { Map(0xFFFF0000ull, 0x10000) };
        Require(Span(nearLimit, 0x10000) == 0xFFFF0000ull, "整段在 4 GiB 之下时可用");
    }

    // 用例：64 位长度回绕按 4 GiB 上界截断
    void CaseFindSpanWraparound() {
        const std::vector<Entry> wrapped = { Map(0x100000, 0xFFFFFFFFFFFFFFFFull) };
        Require(Span(wrapped, 0x1000) == 0x100000, "条目长度回绕按到 4 GiB 上界处理");
        const std::vector<Entry> high = { Map(0xFFFFFFFFFFFFF000ull, 0x1000) };
        Require(Span(high, 0x1000) == 0, "基址在 4 GiB 以上的回绕条目不参与搜索");
        const std::vector<Entry> head = { Map(0xFFFFFFFF00000000ull, 0x200000) };
        Require(Span(head, 0x1000) == 0, "整条落在 4 GiB 以上时返回 0");
    }

    // 用例：结果按 4 KiB 对齐
    void CaseFindSpanAlignment() {
        const std::vector<Entry> unaligned = { Map(0x100800, 0x10000) };
        Require(Span(unaligned, 0x1000) == 0x101000, "落点必须向上对齐到 4 KiB");
        const std::vector<Entry> page = { Map(0x101000, 0x1000) };
        Require(Span(page, 0x1000) == 0x101000, "已对齐的起点保持不变");
        Require((Span(page, 0x1000) & 0xFFF) == 0, "落点必须 4 KiB 对齐");
    }

    // 用例：已分配落点互斥与分配器顺序
    void CaseFindSpanAllocator() {
        const std::vector<Entry> map = { Map(0x100000, 0x10000) };
        Core::MemoryMapAllocator allocator(map.data(), 1, 0, 0, 0, 0);
        Core::PreloadSpan first{};
        Core::PreloadSpan second{};
        Require(allocator.Allocate(0x1000, first), "第一次分配必须成功");
        Require(allocator.Allocate(0x1000, second), "第二次分配必须成功");
        Require(first.phys == 0x100000, "首次分配取最低可用落点");
        Require(second.phys == 0x101000, "第二次分配必须避开已分配落点");
        Require(first.phys + first.bytes <= second.phys || second.phys + second.bytes <= first.phys, "两次分配不得重叠");
        allocator.Release(second);
        Core::PreloadSpan third{};
        Require(allocator.Allocate(0x1000, third), "归还后必须能继续分配");
        Require(third.phys == 0x101000, "归还后的落点必须可复用");
        // 已分配落点直接来自约束时同样被避开
        Core::PreloadSpan taken[2] = {};
        taken[0].phys = 0x100000;
        taken[0].bytes = 0x1000;
        Core::SpanConstraints constraints{};
        constraints.memoryMap = map.data();
        constraints.memoryMapCount = 1;
        constraints.taken = taken;
        constraints.takenCount = 1;
        Require(Core::FindModuleSpan(constraints, 0x1000) == 0x101000, "显式传入的已分配落点必须被避开");
    }

    // 用例：合法 13 条列表产出计划
    void CasePlanValid() {
        BuildValidList();
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadPlan plan{};
        const Diagnostic reason = Core::PlanPreload(MakeSource(1), plan);
        RequireOk(reason, "合法列表");
        Require(plan.count == kClosureCount, "计划条数必须等于闭包成员数");
        Require(plan.moduleMajor == 1, "计划主版本取自来源");
        Require(plan.revision == 7, "计划修订号取自列表");
        Require(plan.list == reinterpret_cast<const uint8_t*>(g_list), "计划列表副本必须是列表字节");
        Require(plan.listBytes == g_listBytes, "计划列表长度必须是列表字节数");
        Require(plan.manifest == g_manifest, "计划清单副本必须是来源清单");
        Require(plan.setDirectory != nullptr && std::strcmp(plan.setDirectory, "/Set") == 0, "计划必须带回集目录");
        Require(plan.moduleBytes == kClosureCount * 0x100, "计划总字节必须是清单 DataBytes 之和");
        for (uint32_t i = 0; i < kClosureCount; ++i) {
            Require(plan.entries[i].artifactIndex == i, "计划条目下标必须对应清单产物下标");
            Require(plan.entries[i].dataBytes == 0x100, "计划条目长度必须来自清单");
            Require(plan.entries[i].digest != nullptr, "计划条目摘要必须来自清单");
        }
    }

    // 用例：容量超 16 KiB 被拒
    void CasePlanTooLarge() {
        BuildValidList();
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadSource source = MakeSource(1);
        source.listBytes = Core::kBootListMaxBytes + 1;
        Core::PreloadPlan plan{};
        const Diagnostic reason = Core::PlanPreload(source, plan);
        RequireCode(reason, "E-CORE-96", "列表容量超限");
        RequireText(reason, "The module load list is invalid", "容量超限按结构错误拒绝");
    }

    // 用例：条目超 30 条被拒（结构检查先于语义核对）
    void CasePlanTooManyEntries() {
        std::vector<std::string> filler(31);
        std::vector<const char*> fillerPaths(31);
        for (uint32_t i = 0; i < 31; ++i) {
            filler[i] = "Modules/Filler" + std::to_string(i) + ".1.0.0.osm";
            fillerPaths[i] = filler[i].c_str();
        }
        BuildList(fillerPaths.data(), 31, 1, 7);
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadPlan plan{};
        const Diagnostic reason = Core::PlanPreload(MakeSource(1), plan);
        RequireCode(reason, "E-CORE-96", "列表条目超 30 条");
        RequireText(reason, "The module load list is invalid", "30 条上限按结构错误拒绝，而不是未知成员");
    }

    // 用例：重复路径被拒
    void CasePlanDuplicatePath() {
        const char* paths[13] = {};
        for (uint32_t i = 0; i < kClosureCount; ++i) paths[i] = kClosurePaths[i];
        paths[1] = kClosurePaths[0];
        BuildList(paths, kClosureCount, 1, 7);
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadPlan plan{};
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "重复路径");
    }

    // 用例：路径不在 Modules/ 下被拒
    void CasePlanOutsideModules() {
        const char* paths[13] = {};
        for (uint32_t i = 0; i < kClosureCount; ++i) paths[i] = kClosurePaths[i];
        paths[0] = "Other/DevModel.1.0.0.osm";
        BuildList(paths, kClosureCount, 1, 7);
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadPlan plan{};
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "路径不在 Modules/ 下");
    }

    // 用例：路径含 .. 被拒
    void CasePlanDotDot() {
        const char* paths[13] = {};
        for (uint32_t i = 0; i < kClosureCount; ++i) paths[i] = kClosurePaths[i];
        paths[0] = "Modules/../DevModel.1.0.0.osm";
        BuildList(paths, kClosureCount, 1, 7);
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadPlan plan{};
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "路径含 ..");
    }

    // 用例：条目不在清单被拒
    void CasePlanNotInManifest() {
        BuildValidList();
        BuildArtifacts(kClosurePaths, kClosureCount - 1, 0x100);
        Core::PreloadPlan plan{};
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "列表条目不在清单里");
    }

    // 用例：固定头与结尾换行格式
    void CasePlanMalformedHeader() {
        BuildValidList();
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadPlan plan{};
        char saved[Core::kBootListMaxBytes] = {};
        std::memcpy(saved, g_list, g_listBytes);
        const uint32_t savedBytes = g_listBytes;
        const uint32_t headerBytes = static_cast<uint32_t>(std::strlen("LPML 1\nmajor 1\nrevision 7\n"));
        // 魔数不符
        std::memcpy(g_list, "LPML 2\n", 7);
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "格式标记不符");
        std::memcpy(g_list, saved, savedBytes);
        // 主版本为零
        g_list[7 + 6] = '0';
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "主版本为零");
        std::memcpy(g_list, saved, savedBytes);
        // 修订号为零
        g_list[15 + 9] = '0';
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "修订号为零");
        std::memcpy(g_list, saved, savedBytes);
        // 缺结尾换行
        g_listBytes = savedBytes - 1;
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "缺结尾换行");
        g_listBytes = savedBytes;
        // 行内出现 CR
        g_list[headerBytes - 1] = '\r';
        RequireCode(Core::PlanPreload(MakeSource(1), plan), "E-CORE-96", "行内含 CR");
        std::memcpy(g_list, saved, savedBytes);
        RequireOk(Core::PlanPreload(MakeSource(1), plan), "恢复后的合法列表");
    }

    // 用例：期望集漏 / 重两种现场；非闭包条目不能顶替闭包成员
    void CaseExpectedSetMismatch() {
        Core::PreloadPlan plan{};
        // 缺：少一条
        BuildList(kClosurePaths, kClosureCount - 1, 1, 7);
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Diagnostic reason = Core::PlanPreload(MakeSource(1), plan);
        RequireCode(reason, "E-CORE-96", "期望集缺项");
        RequireText(reason, "The module load list is missing a boot closure module", "期望集缺项现场");
        // 重：同一成员的另一个版本出现两次
        const char* duplicated[13] = {};
        for (uint32_t i = 0; i < kClosureCount; ++i) duplicated[i] = kClosurePaths[i];
        duplicated[12] = "Modules/Ext4.1.0.1.osm";
        BuildList(duplicated, kClosureCount, 1, 7);
        BuildArtifacts(duplicated, kClosureCount, 0x100);
        reason = Core::PlanPreload(MakeSource(1), plan);
        RequireCode(reason, "E-CORE-96", "期望集重复");
        RequireText(reason, "The module load list repeats a boot closure module", "期望集重复现场");
        // 非闭包条目不算成员：列表里换成一条运行期模块，DevModel 仍然缺项
        const char* ghost[13] = {};
        for (uint32_t i = 0; i < kClosureCount; ++i) ghost[i] = kClosurePaths[i];
        ghost[0] = "Modules/Ghost.1.0.0.osm";
        BuildList(ghost, kClosureCount, 1, 7);
        BuildArtifacts(ghost, kClosureCount, 0x100);
        reason = Core::PlanPreload(MakeSource(1), plan);
        RequireCode(reason, "E-CORE-96", "非闭包条目不能顶替闭包成员");
        RequireText(reason, "The module load list is missing a boot closure module", "非闭包条目按忽略处理，缺项照报");
    }

    // 用例：列表含运行期模块时只预装闭包成员（运行期条目由内核挂卷后装载）
    void CasePlanSkipsRuntimeEntries() {
        // 13 条闭包 + 2 条运行期（必需平铺 + 可选嵌套），清单 Type=2 条目与列表一一对应
        std::vector<std::string> runtime = {
            "Modules/Fault.1.0.0.osm",
            "Modules/Optional/Optional.1.0.0.osm",
        };
        std::vector<const char*> listed;
        listed.push_back(runtime[1].c_str());   // 运行期条目可以出现在列表任意位置
        for (uint32_t i = 0; i < kClosureCount; ++i) listed.push_back(kClosurePaths[i]);
        listed.push_back(runtime[0].c_str());
        BuildList(listed.data(), static_cast<uint32_t>(listed.size()), 1, 7);
        BuildArtifacts(listed.data(), static_cast<uint32_t>(listed.size()), 0x100);
        Core::PreloadPlan plan{};
        RequireOk(Core::PlanPreload(MakeSource(1), plan), "含运行期条目的列表");
        Require(plan.count == kClosureCount, "预装计划只含闭包成员");
        Require(plan.moduleBytes == static_cast<uint64_t>(kClosureCount) * 0x100, "总字节只算闭包成员");
        for (uint32_t i = 0; i < kClosureCount; ++i) {
            Require(plan.entries[i].artifactIndex == i + 1, "闭包成员的清单下标必须与列表位置一致");
            Require(std::memcmp(plan.entries[i].path, kClosurePaths[i], std::strlen(kClosurePaths[i])) == 0,
                "计划条目必须按闭包成员的列表顺序排列");
        }
    }

    // 用例：列表主版本与 .os 不符（E-95）
    void CaseListMajorGate() {
        BuildValidList();
        BuildArtifacts(kClosurePaths, kClosureCount, 0x100);
        Core::PreloadPlan plan{};
        RequireCode(Core::PlanPreload(MakeSource(2), plan), "E-CORE-95", "列表主版本与 .os 不符");
        // 来源没有 .os 主版本时按列表值，验签已保证两者一致
        RequireOk(Core::PlanPreload(MakeSource(0), plan), "来源未给主版本时按列表值");
        Require(plan.moduleMajor == 1, "按列表值取主版本");
    }

    // 用例：模块头主版本与 .os 不符（E-95）
    void CaseModuleMajorGate() {
        FakeSink sink;
        PreparePreloadFixture(sink, 0x1000, 1);
        Put16(g_modules[5] + kModuleMajorField, 2);
        Core::PreloadPlan plan{};
        RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
        Core::PreloadResult result{};
        RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-95", "模块主版本与 .os 不符");
        Require(sink.releaseCount == 5, "失败前已分配的落点必须归还");
        Require(result.recordCount == 0 && result.recordsPhys == 0, "失败不得交出记录");
    }

    // 用例：最小读取面五项（E-94）
    void CaseMinimalReadFace() {
        const char* const kNames[5] = { "Magic", "Version", "HeaderBytes", "Flags", "TotalBytes" };
        for (uint32_t variant = 0; variant < 5; ++variant) {
            FakeSink sink;
            PreparePreloadFixture(sink, 0x1000, 1);
            switch (variant) {
                case 0: g_modules[3][0] = 0x00; break;
                case 1: Put16(g_modules[3] + 0x08, 2); break;
                case 2: Put16(g_modules[3] + 0x0A, 0x40); break;
                case 3: Put32(g_modules[3] + 0x0C, 1); break;
                default: Put32(g_modules[3] + 0x10, 0x800); break;
            }
            Core::PreloadPlan plan{};
            RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
            Core::PreloadResult result{};
            RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-94", kNames[variant]);
        }
        // 清单 DataBytes 与实际文件长度不一致同样按最小读取面失败
        FakeSink sink;
        PreparePreloadFixture(sink, 0x1000, 1);
        g_artifacts[3].dataBytes = 0x1001;
        Core::PreloadPlan plan{};
        RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
        Core::PreloadResult result{};
        RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-94", "清单长度与实际长度不符");
    }

    // 用例：摘要不符（E-68）
    void CaseDigestMismatch() {
        FakeSink sink;
        PreparePreloadFixture(sink, 0x1000, 1);
        g_artifactDigests[5][0] ^= 0xFF;
        Core::PreloadPlan plan{};
        RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
        Core::PreloadResult result{};
        RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-68", "摘要不符");
        Require(sink.releaseCount == 6, "失败前已分配的落点必须归还");
    }

    // 用例：读失败（E-67）
    void CaseReadFailure() {
        {
            FakeSink sink;
            PreparePreloadFixture(sink, 0x1000, 1);
            sink.failOpenIndex = 4;
            Core::PreloadPlan plan{};
            RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
            Core::PreloadResult result{};
            RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-67", "模块文件打不开");
            Require(sink.releaseCount == 4, "打开失败前已分配的落点必须归还");
        }
        {
            FakeSink sink;
            PreparePreloadFixture(sink, 0x1000, 1);
            sink.failReadIndex = 2;
            Core::PreloadPlan plan{};
            RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
            Core::PreloadResult result{};
            RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-67", "模块文件读失败");
            Require(sink.releaseCount == 2, "读失败前已分配的落点必须归还");
        }
    }

    // 用例：落点分配失败（E-97）
    void CaseNoMemory() {
        FakeSink sink;
        PreparePreloadFixture(sink, 0x1000, 1);
        sink.failAllocateIndex = 3;
        Core::PreloadPlan plan{};
        RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
        Core::PreloadResult result{};
        RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-97", "落点分配失败");
        Require(result.recordCount == 0 && result.recordsPhys == 0 && result.moduleBytes == 0, "失败不得交出交接事实");
        Require(sink.releaseCount == 3, "落点失败前已分配的落点必须归还");
    }

    // 用例：失败不交接半套（再跑正例必须从干净记录开始）
    void CaseNoPartialHandoff() {
        {
            FakeSink sink;
            PreparePreloadFixture(sink, 0x1000, 1);
            Put16(g_modules[7] + kModuleMajorField, 9);
            Core::PreloadPlan plan{};
            RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
            Core::PreloadResult result{};
            RequireCode(Core::PreloadModules(plan, sink, result), "E-CORE-95", "中途失败");
            Require(result.recordCount == 0 && result.recordsPhys == 0, "失败后结果必须全零");
        }
        {
            FakeSink sink;
            PreparePreloadFixture(sink, 0x1000, 1);
            Core::PreloadPlan plan{};
            RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
            Core::PreloadResult result{};
            RequireOk(Core::PreloadModules(plan, sink, result), "失败之后的正例");
            Require(result.recordCount == kClosureCount, "记录数必须回到闭包成员数");
            const Core::PreloadArrayHeader* const header = reinterpret_cast<const Core::PreloadArrayHeader*>(static_cast<uintptr_t>(result.recordsPhys));
            Require(header->count == kClosureCount, "记录数组头必须回到完整条数");
            Require(header->reserved == 0, "记录数组头保留字段必须为零");
        }
    }

    // 用例：大模块按块读入
    void CaseLargeModule() {
        FakeSink sink;
        const uint32_t bytes = 0x18000;
        PreparePreloadFixture(sink, bytes, 1);
        Core::PreloadPlan plan{};
        RequireOk(Core::PlanPreload(MakeSource(1), plan), "合法列表");
        Core::PreloadResult result{};
        RequireOk(Core::PreloadModules(plan, sink, result), "大模块读入");
        Require(result.recordCount == kClosureCount, "记录数必须等于闭包成员数");
        Require(result.moduleBytes == static_cast<uint64_t>(kClosureCount) * bytes, "总字节数必须是全部模块之和");
        const Core::PreloadRecord* const records = Core::Records(result);
        const uint8_t* const data = sink.DataOf(records[kClosureCount - 1].phys);
        Require(data != nullptr, "最后一个模块的落点必须存在");
        Require(std::memcmp(data, g_modules[kClosureCount - 1], bytes) == 0, "读入内容必须与文件字节一致");
    }

    // 用例：屏幕事实模式 2 与 3 是 VGA 文本页
    void CaseScreenVgaText() {
        for (uint32_t mode = 2; mode <= 3; ++mode) {
            const Core::ScreenFacts facts = Core::DecodeVgaScreenFacts(static_cast<uint8_t>(mode), 80);
            Require(facts.kind == Boot::ScreenKind::VgaText, "模式 2 / 3 必须给 VgaText");
            Require(facts.address == 0xB8000, "VGA 文本页必须在 0xB8000");
            Require(facts.width == Core::kScreenColumns, "文本列数必须是 80");
            Require(facts.height == Core::kScreenRows, "文本行数必须是 25");
            Require(facts.stride == Core::kScreenStride, "每行字节数必须是 160");
            Require(facts.bytes == Core::kScreenBytes, "可写字节数必须是 4000");
            Require(facts.format == Boot::ScreenFormat::VgaText, "像素格式必须是 VgaText");
        }
    }

    // 用例：屏幕事实模式 7 是 MDA 文本页
    void CaseScreenMdaText() {
        const Core::ScreenFacts facts = Core::DecodeVgaScreenFacts(7, 80);
        Require(facts.kind == Boot::ScreenKind::VgaText, "模式 7 必须给 VgaText");
        Require(facts.address == 0xB0000, "MDA 文本页必须在 0xB0000");
        Require(facts.width == Core::kScreenColumns && facts.height == Core::kScreenRows, "MDA 几何同 80×25");
    }

    // 用例：非文本模式与列数不符都按未提供
    void CaseScreenNone() {
        const uint8_t unsupported[5] = { 0, 1, 4, 6, 0x13 };
        for (uint32_t i = 0; i < 5; ++i) {
            const Core::ScreenFacts facts = Core::DecodeVgaScreenFacts(unsupported[i], 80);
            Require(facts.kind == Boot::ScreenKind::None, "非文本模式必须给 None");
            Require(facts.address == 0, "未提供时地址必须为零");
        }
        const Core::ScreenFacts narrow = Core::DecodeVgaScreenFacts(3, 40);
        Require(narrow.kind == Boot::ScreenKind::None, "列数不是 80 时必须给 None");
        const Core::ScreenFacts zero = Core::DecodeVgaScreenFacts(3, 0);
        Require(zero.kind == Boot::ScreenKind::None, "列数为零时必须给 None");
    }

    // 用例：交接布局常量与记录字段偏移
    void CaseHandoffLayout() {
        // 记录数组：8 字节头 + 32 槽 × 16 字节，count 上限 30
        Require(sizeof(Core::PreloadRecord) == 16, "预装记录必须是 16 字节");
        Require(sizeof(Core::PreloadArrayHeader) == 8, "记录数组头必须是 8 字节");
        Require(sizeof(Core::PreloadArray) == 8 + 32 * 16, "记录数组必须是 8 + 32 × 16 字节");
        Require(Core::kPreloadArrayBytes == 520, "记录数组长度常量必须是 520");
        Require(Core::kPreloadSlots == 32, "静态槽位必须是 32");
        Require(Core::kPreloadMaxEntries == 30, "条目上限必须是 30");
        Require(Core::kBootListMaxBytes == 0x4000, "列表容量上限必须是 16 KiB");
        Require(Core::kPreloadPageBytes == 0x1000, "落点对齐必须是 4 KiB");
        Require(Core::kPreloadAddressLimit == 0x100000000ull, "落点必须在低 4 GiB");
        Require(Core::kPreloadLowLimit == 0x100000ull, "低 1 MiB 必须被避开");
        // 记录字段偏移：16 字节记录，字段顺序与偏移固定
        Require(offsetof(Core::PreloadRecord, phys) == 0x00, "记录的 phys 必须在 0x00");
        Require(offsetof(Core::PreloadRecord, bytes) == 0x08, "记录的 bytes 必须在 0x08");
        Require(offsetof(Core::PreloadRecord, artifactIndex) == 0x0C, "记录的 artifactIndex 必须在 0x0C");
        Require(offsetof(Core::PreloadRecord, reserved) == 0x0E, "记录的 reserved 必须在 0x0E");
        // BootInfo 交接包五字段与结构长度
        Require(Boot::InfoLayout::kFieldKernelSetManifest == 0xE0, "清单副本地址字段必须在 0xE0");
        Require(Boot::InfoLayout::kFieldBootList == 0xE8, "列表副本地址字段必须在 0xE8");
        Require(Boot::InfoLayout::kFieldBootModules == 0xF0, "记录数组地址字段必须在 0xF0");
        Require(Boot::InfoLayout::kFieldKernelSetManifestBytes == 0xF8, "清单副本长度字段必须在 0xF8");
        Require(Boot::InfoLayout::kFieldBootListBytes == 0xFC, "列表副本长度字段必须在 0xFC");
        Require(sizeof(Boot::Info) == 0x100, "BootInfo 长度必须是 0x100");
    }
}

// 入口：跑全部用例；有用例失败返回 1
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string arg = argv[1];
            if (arg == "-h" || arg == "--help") {
                std::cout <<
                    "CheckPreload — 引导闭包预装共享机制的宿主侧检查\n"
                    "\n"
                    "用法：CheckPreload\n"
                    "\n"
                    "检查对象是 BaleenCore 与 UEFI 侧链接的同一份 Packages/Baleen/Core/Src/BootClosure.cpp\n"
                    "（列表解析与期望集、内存图落点搜索、最小读取面 / 主版本门 / SHA-256 与记录填写）\n"
                    "以及 BIOS 侧屏幕事实的纯映射（Packages/Baleen/Core/Src/ScreenFacts.cpp）\n";
                return 0;
            }
            Fail("无法识别的参数：" + arg);
        }
        Case("落点：空图、零长度与超界", CaseFindSpanDegenerate);
        Case("落点：相邻可用条目合并与空洞", CaseFindSpanAdjacent);
        Case("落点：避开引导器与内核装载跨度", CaseFindSpanAvoidSpans);
        Case("落点：避开低 1MiB", CaseFindSpanLowMemory);
        Case("落点：末端跨 4GiB 不上送", CaseFindSpanAddressLimit);
        Case("落点：64 位长度回绕", CaseFindSpanWraparound);
        Case("落点：结果按 4KiB 对齐", CaseFindSpanAlignment);
        Case("落点：已分配落点互斥与分配器", CaseFindSpanAllocator);
        Case("列表：合法 13 条产出计划", CasePlanValid);
        Case("列表：容量超 16KiB", CasePlanTooLarge);
        Case("列表：条目超 30 条", CasePlanTooManyEntries);
        Case("列表：重复路径", CasePlanDuplicatePath);
        Case("列表：路径不在 Modules/ 下", CasePlanOutsideModules);
        Case("列表：路径含 ..", CasePlanDotDot);
        Case("列表：条目不在清单", CasePlanNotInManifest);
        Case("列表：固定头与结尾换行", CasePlanMalformedHeader);
        Case("期望集：漏 / 重", CaseExpectedSetMismatch);
        Case("列表：运行期条目只忽略、不预装", CasePlanSkipsRuntimeEntries);
        Case("主版本门：列表与 .os 不符", CaseListMajorGate);
        Case("主版本门：模块头与 .os 不符", CaseModuleMajorGate);
        Case("最小读取面：头 0x00..0x14 五项", CaseMinimalReadFace);
        Case("摘要不符返回 68", CaseDigestMismatch);
        Case("读失败返回 67", CaseReadFailure);
        Case("落点分配失败返回 97", CaseNoMemory);
        Case("失败不交接半套", CaseNoPartialHandoff);
        Case("大模块分块读入", CaseLargeModule);
        Case("屏幕事实：模式 2 / 3 是 VGA 文本页", CaseScreenVgaText);
        Case("屏幕事实：模式 7 是 MDA 文本页", CaseScreenMdaText);
        Case("屏幕事实：非文本模式与列数不符给 None", CaseScreenNone);
        Case("交接布局常量与记录偏移", CaseHandoffLayout);
        if (g_ran != kCaseCount) Fail("内部回归用例数量不符：实跑 " + std::to_string(g_ran) + "，声明 " + std::to_string(kCaseCount));
        const unsigned passed = g_ran - g_failed;
        std::cout << "CheckPreload: " << passed << "/" << g_ran << " cases passed" << std::endl;
        return g_failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "CheckPreload: " << error.what() << '\n';
        return 1;
    }
}
