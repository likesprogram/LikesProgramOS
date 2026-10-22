/* ModuleLoader.cpp
    ModuleLoader 的实现：11 步装载编排、失败回滚与模块登记

    编排只调用机制层：容器解析、注册表、依赖判定、对象归属
    与平台（页帧、直映射、入口调用）；本文件不打印、不停机，失败返回编号与原因文本，
    打印与失败传播（FATAL / WARN）由调用方决定
*/

#include <ModuleLoader.hpp>

#include <Libc.hpp>
#include <ModuleImage.hpp>
#include <ModuleRelocation.hpp>
#include <Objects.hpp>
#include <Paging.hpp>
#include <Print.hpp>
#include <Sha256.hpp>
#include <Tags.hpp>

namespace {
    // 记录失败并返回编号：打印与失败传播（FATAL / WARN）由调用方决定
    Kernel::Diagnostics::Diagnostic Fail(Kernel::LoadOutcome& outcome, Kernel::Diagnostics::Diagnostic diagnostic, const char* reason) {
        outcome.diagnostic = diagnostic;
        outcome.reason = reason;
        outcome.state = Kernel::ModuleState::Failed;
        return diagnostic;
    }

    // 打印模块初始化标记：逻辑名取容器自描述的 ModuleName（不以空字符结尾，按字节数写）
    // 标记由装载器一侧打印，玩具模块因此不需要导入 Core.Log / Print；
    // 运行期模块用独立标记，CheckAll 的「运行期模块」列按它计数，闭包列只数 module init ok:
    void ReportModuleInitialized(const Kernel::ContainerView& view, bool runtime) {
        if (view.moduleName == nullptr || view.moduleNameBytes == 0) return;
        Kernel::Tags::WriteTag(Kernel::Tags::Tag::Ok);
        Print::Write(runtime ? "runtime module init ok: " : "module init ok: ");
        for (uint32_t index = 0; index < view.moduleNameBytes; ++index) Print::PutChar(static_cast<char>(view.moduleName[index]));
        Print::Write("\r\n");
    }

    // —— 装载平台到映像平台的桥接：取页、归还与地址换算三个回调 ——
    uint64_t PlatformAcquire(void* context, uint32_t count) { return static_cast<Kernel::LoaderPlatform*>(context)->AllocatePages(count); }
    void PlatformRelease(void* context, uint64_t physical, uint32_t count) { static_cast<Kernel::LoaderPlatform*>(context)->FreePages(physical, count); }
    void* PlatformMap(void* context, uint64_t physical) { return static_cast<Kernel::LoaderPlatform*>(context)->MapPhysical(physical); }

    // 把装载平台桥接成映像平台：第 6/7 步因此跑的是同一份映像映射与重定位实现
    Kernel::ImagePlatform PlatformImage(Kernel::LoaderPlatform& platform) {
        Kernel::ImagePlatform image;
        image.acquire = &PlatformAcquire;
        image.release = &PlatformRelease;
        image.map = &PlatformMap;
        image.context = &platform;
        return image;
    }


    // 摘要核对：整文件 SHA-256 与清单条目摘要逐字节相等
    bool DigestMatches(const uint8_t* bytes, uint32_t count, const uint8_t* digest) {
        Common::Digest::Sha256 sha;
        sha.Begin();
        sha.Update(bytes, count);
        uint8_t actual[Common::Digest::kSha256Bytes] = {};
        sha.Finish(actual);
        return memcmp(actual, digest, Common::Digest::kSha256Bytes) == 0;
    }

