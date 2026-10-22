// CheckLoader：模块装载编排（11 步）的宿主侧检查
// 复用内核同一份 ModuleLoader / ModuleRegistry / ModuleContainer / InterfaceRegistry / DependencyGraph / Objects
// 用宿主替身提供物理映射、页帧分配与入口调用，注入每步失败并核对回滚、状态与「Init 前不执行」
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

#include <Abi.hpp>
#include <BootInfo.hpp>
#include <DependencyGraph.hpp>
#include <Entry.hpp>
#include <Handoff.hpp>
#include <InterfaceRegistry.hpp>
#include <ManifestView.hpp>
#include <ModuleContainer.hpp>
#include <ModuleLoader.hpp>
#include <ModuleRegistry.hpp>
#include <Objects.hpp>
#include <Sha256.hpp>
#include <Types.hpp>

// 内核侧的有界等待在宿主检查里不做真等待（Core.Time 槽 2 引用它）
extern "C" bool _Kernel_WaitMs(uint32_t) { return true; }
// 内核侧的 TSC 标定在宿主检查里给固定值（Core.Time 槽 1 引用它）：PIT 标定属平台代码，不在宿主跑
extern "C" uint64_t _Kernel_TicksPerMs() { return 1000; }

namespace {
    int g_total = 0;
    int g_failed = 0;

    void Check(bool ok, const std::string& name, const char* detail = nullptr) {
        ++g_total;
        if (ok) {
            printf("PASS %s\n", name.c_str());
            return;
        }
        ++g_failed;
        printf("FAIL %s%s%s\n", name.c_str(), detail != nullptr ? " - " : "", detail != nullptr ? detail : "");
    }

    // —— 字节写入 ——
    void Put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
        bytes[offset] = static_cast<uint8_t>(value & 0xFF);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
        for (uint32_t index = 0; index < 4; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
    }

    void Put64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
        for (uint32_t index = 0; index < 8; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
    }

    uint64_t Get64(const std::vector<uint8_t>& bytes, size_t offset) {
        uint64_t value = 0;
        for (uint32_t index = 0; index < 8; ++index) value |= static_cast<uint64_t>(bytes[offset + index]) << (8 * index);
        return value;
    }