    // 引导闭包来源核对：清单条目必须是列表里的引导闭包成员，且有长度一致的预装记录指向同一条目
    bool ClosureSourceMatches(const Kernel::HandoffFacts& handoff, const Kernel::ManifestEntry& entry, uint32_t bytesCount) {
        // 先确认清单条目是列表里的引导闭包成员：列表同时含运行期模块，路径在列表里不等于可预装
        bool inClosure = false;
        for (uint32_t index = 0; index < handoff.list.count; ++index) {
            if (handoff.list.pathBytes[index] != entry.pathBytes) continue;
            if (memcmp(handoff.list.paths[index], entry.path, entry.pathBytes) != 0) continue;
            inClosure = handoff.list.closureMember[index] != Kernel::kNotClosureMember;
            break;
        }
        if (!inClosure) return false;
        // 再确认该条目有预装记录，且记录长度与本次装载的字数一致
        for (uint32_t index = 0; index < handoff.recordCount; ++index) {
            const Kernel::HandoffRecord& record = handoff.records[index];
            if (record.artifactIndex >= handoff.manifest.entryCount) continue;
            const Kernel::ManifestEntry& recorded = handoff.manifest.entries[record.artifactIndex];
            if (recorded.pathBytes != entry.pathBytes) continue;
            if (memcmp(recorded.path, entry.path, entry.pathBytes) != 0) continue;
            if (record.bytes != bytesCount) continue;
            return true;
        }
        return false;
    }


    // 导入解析：先解析到临时数组，再逐槽写进模块自己的指针槽（槽位可以不连续）
    bool ResolveImports(const Kernel::ContainerView& view, uint8_t* baseVirt, Kernel::InterfaceRegistry& registry, const char*& reason) {
        for (uint32_t index = 0; index < view.importCount; ++index) {
            const Kernel::ContainerImport& import = view.imports[index];
            void* resolved[Kernel::kContainerMaxSlots] = {};
            ModuleImportEntry entry{};
            entry.id = import.interfaceId;
            entry.minVersion = import.minVersion;
            entry.maxVersion = import.maxVersion;
            entry.slotCount = import.slotCount;
            entry.slots = resolved;
            const Kernel::RegistryResult result = registry.ResolveImport(entry);
            if (result.error != KernelError::kOk) {
                reason = result.reason;
                return false;
            }
            for (uint32_t slot = 0; slot < import.slotCount; ++slot) {
                uint32_t offset = 0;
                if (!Kernel::ContainerImportSlot(import, slot, offset)) {
                    reason = "an import patch slot offset is out of range";
                    return false;
                }
                memcpy(baseVirt + offset, &resolved[slot], sizeof(void*));
            }
        }
        return true;
    }

    // 依赖检查：被依赖模块必须已 Initialized，且版本落在声明区间内
    bool CheckDependencies(const Kernel::ContainerView& view, const Kernel::ModuleRegistry& modules, const char*& reason) {
        for (uint32_t index = 0; index < view.dependencyCount; ++index) {
            const Kernel::ContainerDependency& dependency = view.dependencies[index];
            const Kernel::ModuleRecord* const provider = modules.Find(dependency.moduleId);
            if (provider == nullptr) {
                reason = "a dependency module isn't loaded";
                return false;
            }
            if (provider->state != Kernel::ModuleState::Initialized) {
                reason = "a dependency module hasn't finished Init";
                return false;
            }
            Kernel::Dependency::DependencyEntry entry{};
            entry.moduleId = dependency.moduleId;
            entry.minMajor = dependency.minMajor;
            entry.minMinor = dependency.minMinor;
            entry.minPatch = dependency.minPatch;
            entry.maxMajor = dependency.maxMajor;
            entry.maxMinor = dependency.maxMinor;
            entry.maxPatch = dependency.maxPatch;
            entry.flags = 0;
            if (!Kernel::Dependency::VersionMatches(entry, provider->major, provider->minor, provider->patch)) {
                reason = "a dependency version is out of the declared range";
                return false;
            }
        }
        return true;
    }

    // 提供者唯一预检：容器的每条导出不得与注册表里已有的 (InterfaceId, Version) 冲突
    bool CheckProviderUnique(const Kernel::ContainerView& view, const Kernel::InterfaceRegistry& registry, const char*& reason) {
        for (uint32_t index = 0; index < view.exportCount; ++index) {
            const Kernel::ContainerExport& entry = view.exports[index];
            if (registry.Find(entry.interfaceId, entry.version) != nullptr) {
                reason = "duplicate provider for an interface version";
                return false;
            }
        }
        return true;
    }

    // 导出物化：把容器槽位偏移翻成函数指针数组，并建出可登记的导出条目
    // 槽位表与条目都取自模块注册表的导出池，随模块生命周期常驻
    bool MaterializeExports(const Kernel::ContainerView& view, uint64_t base, Kernel::ModuleRegistry& modules, ModuleExportEntry*& entries, uint32_t& count, const char*& reason) {
        entries = nullptr;
        count = 0;
        if (view.exportCount == 0) return true;
        uint32_t totalSlots = 0;
        for (uint32_t index = 0; index < view.exportCount; ++index) totalSlots += view.exports[index].slotCount;
        const void** const slotPool = modules.AllocateSlotPointers(totalSlots);
        ModuleExportEntry* const entryPool = modules.AllocateExportEntries(view.exportCount);
        if (slotPool == nullptr || entryPool == nullptr) {
            reason = "the loader export pool is full";
            return false;
        }
        uint32_t slotCursor = 0;
        for (uint32_t index = 0; index < view.exportCount; ++index) {
            const Kernel::ContainerExport& source = view.exports[index];
            ModuleExportEntry& target = entryPool[index];
            target.id = source.interfaceId;
            target.version = source.version;
            target.slotCount = source.slotCount;
            target.slots = &slotPool[slotCursor];
            for (uint32_t slot = 0; slot < source.slotCount; ++slot) {
                uint32_t offset = 0;
                if (!Kernel::ContainerExportSlot(source, slot, offset)) {
                    reason = "an export slot offset is out of range";
                    return false;
                }
                slotPool[slotCursor + slot] = reinterpret_cast<const void*>(static_cast<uintptr_t>(base + offset));
            }
            slotCursor += source.slotCount;
        }
        entries = entryPool;
        count = view.exportCount;
        return true;
    }
}

namespace Kernel {
    void* KernelLoaderPlatform::MapPhysical(uint64_t physical) {
        if (physical == 0 || physical >= kMapLimitBytes) return nullptr;
        return PhysToVirt(physical);
    }

    uint64_t KernelLoaderPlatform::AllocatePages(uint32_t count) { return m_pages != nullptr ? m_pages->Allocate(count) : 0; }

    void KernelLoaderPlatform::FreePages(uint64_t physical, uint32_t count) {
        if (m_pages == nullptr || physical == 0 || count == 0) return;
        m_pages->Free(physical, count);
    }

    KernelError KernelLoaderPlatform::CallEntry(uint64_t base, uint32_t entryOffset, const KernelAbiContext& context) {
        // 恒等映射下物理地址即调用地址；启用高地址映射后基址换成映射后的虚拟基址，本行不变
        const auto entry = reinterpret_cast<KernelError (*)(const KernelAbiContext*)>(static_cast<uintptr_t>(base + entryOffset));
        return entry(&context);
    }

    Diagnostics::Diagnostic SortModulesForLoad(const Dependency::GraphInput& input, Dependency::TopologyResult& order, const char*& reason) {
        Dependency::Reject reject = Dependency::Reject::None;
        const char* const text = Dependency::SortByDependency(input, order, reject);
        if (text == nullptr) {
            reason = nullptr;
            return Diagnostics::Diagnostic{};
        }
        reason = text;
        if (reject == Dependency::Reject::InvalidModuleList || reject == Dependency::Reject::InvalidDependencyList) return Diagnostics::kFailOsmContainer;
        return Diagnostics::kFailModuleInit;
    }