    size_t Align(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

    // —— 容器构造：按容器字节表拼一份合法容器 ——
    struct SectionSpec {
        uint32_t type = 1;                 // 1 TEXT / 2 RODATA / 3 DATA / 4 BSS
        uint32_t flags = 5;                // R|X
        uint32_t memoryOffset = 0;         // 相对模块基址
        uint32_t memoryBytes = 0x1000;     // 内存跨度
        uint32_t align = 4096;             // 段对齐
        std::vector<uint8_t> fileBytes;    // 文件内容（BSS 为空）
    };

    struct ExportSpec {
        InterfaceId id = {};
        uint16_t version = 1;
        std::vector<uint32_t> slotOffsets;   // 槽位：TEXT 段内的偏移
    };

    struct ImportSpec {
        InterfaceId id = {};
        uint16_t minVersion = 1;
        uint16_t maxVersion = 0xFFFF;
        std::vector<uint32_t> patchOffsets;  // 回填槽：DATA/BSS 内的 8 字节对齐偏移
    };

    struct DependencySpec {
        ModuleId id = {};
        uint16_t minMajor = 1;
        uint16_t minMinor = 0;
        uint16_t minPatch = 0;
        uint16_t maxMajor = 0;
        uint16_t maxMinor = 0;
        uint16_t maxPatch = 0;
    };

    struct RelocSpec {
        uint8_t type = 1;      // 1 RELATIVE64 / 2 RELATIVE32
        uint32_t target = 0;   // DATA/BSS 内偏移
        uint64_t addend = 0;
    };

    struct ContainerSpec {
        ModuleId moduleId = {};
        uint16_t major = 1;
        uint16_t minor = 0;
        uint16_t patch = 0;
        uint16_t abiMin = 1;
        uint16_t abiMax = 0xFFFF;
        uint32_t entryInit = 0x10;
        uint32_t entryActivate = 0;
        uint32_t entryAbort = 0x20;
        std::string moduleName = "CheckModule";
        std::vector<SectionSpec> sections;
        std::vector<ExportSpec> exports;
        std::vector<ImportSpec> imports;
        std::vector<DependencySpec> dependencies;
        std::vector<RelocSpec> relocations;
    };

    // 一份可装载的默认容器：TEXT 0x1000 + DATA 0x1000 + BSS 0x1000
    ContainerSpec DefaultSpec(const ModuleId& moduleId) {
        ContainerSpec spec;
        spec.moduleId = moduleId;
        SectionSpec text;
        text.type = 1;
        text.flags = 5;
        text.memoryOffset = 0;
        text.memoryBytes = 0x1000;
        text.fileBytes.assign(0x100, 0x90);
        SectionSpec data;
        data.type = 3;
        data.flags = 3;
        data.memoryOffset = 0x1000;
        data.memoryBytes = 0x1000;
        data.fileBytes.assign(0x100, 0x11);
        SectionSpec bss;
        bss.type = 4;
        bss.flags = 3;
        bss.memoryOffset = 0x2000;
        bss.memoryBytes = 0x1000;
        spec.sections = {text, data, bss};
        return spec;
    }

    std::vector<uint8_t> BuildContainer(const ContainerSpec& spec) {
        const uint32_t headerBytes = 0x80;
        // 布局：文件头 → 段表 → 段文件区 → 导出表与槽位表 → 导入表与回填表 → 依赖表 → 重定位表 → 字符串表
        // 段文件偏移 4 字节对齐、重定位表 8 字节对齐，各表区与段文件区互不重叠
        const uint32_t sectionTableBytes = static_cast<uint32_t>(spec.sections.size()) * Kernel::kContainerSectionEntryBytes;
        std::vector<uint8_t> strings;
        strings.push_back(0);   // 偏移 0 是空串
        const uint32_t nameOffset = static_cast<uint32_t>(strings.size());
        strings.insert(strings.end(), spec.moduleName.begin(), spec.moduleName.end());
        strings.push_back(0);
        size_t finalCursor = headerBytes + sectionTableBytes;
        std::vector<uint32_t> finalSectionOffsets(spec.sections.size(), 0);
        for (size_t index = 0; index < spec.sections.size(); ++index) {
            if (spec.sections[index].fileBytes.empty()) continue;
            finalCursor = Align(finalCursor, 4);
            finalSectionOffsets[index] = static_cast<uint32_t>(finalCursor);
            finalCursor += spec.sections[index].fileBytes.size();
        }
        finalCursor = Align(finalCursor, 4);
        const uint32_t finalExportTable = static_cast<uint32_t>(finalCursor);
        finalCursor += spec.exports.size() * 28;
        finalCursor = Align(finalCursor, 4);
        const uint32_t finalExportSlots = static_cast<uint32_t>(finalCursor);
        for (const ExportSpec& item : spec.exports) finalCursor += item.slotOffsets.size() * 4;
        finalCursor = Align(finalCursor, 4);
        const uint32_t finalImportPatchTable = static_cast<uint32_t>(finalCursor);
        for (const ImportSpec& item : spec.imports) finalCursor += item.patchOffsets.size() * 4;
        finalCursor = Align(finalCursor, 4);
        const uint32_t finalImportTable = static_cast<uint32_t>(finalCursor);
        finalCursor += spec.imports.size() * 28;
        finalCursor = Align(finalCursor, 4);
        const uint32_t finalDependencyTable = static_cast<uint32_t>(finalCursor);
        finalCursor += spec.dependencies.size() * 32;
        finalCursor = Align(finalCursor, 8);
        const uint32_t finalRelocationTable = static_cast<uint32_t>(finalCursor);
        finalCursor += spec.relocations.size() * 16;
        finalCursor = Align(finalCursor, 4);
        const uint32_t finalStringTable = static_cast<uint32_t>(finalCursor);
        finalCursor += strings.size();
        const uint32_t finalTotal = static_cast<uint32_t>(finalCursor);

        const uint8_t magic[8] = {'L', 'P', 'O', 'S', 'M', 'O', 'D', 0};
        std::vector<uint8_t> bytes(finalTotal, 0);
        memcpy(&bytes[0], magic, sizeof(magic));
        Put16(bytes, 0x08, Kernel::kContainerVersion);
        Put16(bytes, 0x0A, static_cast<uint16_t>(Kernel::kContainerHeaderBytes));
        Put32(bytes, 0x0C, 0);
        Put32(bytes, 0x10, finalTotal);
        Put32(bytes, 0x14, 0x3000);
        Put32(bytes, 0x18, spec.entryInit);
        Put32(bytes, 0x1C, spec.entryActivate);
        Put32(bytes, 0x20, spec.entryAbort);
        Put16(bytes, 0x24, static_cast<uint16_t>(headerBytes));
        Put16(bytes, 0x26, static_cast<uint16_t>(spec.sections.size()));
        Put16(bytes, 0x28, static_cast<uint16_t>(Kernel::kContainerSectionEntryBytes));
        Put16(bytes, 0x2A, static_cast<uint16_t>(spec.exports.size()));
        Put32(bytes, 0x2C, finalExportTable);
        Put16(bytes, 0x30, static_cast<uint16_t>(spec.imports.size()));
        Put16(bytes, 0x32, static_cast<uint16_t>(Kernel::kContainerImportEntryBytes));
        Put32(bytes, 0x34, finalImportTable);
        Put16(bytes, 0x38, static_cast<uint16_t>(spec.dependencies.size()));
        Put16(bytes, 0x3A, static_cast<uint16_t>(Kernel::kContainerDependencyEntryBytes));
        Put32(bytes, 0x3C, finalDependencyTable);
        Put32(bytes, 0x40, static_cast<uint32_t>(spec.relocations.size()));
        Put32(bytes, 0x44, finalRelocationTable);
        Put32(bytes, 0x48, finalStringTable);
        Put32(bytes, 0x4C, static_cast<uint32_t>(strings.size()));
        memcpy(&bytes[0x50], spec.moduleId.bytes, 16);
        Put32(bytes, 0x60, nameOffset);
        Put16(bytes, 0x64, static_cast<uint16_t>(spec.moduleName.size()));
        Put16(bytes, 0x66, spec.major);
        Put16(bytes, 0x68, spec.minor);
        Put16(bytes, 0x6A, spec.patch);
        Put16(bytes, 0x6C, spec.abiMin);
        Put16(bytes, 0x6E, spec.abiMax);

        // 段表
        for (size_t index = 0; index < spec.sections.size(); ++index) {
            const SectionSpec& section = spec.sections[index];
            const size_t offset = headerBytes + index * Kernel::kContainerSectionEntryBytes;
            Put32(bytes, offset + 0x00, 0);   // 匿名段
            Put32(bytes, offset + 0x04, section.type);
            Put32(bytes, offset + 0x08, section.flags);
            Put32(bytes, offset + 0x0C, finalSectionOffsets[index]);
            Put32(bytes, offset + 0x10, static_cast<uint32_t>(section.fileBytes.size()));
            Put32(bytes, offset + 0x14, section.memoryOffset);
            Put32(bytes, offset + 0x18, section.memoryBytes);
            Put32(bytes, offset + 0x1C, section.align);
            if (!section.fileBytes.empty()) memcpy(&bytes[finalSectionOffsets[index]], section.fileBytes.data(), section.fileBytes.size());
        }
        // 导出表与槽位表
        size_t slotCursor = finalExportSlots;
        for (size_t index = 0; index < spec.exports.size(); ++index) {
            const ExportSpec& item = spec.exports[index];
            const size_t offset = finalExportTable + index * 28;
            memcpy(&bytes[offset + 0x00], item.id.bytes, 16);
            Put16(bytes, offset + 0x10, item.version);
            Put16(bytes, offset + 0x12, static_cast<uint16_t>(item.slotOffsets.size()));
            Put32(bytes, offset + 0x14, static_cast<uint32_t>(slotCursor));
            Put32(bytes, offset + 0x18, 0);
            for (uint32_t slot = 0; slot < item.slotOffsets.size(); ++slot) Put32(bytes, slotCursor + slot * 4, item.slotOffsets[slot]);
            slotCursor += item.slotOffsets.size() * 4;
        }
        // 导入表：回填表从 finalImportPatchTable 起，逐条接在上一段之后
        size_t patchCursor = finalImportPatchTable;
        for (size_t index = 0; index < spec.imports.size(); ++index) {
            const ImportSpec& item = spec.imports[index];
            const size_t offset = finalImportTable + index * 28;
            memcpy(&bytes[offset + 0x00], item.id.bytes, 16);
            Put16(bytes, offset + 0x10, item.minVersion);
            Put16(bytes, offset + 0x12, item.maxVersion);
            Put32(bytes, offset + 0x14, static_cast<uint32_t>(patchCursor));
            Put16(bytes, offset + 0x18, static_cast<uint16_t>(item.patchOffsets.size()));
            Put16(bytes, offset + 0x1A, 0);
            for (uint32_t slot = 0; slot < item.patchOffsets.size(); ++slot) Put32(bytes, patchCursor + slot * 4, item.patchOffsets[slot]);
            patchCursor += item.patchOffsets.size() * 4;
        }
        // 依赖表
        for (size_t index = 0; index < spec.dependencies.size(); ++index) {
            const DependencySpec& item = spec.dependencies[index];
            const size_t offset = finalDependencyTable + index * 32;
            memcpy(&bytes[offset + 0x00], item.id.bytes, 16);
            Put16(bytes, offset + 0x10, item.minMajor);
            Put16(bytes, offset + 0x12, item.minMinor);
            Put16(bytes, offset + 0x14, item.minPatch);
            Put16(bytes, offset + 0x16, item.maxMajor);
            Put16(bytes, offset + 0x18, item.maxMinor);
            Put16(bytes, offset + 0x1A, item.maxPatch);
            Put32(bytes, offset + 0x1C, 0);
        }
        // 重定位表
        for (size_t index = 0; index < spec.relocations.size(); ++index) {
            const RelocSpec& item = spec.relocations[index];
            const size_t offset = finalRelocationTable + index * 16;
            bytes[offset + 0x00] = item.type;
            bytes[offset + 0x01] = 0;
            Put16(bytes, offset + 0x02, 0);
            Put32(bytes, offset + 0x04, item.target);
            Put64(bytes, offset + 0x08, item.addend);
        }
        // 字符串表
        memcpy(&bytes[finalStringTable], strings.data(), strings.size());
        return bytes;
    }

    // —— 宿主替身平台：假物理空间是一段宿主缓冲，物理地址 = 基址 + 偏移 ——
    class FakePlatform final : public Kernel::LoaderPlatform {
    public:
        static constexpr uint64_t kArenaBase = 0x20000000;
        static constexpr uint32_t kPageBytes = 4096;

        explicit FakePlatform(size_t arenaBytes) : arena(arenaBytes, 0) {}

        std::vector<uint8_t> arena;      // 假物理空间
        uint64_t next = 0;               // 假分配水位
        uint32_t entryCalls = 0;         // 入口调用次数（Init 前不执行由它验证）
        uint32_t abortCalls = 0;         // Abort 调用次数
        uint32_t freeCalls = 0;          // FreePages 调用次数
        uint32_t freedPages = 0;         // 归还的页数
        bool allocateFails = false;      // 注入映射失败
        KernelError initResult = KernelError::kOk;   // Init 返回值
        bool registerObjectInInit = false;           // Init 里登记一个对象，验证归属
        uint32_t registeredObject = 0;               // 登记的句柄
        uint32_t abortEntryOffset = 0x20;            // 容器里的 Abort 入口偏移
        uint64_t lastBase = 0;                       // 最近一次入口调用的基址
        KernelAbiContext lastContext{};              // 最近一次入口上下文

        void* MapPhysical(uint64_t physical) override {
            if (physical < kArenaBase) return nullptr;
            const uint64_t offset = physical - kArenaBase;
            if (offset >= arena.size()) return nullptr;
            return arena.data() + offset;
        }

        uint64_t AllocatePages(uint32_t count) override {
            if (allocateFails) return 0;
            const uint64_t bytes = static_cast<uint64_t>(count) * kPageBytes;
            if (next + bytes > arena.size()) return 0;
            const uint64_t physical = kArenaBase + next;
            next += bytes;
            return physical;
        }

        void FreePages(uint64_t physical, uint32_t count) override {
            (void)physical;
            ++freeCalls;
            freedPages += count;
        }


        KernelError CallEntry(uint64_t base, uint32_t entryOffset, const KernelAbiContext& context) override {
            if (entryOffset == abortEntryOffset) {
                ++abortCalls;
                return KernelError::kOk;
            }
            ++entryCalls;
            lastBase = base;
            lastContext = context;
            if (registerObjectInInit) {
                Kernel::Objects::ResourceRange range{};
                ObjectHandle handle = 0;
                (void)Kernel::Objects::Register(7, 0x1234, range, &handle);
                registeredObject = handle;
            }
            return initResult;
        }
    };

    // —— 检查环境：内核核心导出 + 模块注册表 + 假平台 ——
    struct Env {
        FakePlatform platform{8u * 1024 * 1024};
        Kernel::InterfaceRegistry registry;
        Kernel::ModuleRegistry modules;
        Boot::Info bootInfo{};

        Env() {
            bootInfo.bytes = Boot::InfoLayout::kBytes;
            bootInfo.version = Boot::InfoLayout::kVersion;
            for (uint32_t index = 0; index < 32; ++index) bootInfo.kernelSetBuildId[index] = static_cast<uint8_t>(0x40 + index);
            const char* reason = nullptr;
            (void)Kernel::InitializeCoreExports(registry, nullptr, &bootInfo, reason);
            Kernel::Objects::Reset();
        }

        Kernel::LoaderContext Context(uint16_t osModuleMajor = 1) {
            Kernel::LoaderContext context;
            context.platform = &platform;
            context.registry = &registry;
            context.modules = &modules;
            context.osModuleMajor = osModuleMajor;
            context.bootInfo = &bootInfo;
            return context;
        }
    };

    // 装载一次：构造容器、可选地改字节、按需造闭包来源事实
    struct LoadRun {
        Kernel::LoadOutcome outcome{};
        std::vector<uint8_t> bytes;
        std::string path = "Modules/CheckModule.1.0.0.osm";
        uint8_t digest[32] = {};
    };

    Kernel::Diagnostics::Diagnostic LoadOne(Env& env, const ContainerSpec& spec, LoadRun& run, const std::function<void(std::vector<uint8_t>&)>& mutate = nullptr, bool closure = false, const std::function<void(std::vector<uint8_t>&)>& mutateAfterDigest = nullptr) {
        run.bytes = BuildContainer(spec);
        if (mutate) mutate(run.bytes);
        Common::Digest::Sha256 sha;
        sha.Begin();
        sha.Update(run.bytes.data(), static_cast<uint32_t>(run.bytes.size()));
        sha.Finish(run.digest);
        // 摘要在前、改字节在后：制造“清单摘要与实际字节不符”的现场
        if (mutateAfterDigest) mutateAfterDigest(run.bytes);

        Kernel::ManifestEntry entry{};
        entry.type = KernelSet::ManifestLayout::kTypeOsm;
        entry.pathBytes = static_cast<uint32_t>(run.path.size());
        entry.path = reinterpret_cast<const uint8_t*>(run.path.data());
        entry.dataBytes = run.bytes.size();
        entry.digest = run.digest;

        Kernel::HandoffFacts handoff{};
        if (closure) {
            handoff.manifest.entries[0] = entry;
            handoff.manifest.entryCount = 1;
            handoff.list.count = 1;
            handoff.list.paths[0] = entry.path;
            handoff.list.pathBytes[0] = static_cast<uint16_t>(entry.pathBytes);
            handoff.records[0].phys = 0x30000000;
            handoff.records[0].bytes = static_cast<uint32_t>(run.bytes.size());
            handoff.records[0].artifactIndex = 0;
            handoff.recordCount = 1;
        }

        Kernel::LoadRequest request{};
        request.bytes = run.bytes.data();
        request.bytesCount = static_cast<uint32_t>(run.bytes.size());
        request.manifestEntry = &entry;
        request.handoff = closure ? &handoff : nullptr;
        request.closureModule = closure;
        request.preloadOrder = 1;
        const Kernel::LoaderContext context = env.Context();
        return Kernel::LoadModule(context, request, run.outcome);
    }

    // 测试用模块标识：用生成头常量（闭包成员）与自造标识各取所需
    ModuleId TestModuleId() {
        ModuleId id{};
        for (uint32_t index = 0; index < 16; ++index) id.bytes[index] = static_cast<uint8_t>(0x10 + index);
        return id;
    }

    // 取注册表里某个导出的槽位表（用于核对导入回填）
    const void* const* ExportSlots(const Kernel::InterfaceRegistry& registry, const InterfaceId& id, uint16_t version) {
        const Kernel::RegisteredExport* const entry = registry.Find(id, version);
        if (entry == nullptr) return nullptr;
        return reinterpret_cast<const void* const*>(entry->entry.slots);
    }
}

int main() {
    // —— 1. 正例：完整模块（导出 + 导入 Core.Log + 依赖 + 重定位）——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        ExportSpec exportSpec;
        exportSpec.id = kIfCoreMem;   // 内核核心已提供同版本 → 会冲突，这里换一个自造接口
        for (uint32_t index = 0; index < 16; ++index) exportSpec.id.bytes[index] = static_cast<uint8_t>(0xA0 + index);
        exportSpec.version = 1;
        exportSpec.slotOffsets = {0x30, 0x40};
        spec.exports = {exportSpec};
        ImportSpec importSpec;
        importSpec.id = kIfCoreLog;
        importSpec.minVersion = 1;
        importSpec.maxVersion = 1;
        importSpec.patchOffsets = {0x1000, 0x1008};
        spec.imports = {importSpec};
        DependencySpec dependency;
        dependency.id = kModDevModel;
        spec.dependencies = {dependency};
        RelocSpec relocation;
        relocation.type = 1;
        relocation.target = 0x1010;
        relocation.addend = 0x28;
        spec.relocations = {relocation};
        // 依赖提供者：已 Initialized 的 DevModel
        (void)env.modules.Add(kModDevModel, 1, 0, 0, 0, 0, 0, 1);
        (void)env.modules.SetState(kModDevModel, Kernel::ModuleState::Initialized);

        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code == nullptr, "正例：装载成功", run.outcome.reason);
        Check(run.outcome.state == Kernel::ModuleState::Initialized, "正例：模块状态 Initialized");
        Check(env.platform.entryCalls == 1, "正例：Init 调用一次");
        Check(run.outcome.pageCount == 3 && run.outcome.base != 0, "正例：映像 3 页");
        const Kernel::ModuleRecord* const record = env.modules.Find(TestModuleId());
        Check(record != nullptr && record->state == Kernel::ModuleState::Initialized, "正例：注册表记录 Initialized");
        Check(env.registry.Find(exportSpec.id, 1) != nullptr, "正例：导出已登记");
        // 段拷贝、BSS 清零、重定位与导入回填
        const uint8_t* const image = static_cast<const uint8_t*>(env.platform.MapPhysical(run.outcome.base));
        Check(image != nullptr && image[0] == 0x90 && image[0xFF] == 0x90, "正例：TEXT 段已拷贝");
        Check(image != nullptr && image[0x1050] == 0x11, "正例：DATA 段已拷贝");   // 0x1000/0x1008 被导入回填、0x1010 被重定位占用
        Check(image != nullptr && image[0x2000] == 0x00 && image[0x2FFF] == 0x00, "正例：BSS 已清零");
        Check(image != nullptr && Get64(std::vector<uint8_t>(image + 0x1010, image + 0x1018), 0) == run.outcome.base + 0x28, "正例：重定位写入 Base + Addend");
        const void* const* const coreLogSlots = ExportSlots(env.registry, kIfCoreLog, 1);
        Check(coreLogSlots != nullptr && image != nullptr && memcmp(image + 0x1000, &coreLogSlots[0], 8) == 0, "正例：导入槽 0 回填为 Core.Log 槽 0");
        Check(coreLogSlots != nullptr && image != nullptr && memcmp(image + 0x1008, &coreLogSlots[1], 8) == 0, "正例：导入槽 1 回填为 Core.Log 槽 1");
        Check(env.platform.freeCalls == 0, "正例：成功不归还映像页");
    }

    // —— 2. 第 1 步：摘要与来源 ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run, nullptr, false, [](std::vector<uint8_t>& bytes) { bytes[0x90] ^= 0xFF; });
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-58") == 0, "摘要不符 → E-KRN-58", run.outcome.reason);
        Check(env.platform.entryCalls == 0, "摘要不符：Init 未执行");
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run, nullptr, true);
        Check(diagnostic.code == nullptr, "闭包模块正例：来源核对通过", run.outcome.reason);
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        // 闭包模块但请求里没有交接事实：来源核对失败
        run.bytes = BuildContainer(spec);
        Common::Digest::Sha256 sha;
        sha.Begin();
        sha.Update(run.bytes.data(), static_cast<uint32_t>(run.bytes.size()));
        sha.Finish(run.digest);
        Kernel::ManifestEntry entry{};
        entry.type = KernelSet::ManifestLayout::kTypeOsm;
        entry.pathBytes = static_cast<uint32_t>(run.path.size());
        entry.path = reinterpret_cast<const uint8_t*>(run.path.data());
        entry.dataBytes = run.bytes.size();
        entry.digest = run.digest;
        Kernel::LoadRequest request{};
        request.bytes = run.bytes.data();
        request.bytesCount = static_cast<uint32_t>(run.bytes.size());
        request.manifestEntry = &entry;
        request.closureModule = true;
        const Kernel::LoaderContext context = env.Context();
        const Kernel::Diagnostics::Diagnostic diagnostic = Kernel::LoadModule(context, request, run.outcome);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-58") == 0, "闭包模块缺交接事实 → E-KRN-58", run.outcome.reason);
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        // 清单条目类型不是 .osm
        run.bytes = BuildContainer(spec);
        Common::Digest::Sha256 sha;
        sha.Begin();
        sha.Update(run.bytes.data(), static_cast<uint32_t>(run.bytes.size()));
        sha.Finish(run.digest);
        Kernel::ManifestEntry entry{};
        entry.type = KernelSet::ManifestLayout::kTypeOs;
        entry.pathBytes = static_cast<uint32_t>(run.path.size());
        entry.path = reinterpret_cast<const uint8_t*>(run.path.data());
        entry.dataBytes = run.bytes.size();
        entry.digest = run.digest;
        Kernel::LoadRequest request{};
        request.bytes = run.bytes.data();
        request.bytesCount = static_cast<uint32_t>(run.bytes.size());
        request.manifestEntry = &entry;
        const Kernel::LoaderContext context = env.Context();
        const Kernel::Diagnostics::Diagnostic diagnostic = Kernel::LoadModule(context, request, run.outcome);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-58") == 0, "清单条目不是 .osm → E-KRN-58", run.outcome.reason);
    }

    // —— 3. 第 2/3 步：容器结构 ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run, [](std::vector<uint8_t>& bytes) { bytes[0] = 'X'; });
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-57") == 0, "容器魔数错 → E-KRN-57", run.outcome.reason);
        Check(env.platform.entryCalls == 0, "容器魔数错：Init 未执行");
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        spec.sections[1].memoryOffset = 0;   // 与 TEXT 段内存区间重叠
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-57") == 0, "段内存区间重叠 → E-KRN-57", run.outcome.reason);
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        spec.entryInit = 0x5000;   // 入口不在 TEXT 段
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-57") == 0, "入口不在 TEXT → E-KRN-57", run.outcome.reason);
    }

    // —— 4. 第 4 步：标识、主版本与 ABI ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        spec.moduleId = ModuleId{};   // 全零标识
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-57") == 0, "ModuleId 全零 → E-KRN-57", run.outcome.reason);
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        spec.major = 2;   // 与 .os 的 1 不等
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-56") == 0, "主版本硬门 → E-KRN-56", run.outcome.reason);
        Check(env.platform.entryCalls == 0, "主版本硬门：Init 未执行");
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        spec.abiMin = 2;   // 内核 ABI 是 1，不在区间内
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "ABI 区间不接受 → E-KRN-59", run.outcome.reason);
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic first = LoadOne(env, spec, run);
        Check(first.code == nullptr, "重复装载前置：第一次成功", run.outcome.reason);
        LoadRun second;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, second);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-57") == 0, "重复装载同一 ModuleId → E-KRN-57", second.outcome.reason);
    }

    // —— 5. 第 5 步：提供者唯一 ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        ExportSpec exportSpec;
        exportSpec.id = kIfCoreLog;   // 内核核心已提供 Core.Log v1
        exportSpec.version = 1;
        exportSpec.slotOffsets = {0x30};
        spec.exports = {exportSpec};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "提供者重复 → E-KRN-59", run.outcome.reason);
        Check(env.platform.entryCalls == 0, "提供者重复：Init 未执行");
    }

    // —— 6. 第 6 步：映射 ——
    {
        Env env;
        env.platform.allocateFails = true;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "映射失败 → E-KRN-59", run.outcome.reason);
        Check(env.platform.entryCalls == 0, "映射失败：Init 未执行");
        Check(env.modules.Count() == 0, "映射失败：注册表不留记录");
    }

    // —— 7. 第 7 步：重定位 ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        RelocSpec relocation;
        relocation.type = 2;            // RELATIVE32
        relocation.target = 0x1010;
        relocation.addend = 0x100000000ull;   // 超过 32 位
        spec.relocations = {relocation};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-57") == 0, "32 位重定位溢出 → E-KRN-57", run.outcome.reason);
        Check(env.platform.entryCalls == 0, "重定位失败：Init 未执行");
        Check(env.platform.freeCalls == 1 && env.platform.freedPages == 3, "重定位失败：映像页已归还");
        const Kernel::ModuleRecord* const record = env.modules.Find(TestModuleId());
        Check(record != nullptr && record->state == Kernel::ModuleState::Failed, "重定位失败：记录为 Failed");
    }

    // —— 8. 第 8 步：导入解析 ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        ImportSpec importSpec;
        importSpec.id = kIfCoreMem;   // 内核核心不导出 Core.Mem 的版本 9
        importSpec.minVersion = 9;
        importSpec.maxVersion = 9;
        importSpec.patchOffsets = {0x1000};
        spec.imports = {importSpec};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "导入版本不存在 → E-KRN-59", run.outcome.reason);
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        ImportSpec importSpec;
        importSpec.id = kIfCoreLog;
        importSpec.minVersion = 1;
        importSpec.maxVersion = 1;
        importSpec.patchOffsets = {0x1000, 0x1008, 0x1010, 0x1018, 0x1020};   // 5 > 提供者 4 槽
        spec.imports = {importSpec};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "导入槽数超过提供者 → E-KRN-59", run.outcome.reason);
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        ImportSpec importSpec;
        for (uint32_t index = 0; index < 16; ++index) importSpec.id.bytes[index] = static_cast<uint8_t>(0xE0 + index);
        importSpec.patchOffsets = {0x1000};
        spec.imports = {importSpec};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "导入无提供者 → E-KRN-59", run.outcome.reason);
    }

    // —— 9. 第 9 步：依赖检查 ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        DependencySpec dependency;
        dependency.id = kModDevModel;   // 未装载
        spec.dependencies = {dependency};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "依赖未装载 → E-KRN-59", run.outcome.reason);
    }
    {
        Env env;
        (void)env.modules.Add(kModDevModel, 1, 0, 0, 0, 0, 0, 1);   // 登记但未 Initialized
        ContainerSpec spec = DefaultSpec(TestModuleId());
        DependencySpec dependency;
        dependency.id = kModDevModel;
        spec.dependencies = {dependency};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "依赖未 Initialized → E-KRN-59", run.outcome.reason);
    }
    {
        Env env;
        (void)env.modules.Add(kModDevModel, 2, 0, 0, 0, 0, 0, 1);
        (void)env.modules.SetState(kModDevModel, Kernel::ModuleState::Initialized);
        ContainerSpec spec = DefaultSpec(TestModuleId());
        DependencySpec dependency;
        dependency.id = kModDevModel;
        dependency.maxMajor = 1;   // 提供者 2.0.0 超出 [1.0.0, 1.x]
        spec.dependencies = {dependency};
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "依赖版本不匹配 → E-KRN-59", run.outcome.reason);
    }

    // —— 10/11. 归属上下文与 Init 失败回滚 ——
    {
        Env env;
        env.platform.registerObjectInInit = true;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code == nullptr, "归属正例：装载成功", run.outcome.reason);
        ModuleId owner{};
        const bool owned = Kernel::Objects::Owner(env.platform.registeredObject, &owner) == Kernel::Objects::ObjectResult::Ok;
        Check(owned && owner == TestModuleId(), "归属正例：对象归模块所有");
    }
    {
        Env env;
        env.platform.registerObjectInInit = true;
        env.platform.initResult = KernelError::kCorrupt;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "Init 失败 → E-KRN-59", run.outcome.reason);
        Check(env.platform.entryCalls == 1 && env.platform.abortCalls == 1, "Init 失败：调用 Abort 一次");
        Check(env.platform.freeCalls == 1 && env.platform.freedPages == 3, "Init 失败：映像页已归还");
        Check(Kernel::Objects::Count() == 0, "Init 失败：对象归属已回收");
        const Kernel::ModuleRecord* const record = env.modules.Find(TestModuleId());
        Check(record != nullptr && record->state == Kernel::ModuleState::Failed, "Init 失败：记录为 Failed");
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        spec.entryAbort = 0;   // 没有 Abort 入口
        env.platform.initResult = KernelError::kIo;
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "Init 失败且无 Abort → E-KRN-59", run.outcome.reason);
        Check(env.platform.abortCalls == 0, "Init 失败且无 Abort：不调用 Abort");
        Check(env.platform.freeCalls == 1, "Init 失败且无 Abort：映像页已归还");
    }
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        ExportSpec exportSpec;
        for (uint32_t index = 0; index < 16; ++index) exportSpec.id.bytes[index] = static_cast<uint8_t>(0xB0 + index);
        exportSpec.version = 1;
        exportSpec.slotOffsets = {0x30};
        spec.exports = {exportSpec};
        env.platform.initResult = KernelError::kState;
        LoadRun run;
        const Kernel::Diagnostics::Diagnostic diagnostic = LoadOne(env, spec, run);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "Init 失败：导出不发布 → E-KRN-59", run.outcome.reason);
        Check(env.registry.Find(exportSpec.id, 1) == nullptr, "Init 失败：导出未登记");
    }

    // —— 12. 导出池水位回滚 ——
    {
        Env env;
        ContainerSpec spec = DefaultSpec(TestModuleId());
        ExportSpec exportSpec;
        for (uint32_t index = 0; index < 16; ++index) exportSpec.id.bytes[index] = static_cast<uint8_t>(0xC0 + index);
        exportSpec.version = 1;
        exportSpec.slotOffsets = {0x30, 0x40};
        spec.exports = {exportSpec};
        env.platform.initResult = KernelError::kCorrupt;
        LoadRun run;
        const Kernel::ExportPoolMark before = env.modules.MarkPools();
        (void)LoadOne(env, spec, run);
        const Kernel::ExportPoolMark after = env.modules.MarkPools();
        Check(before.exportCount == after.exportCount && before.slotCount == after.slotCount, "Init 失败：导出池水位已回滚");
    }

    // —— 13. 依赖拓扑排序 ——
    {
        Kernel::Dependency::DependencyEntry entries[1] = {};
        entries[0].moduleId = kModDevModel;
        entries[0].minMajor = 1;
        Kernel::Dependency::ModuleNode nodes[2] = {};
        nodes[0].moduleId = kModVfs;
        nodes[0].major = 1;
        nodes[0].dependencies = entries;   // Vfs 依赖 DevModel
        nodes[0].dependencyCount = 1;
        nodes[0].preloadOrder = 2;
        nodes[1].moduleId = kModDevModel;
        nodes[1].major = 1;
        nodes[1].preloadOrder = 1;
        Kernel::Dependency::GraphInput input{};
        input.modules = nodes;
        input.moduleCount = 2;
        Kernel::Dependency::TopologyResult order{};
        const char* reason = nullptr;
        const Kernel::Diagnostics::Diagnostic diagnostic = Kernel::SortModulesForLoad(input, order, reason);
        Check(diagnostic.code == nullptr && order.count == 2 && order.index[0] == 1 && order.index[1] == 0, "拓扑排序：提供者先于消费者", reason);
    }
    {
        // 环：A 依赖 B、B 依赖 A
        Kernel::Dependency::DependencyEntry entries[2] = {};
        entries[0].moduleId = kModDevModel;
        entries[0].minMajor = 1;
        entries[1].moduleId = kModVfs;
        entries[1].minMajor = 1;
        Kernel::Dependency::ModuleNode nodes[2] = {};
        nodes[0].moduleId = kModVfs;
        nodes[0].major = 1;
        nodes[0].dependencies = &entries[0];
        nodes[0].dependencyCount = 1;
        nodes[1].moduleId = kModDevModel;
        nodes[1].major = 1;
        nodes[1].dependencies = &entries[1];
        nodes[1].dependencyCount = 1;
        Kernel::Dependency::GraphInput input{};
        input.modules = nodes;
        input.moduleCount = 2;
        Kernel::Dependency::TopologyResult order{};
        const char* reason = nullptr;
        const Kernel::Diagnostics::Diagnostic diagnostic = Kernel::SortModulesForLoad(input, order, reason);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "依赖成环 → E-KRN-59", reason);
    }
    {
        // 缺提供者
        Kernel::Dependency::DependencyEntry entries[1] = {};
        entries[0].moduleId = kModNvme;
        entries[0].minMajor = 1;
        Kernel::Dependency::ModuleNode nodes[1] = {};
        nodes[0].moduleId = kModVfs;
        nodes[0].major = 1;
        nodes[0].dependencies = entries;
        nodes[0].dependencyCount = 1;
        Kernel::Dependency::GraphInput input{};
        input.modules = nodes;
        input.moduleCount = 1;
        Kernel::Dependency::TopologyResult order{};
        const char* reason = nullptr;
        const Kernel::Diagnostics::Diagnostic diagnostic = Kernel::SortModulesForLoad(input, order, reason);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-59") == 0, "拓扑缺提供者 → E-KRN-59", reason);
    }
    {
        // 模块列表非法：标识全零
        Kernel::Dependency::ModuleNode nodes[1] = {};
        nodes[0].moduleId = ModuleId{};
        Kernel::Dependency::GraphInput input{};
        input.modules = nodes;
        input.moduleCount = 1;
        Kernel::Dependency::TopologyResult order{};
        const char* reason = nullptr;
        const Kernel::Diagnostics::Diagnostic diagnostic = Kernel::SortModulesForLoad(input, order, reason);
        Check(diagnostic.code != nullptr && strcmp(diagnostic.code, "E-KRN-57") == 0, "拓扑模块列表非法 → E-KRN-57", reason);
    }

    printf("CheckLoader: %d/%d cases passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