    Diagnostics::Diagnostic LoadModule(const LoaderContext& context, const LoadRequest& request, LoadOutcome& outcome) {
        outcome = LoadOutcome{};

        // —— 前置自检：上下文与请求缺一不可 ——
        if (context.platform == nullptr || context.registry == nullptr || context.modules == nullptr)
            return Fail(outcome, Diagnostics::kFailModuleInit, "the loader context is incomplete");
        if (request.bytes == nullptr || request.bytesCount == 0 || request.manifestEntry == nullptr)
            return Fail(outcome, Diagnostics::kFailModuleDigest, "the module bytes or the manifest entry is missing");

        // —— 第 1 步：摘要与来源 ——
        const ManifestEntry& manifestEntry = *request.manifestEntry;
        if (manifestEntry.type != KernelSet::ManifestLayout::kTypeOsm)
            return Fail(outcome, Diagnostics::kFailModuleDigest, "the manifest entry isn't an .osm artifact");
        if (manifestEntry.dataBytes != request.bytesCount)
            return Fail(outcome, Diagnostics::kFailModuleDigest, "the module length doesn't match the manifest entry");
        if (manifestEntry.digest == nullptr || !DigestMatches(request.bytes, request.bytesCount, manifestEntry.digest))
            return Fail(outcome, Diagnostics::kFailModuleDigest, "the module digest doesn't match the manifest entry");
        if (request.closureModule) {
            if (request.handoff == nullptr) return Fail(outcome, Diagnostics::kFailModuleDigest, "the closure module has no handoff facts");
            if (!ClosureSourceMatches(*request.handoff, manifestEntry, request.bytesCount))
                return Fail(outcome, Diagnostics::kFailModuleDigest, "the closure module isn't in the preload list and records");
        }

        // —— 第 2..5 步：容器解析（头与边界、段表、标识与版本、四张表的结构与自洽） ——
        // 容器视图约 9.6 KiB，内核栈只有 16 KiB：装载期单线程，放静态存储，每次解析重新填
        static ContainerView view;
        view = ContainerView{};
        if (const ContainerError error = ParseModuleContainer(request.bytes, request.bytesCount, view); error != ContainerError::None) {
            const char* const text = ContainerErrorText(error);
            if (error == ContainerError::KernelAbi) return Fail(outcome, Diagnostics::kFailModuleInit, text);
            return Fail(outcome, Diagnostics::kFailOsmContainer, text);
        }

        // —— 第 4 步（续）：主版本硬门、ABI 区间、已装载集合去重 ——
        if (view.versionMajor != context.osModuleMajor)
            return Fail(outcome, Diagnostics::kFailBuildId, "the module major doesn't match the kernel set");
        if (view.abiMin > kKernelAbiVersion || (view.abiMax != kContainerAbiMaxUnlimited && kKernelAbiVersion > view.abiMax))
            return Fail(outcome, Diagnostics::kFailModuleInit, "the module kernel ABI range doesn't accept this kernel");
        if (context.modules->Contains(view.moduleId))
            return Fail(outcome, Diagnostics::kFailOsmContainer, "the module is already in the loaded set");

        // —— 第 5 步（续）：提供者唯一预检（登记在 Init 成功后，这里先挡冲突） ——
        if (const char* reason = nullptr; !CheckProviderUnique(view, *context.registry, reason))
            return Fail(outcome, Diagnostics::kFailModuleInit, reason);

        // —— 第 6 步：映射：按 ImageBytes 取连续页作 Base，逐段拷贝、清零 BSS ——
        // 页帧不足归 59（模块装载失败）；段与基址类失败归 57（容器/映像不合法）
        ImagePlatform imagePlatform = PlatformImage(*context.platform);
        ModuleImage image;
        if (const ImageError error = MapModuleImage(view, imagePlatform, image); error != ImageError::None) {
            const char* const text = ImageErrorText(error);
            if (error == ImageError::NoPages) return Fail(outcome, Diagnostics::kFailModuleInit, text);
            return Fail(outcome, Diagnostics::kFailOsmContainer, text);
        }
        const uint64_t base = image.base;
        const uint32_t pageCount = image.pageCount;
        uint8_t* const baseVirt = image.memory;   // 映像层已给出基址的可写指针（当前为直映射窗口地址）
        if (baseVirt == nullptr) {
            ReleaseModuleImage(imagePlatform, image);
            return Fail(outcome, Diagnostics::kFailModuleInit, "the module image isn't mappable");
        }

        // 登记记录与导出池检查点：从这里起的任何失败都按检查点回滚
        const ExportPoolMark poolMark = context.modules->MarkPools();
        ModuleRecord* const record = context.modules->Add(view.moduleId, view.versionMajor, view.versionMinor, view.versionPatch, base, view.imageBytes, pageCount, request.preloadOrder);
        if (record == nullptr) {
            ReleaseModuleImage(imagePlatform, image);
            return Fail(outcome, Diagnostics::kFailOsmContainer, "the module registry is full");
        }
        const auto failAfterMap = [&](Diagnostics::Diagnostic diagnostic, const char* reason) {
            context.modules->RollbackPools(poolMark);
            Objects::ReleaseByOwner(view.moduleId);
            ReleaseModuleImage(imagePlatform, image);
            record->base = 0;
            record->pageCount = 0;
            record->state = ModuleState::Failed;
            outcome.state = ModuleState::Failed;
            return Fail(outcome, diagnostic, reason);
        };

        // —— 第 7 步：重定位（失败归 57，重定位属容器判定面） ——
        if (const RelocationError error = ApplyModuleRelocations(view, image); error != RelocationError::None)
            return failAfterMap(Diagnostics::kFailOsmContainer, RelocationErrorText(error));

        // —— 第 8 步：导入解析（写进模块自己的指针槽，不执行任何模块代码） ——
        if (const char* reason = nullptr; !ResolveImports(view, baseVirt, *context.registry, reason)) return failAfterMap(Diagnostics::kFailModuleInit, reason);

        // —— 第 9 步：依赖检查（提供者必须已 Initialized） ——
        if (const char* reason = nullptr; !CheckDependencies(view, *context.modules, reason)) return failAfterMap(Diagnostics::kFailModuleInit, reason);

        // —— 第 10 步：导出物化（登记前的准备） ——
        ModuleExportEntry* exportEntries = nullptr;
        uint32_t exportCount = 0;
        if (const char* reason = nullptr; !MaterializeExports(view, base, *context.modules, exportEntries, exportCount, reason))
            return failAfterMap(Diagnostics::kFailModuleInit, reason);

        // —— 第 11 步：归属上下文 + Init ——
        KernelAbiContext abiContext{};
        abiContext.bytes = sizeof(KernelAbiContext);
        abiContext.version = kKernelAbiContextVersion;
        abiContext.kernelAbiVersion = kKernelAbiVersion;
        abiContext.moduleBase = base;
        abiContext.moduleId = view.moduleId;
        KernelError initError = KernelError::kOk;
        {
            // 归属上下文在 Init 之前建立：模块登记的资源自动记在它名下
            Objects::OwnerScope owner(view.moduleId);
            initError = context.platform->CallEntry(base, view.entryInit, abiContext);
            if (initError != KernelError::kOk && view.entryAbort != 0) (void)context.platform->CallEntry(base, view.entryAbort, abiContext);
        }
        if (initError != KernelError::kOk) return failAfterMap(Diagnostics::kFailModuleInit, KernelErrorName(initError));

        // Init 成功：发布导出接口并登记模块
        if (exportCount != 0) {
            const RegistryResult registered = context.registry->RegisterExports(exportEntries, static_cast<uint16_t>(exportCount), view.moduleId);
            if (registered.error != KernelError::kOk) {
                // 导出登记失败发生在 Init 之后：先让模块撤销自己的登记，再回收
                if (view.entryAbort != 0) {
                    Objects::OwnerScope owner(view.moduleId);
                    (void)context.platform->CallEntry(base, view.entryAbort, abiContext);
                }
                return failAfterMap(Diagnostics::kFailModuleInit, registered.reason);
            }
        }
        record->exportFirst = poolMark.exportCount;
        record->exportCount = exportCount;
        record->state = ModuleState::Initialized;
        // 初始化标记：逻辑名取容器自描述的 ModuleName（运行期模块没有预装路径也能用同一行）
        ReportModuleInitialized(view, !request.closureModule);
        outcome.base = base;
        outcome.imageBytes = view.imageBytes;
        outcome.pageCount = pageCount;
        outcome.state = ModuleState::Initialized;
        return Diagnostics::Diagnostic{};
    }
}
