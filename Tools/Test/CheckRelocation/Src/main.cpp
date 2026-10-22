/* main.cpp
    CheckRelocation：模块映像映射（ModuleImage）与重定位（ModuleRelocation）的宿主侧检查，手工执行

    检查对象是内核交付的两份实现：Packages/LikesProgramOS/Core/Src/ModuleImage.cpp 与
    ModuleRelocation.cpp；用 vpath 按相对路径直接编进本工具，不复制实现；容器由工具内嵌的
    合成夹具提供（四段各一页、首段之前留一页空洞、两条重定位），并先经真实的
    ParseModuleContainer 解析，才能拿到映射与重定位要的 ContainerView

    平台侧由替身注入：伪映像平台把物理地址换算到工具内一块对齐缓冲（映射真正写这块缓冲，
    因此能逐字节断言段内容、BSS 全零与重定位写入值）；取页与归还走伪分配器，可以注入分配
    失败、不可映射的基址、非法基址与超过 4 GiB 的跨度，并记录调用，用来核对失败路径没有
    留下页帧、成功释放只归还一次

    每个用例打印 [ OK ] 或 [ FAIL ]；用例数固定，末尾与实跑数核对；失败返回 1
*/

#include <ModuleImage.hpp>
#include <ModuleRelocation.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // 容器字节序列
    using Bytes = std::vector<std::uint8_t>;

    // 用例总数：与用例表长度核对，防止用例被误删
    constexpr unsigned kCaseCount = 43;
    // 已跑用例数
    unsigned g_ran = 0;
    // 失败用例数
    unsigned g_failed = 0;

    // 假物理基址：夹具里第一个由伪映像平台分配的页，与映射基址一致
    constexpr std::uint64_t kFakeBase = 0x1100000;
    // 可写缓冲长度：覆盖夹具映像并留出越界观察区
    constexpr std::uint32_t kArenaBytes = 0x20000;
    // 可写缓冲：按 4 KiB 对齐，假物理地址按 kFakeBase 起的偏移落到这里
    alignas(4096) std::uint8_t g_arena[kArenaBytes];

    // 夹具映像跨度：五页，首段从第二页开始，第一页是没有段的空洞
    constexpr std::uint32_t kImageBytes = 0x5000;
    // 夹具段的内存偏移
    constexpr std::uint32_t kTextMemory = 0x1000;
    constexpr std::uint32_t kRodataMemory = 0x2000;
    constexpr std::uint32_t kDataMemory = 0x3000;
    constexpr std::uint32_t kBssMemory = 0x4000;
    // 夹具段的文件字节数：BSS 不占文件字节
    constexpr std::uint32_t kTextFileBytes = 0x20;
    constexpr std::uint32_t kRodataFileBytes = 8;
    constexpr std::uint32_t kDataFileBytes = 0x40;
    // 夹具段文件数据的填充字节：与缓冲预填 0xEE 和清零后的 0 都不同
    constexpr std::uint8_t kTextFill = 0x90;
    constexpr std::uint8_t kRodataFill = 0x91;
    constexpr std::uint8_t kDataFill = 0x92;
    // 缓冲预填字节：映射前写满，用于证明清零与文件拷贝确实发生
    constexpr std::uint8_t kArenaFill = 0xEE;

    // 容器文件头字段偏移：与 ModuleContainer.cpp 的头部字段一致
    constexpr std::size_t kFieldTotalBytes = 0x10;
    constexpr std::size_t kFieldImageBytes = 0x14;
    constexpr std::size_t kFieldEntryInit = 0x18;
    constexpr std::size_t kFieldSectionTableOffset = 0x24;
    constexpr std::size_t kFieldSectionCount = 0x26;
    constexpr std::size_t kFieldSectionEntryBytes = 0x28;
    constexpr std::size_t kFieldExportCount = 0x2A;
    constexpr std::size_t kFieldExportTableOffset = 0x2C;
    constexpr std::size_t kFieldImportCount = 0x30;
    constexpr std::size_t kFieldImportEntryBytes = 0x32;
    constexpr std::size_t kFieldImportTableOffset = 0x34;
    constexpr std::size_t kFieldDependencyCount = 0x38;
    constexpr std::size_t kFieldDependencyEntryBytes = 0x3A;
    constexpr std::size_t kFieldDependencyTableOffset = 0x3C;
    constexpr std::size_t kFieldRelocationCount = 0x40;
    constexpr std::size_t kFieldRelocationTableOffset = 0x44;
    constexpr std::size_t kFieldStringTableOffset = 0x48;
    constexpr std::size_t kFieldStringTableBytes = 0x4C;
    // 段表条目内字段偏移
    constexpr std::size_t kSectionNameOffset = 0x00;
    constexpr std::size_t kSectionType = 0x04;
    constexpr std::size_t kSectionFlags = 0x08;
    constexpr std::size_t kSectionFileOffset = 0x0C;
    constexpr std::size_t kSectionFileBytes = 0x10;
    constexpr std::size_t kSectionMemoryOffset = 0x14;
    constexpr std::size_t kSectionMemoryBytes = 0x18;
    constexpr std::size_t kSectionAlign = 0x1C;
    // 重定位条目内字段偏移
    constexpr std::size_t kRelocationType = 0x00;
    constexpr std::size_t kRelocationTarget = 0x04;
    constexpr std::size_t kRelocationAddend = 0x08;

    // 中止当前用例
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

    // 条件不成立即中止当前用例
    void Require(bool value, const std::string& message) {
        if (!value) Fail(message);
    }

    // 小端写入 16 位：越界即中止当前用例
    void Put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
        if (offset > bytes.size() || bytes.size() - offset < 2) Fail("write beyond the container copy");
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }

    // 小端写入 32 位：越界即中止当前用例
    void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
        if (offset > bytes.size() || bytes.size() - offset < 4) Fail("write beyond the container copy");
        for (unsigned index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
    }

    // 小端写入 64 位：越界即中止当前用例
    void Put64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
        if (offset > bytes.size() || bytes.size() - offset < 8) Fail("write beyond the container copy");
        for (unsigned index = 0; index < 8; ++index) bytes[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
    }

    // 把值向上取整到 align 的整数倍
    std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) { return (value + align - 1) / align * align; }

    // 合成夹具：容器字节、重定位表与段文件偏移；视图指向 fixture.bytes，夹具必须在使用期间有效
    struct Fixture {
        Bytes bytes;                          // 容器字节
        std::uint32_t relocationOffset = 0;   // 重定位表文件偏移
        std::uint32_t textFileOffset = 0;     // TEXT 段文件偏移
        std::uint32_t rodataFileOffset = 0;   // RODATA 段文件偏移
        std::uint32_t dataFileOffset = 0;     // DATA 段文件偏移
    };

    // 改写一条重定位条目：夹具必须已建好，索引在条目数内
    void PatchRelocation(Fixture& fixture, unsigned index, std::uint8_t type, std::uint32_t target, std::uint64_t addend) {
        const std::size_t entry = static_cast<std::size_t>(fixture.relocationOffset) + index * Kernel::kContainerRelocationEntryBytes;
        fixture.bytes[entry + kRelocationType] = type;
        Put32(fixture.bytes, entry + kRelocationTarget, target);
        Put64(fixture.bytes, entry + kRelocationAddend, addend);
    }

    // 构造规范的合成容器：四段各一页、首段之前留一页空洞、一条 RELATIVE64 与一条 RELATIVE32
    Fixture BuildProbe() {
        // 字符串表：偏移 0 空串，随后四个段名与模块名
        std::string strings(1, '\0');
        auto appendName = [&strings](const std::string& name) -> std::uint32_t {
            const auto offset = static_cast<std::uint32_t>(strings.size());
            strings += name;
            strings.push_back('\0');
            return offset;
        };
        const std::uint32_t textName = appendName(".text");
        const std::uint32_t rodataName = appendName(".rodata");
        const std::uint32_t dataName = appendName(".data");
        const std::uint32_t bssName = appendName(".bss");
        const std::uint32_t moduleName = appendName("ProbeModule");
        const std::uint16_t moduleNameBytes = 11;

        // 布局：文件头、段表、三张空表、重定位表、字符串表、段文件数据
        std::uint64_t cursor = Kernel::kContainerHeaderBytes;
        const std::uint32_t sectionTableOffset = static_cast<std::uint32_t>(cursor);
        cursor = sectionTableOffset + 4 * Kernel::kContainerSectionEntryBytes;
        const std::uint32_t emptyTableOffset = static_cast<std::uint32_t>(cursor);
        const std::uint32_t relocationOffset = emptyTableOffset;   // 0x100 已按 8 字节对齐
        cursor = relocationOffset + 2 * Kernel::kContainerRelocationEntryBytes;
        const std::uint32_t stringTableOffset = static_cast<std::uint32_t>(cursor);
        cursor = stringTableOffset + strings.size();
        const std::uint32_t textFileOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = textFileOffset + kTextFileBytes;
        const std::uint32_t rodataFileOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = rodataFileOffset + kRodataFileBytes;
        const std::uint32_t dataFileOffset = static_cast<std::uint32_t>(AlignUp(cursor, 4));
        cursor = dataFileOffset + kDataFileBytes;
        const auto totalBytes = static_cast<std::uint32_t>(cursor);

        Fixture fixture;
        fixture.bytes.assign(totalBytes, 0);
        fixture.relocationOffset = relocationOffset;
        fixture.textFileOffset = textFileOffset;
        fixture.rodataFileOffset = rodataFileOffset;
        fixture.dataFileOffset = dataFileOffset;
        Bytes& bytes = fixture.bytes;
        // 文件头
        static constexpr std::uint8_t kMagic[8] = { 'L', 'P', 'O', 'S', 'M', 'O', 'D', 0 };
        std::memcpy(bytes.data(), kMagic, sizeof(kMagic));
        Put16(bytes, 0x08, 1);
        Put16(bytes, 0x0A, 0x80);
        Put32(bytes, 0x0C, 0);
        Put32(bytes, kFieldTotalBytes, totalBytes);
        Put32(bytes, kFieldImageBytes, kImageBytes);
        Put32(bytes, kFieldEntryInit, kTextMemory + 0x10);
        Put32(bytes, 0x1C, 0);
        Put32(bytes, 0x20, 0);
        Put16(bytes, kFieldSectionTableOffset, static_cast<std::uint16_t>(sectionTableOffset));
        Put16(bytes, kFieldSectionCount, 4);
        Put16(bytes, kFieldSectionEntryBytes, 32);
        Put16(bytes, kFieldExportCount, 0);
        Put32(bytes, kFieldExportTableOffset, emptyTableOffset);
        Put16(bytes, kFieldImportCount, 0);
        Put16(bytes, kFieldImportEntryBytes, 28);
        Put32(bytes, kFieldImportTableOffset, emptyTableOffset);
        Put16(bytes, kFieldDependencyCount, 0);
        Put16(bytes, kFieldDependencyEntryBytes, 32);
        Put32(bytes, kFieldDependencyTableOffset, emptyTableOffset);
        Put32(bytes, kFieldRelocationCount, 2);
        Put32(bytes, kFieldRelocationTableOffset, relocationOffset);
        Put32(bytes, kFieldStringTableOffset, stringTableOffset);
        Put32(bytes, kFieldStringTableBytes, static_cast<std::uint32_t>(strings.size()));
        // 模块标识、版本、ABI 与模块名
        for (std::uint32_t index = 0; index < 16; ++index) bytes[0x50 + index] = static_cast<std::uint8_t>(0x11 + index);
        Put32(bytes, 0x60, moduleName);
        Put16(bytes, 0x64, moduleNameBytes);
        Put16(bytes, 0x66, 1);
        Put16(bytes, 0x68, 2);
        Put16(bytes, 0x6A, 3);
        Put16(bytes, 0x6C, 1);
        Put16(bytes, 0x6E, 0xFFFF);
        // 段表：TEXT / RODATA / DATA / BSS 各一页，首段之前留一页空洞
        const std::uint32_t nameOffsets[4] = { textName, rodataName, dataName, bssName };
        const std::uint32_t types[4] = { 1, 2, 3, 4 };
        const std::uint32_t flags[4] = { 5, 1, 3, 3 };
        const std::uint32_t fileOffsets[4] = { textFileOffset, rodataFileOffset, dataFileOffset, 0 };
        const std::uint32_t fileBytes[4] = { kTextFileBytes, kRodataFileBytes, kDataFileBytes, 0 };
        const std::uint32_t memoryOffsets[4] = { kTextMemory, kRodataMemory, kDataMemory, kBssMemory };
        for (unsigned index = 0; index < 4; ++index) {
            const std::size_t entry = static_cast<std::size_t>(sectionTableOffset) + index * Kernel::kContainerSectionEntryBytes;
            Put32(bytes, entry + kSectionNameOffset, nameOffsets[index]);
            Put32(bytes, entry + kSectionType, types[index]);
            Put32(bytes, entry + kSectionFlags, flags[index]);
            Put32(bytes, entry + kSectionFileOffset, fileOffsets[index]);
            Put32(bytes, entry + kSectionFileBytes, fileBytes[index]);
            Put32(bytes, entry + kSectionMemoryOffset, memoryOffsets[index]);
            Put32(bytes, entry + kSectionMemoryBytes, Kernel::kContainerPageBytes);
            Put32(bytes, entry + kSectionAlign, Kernel::kContainerPageBytes);
        }
        // 重定位表：一条 RELATIVE64 与一条 RELATIVE32，目标都落在可写 DATA 段内
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64), kDataMemory, 0x10);
        PatchRelocation(fixture, 1, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative32), kDataMemory + 8, 0x20);
        // 字符串表与段文件数据
        std::memcpy(bytes.data() + stringTableOffset, strings.data(), strings.size());
        std::memset(bytes.data() + textFileOffset, kTextFill, kTextFileBytes);
        std::memset(bytes.data() + rodataFileOffset, kRodataFill, kRodataFileBytes);
        std::memset(bytes.data() + dataFileOffset, kDataFill, kDataFileBytes);
        return fixture;
    }

    // 解析夹具容器：失败即中止当前用例
    Kernel::ContainerView ParseOk(Bytes& bytes) {
        Kernel::ContainerView view{};
        const Kernel::ContainerError error = Kernel::ParseModuleContainer(bytes.data(), static_cast<std::uint32_t>(bytes.size()), view);
        if (error != Kernel::ContainerError::None) {
            Fail(std::string("the fixture container must parse, got '") + Kernel::ContainerErrorText(error) + "'");
        }
        return view;
    }

    // 伪映像平台：记录取页 / 归还调用与假物理地址，物理地址换算指向工具内的对齐缓冲
    struct FakePages {
        std::uint64_t next = kFakeBase;     // 下一次 Acquire 返回的假物理基址，0 表示分配失败
        bool mapFails = false;              // 为真时物理地址换算返回空
        std::uint32_t acquireCount = 0;     // Acquire 调用次数
        std::uint32_t lastPages = 0;        // 最近一次 Acquire 的页数
        std::uint32_t releaseCount = 0;     // Release 调用次数
        std::uint64_t releasePhysical = 0;  // 最近一次 Release 的起址
        std::uint32_t releasePages = 0;     // 最近一次 Release 的页数
    };

    // 取页回调：返回 next，分配失败由用例把 next 设为 0
    std::uint64_t FakeAcquire(void* context, std::uint32_t count) {
        FakePages* pages = static_cast<FakePages*>(context);
        ++pages->acquireCount;
        pages->lastPages = count;
        return pages->next;
    }

    // 归还回调：记录起址与页数，不碰缓冲
    void FakeRelease(void* context, std::uint64_t physical, std::uint32_t count) {
        FakePages* pages = static_cast<FakePages*>(context);
        ++pages->releaseCount;
        pages->releasePhysical = physical;
        pages->releasePages = count;
    }

    // 地址换算回调：把假物理地址换算成工具内缓冲的宿主指针；mapFails 时返回空
    void* FakeMap(void* context, std::uint64_t physical) {
        FakePages* pages = static_cast<FakePages*>(context);
        if (pages->mapFails) return nullptr;
        if (physical < kFakeBase || physical - kFakeBase >= kArenaBytes) {
            Fail("the fake address is outside the arena");
        }
        return g_arena + (physical - kFakeBase);
    }

    // 把伪平台包成映射接口
    Kernel::ImagePlatform PlatformOf(FakePages& pages) {
        Kernel::ImagePlatform platform;
        platform.acquire = &FakeAcquire;
        platform.release = &FakeRelease;
        platform.map = &FakeMap;
        platform.context = &pages;
        return platform;
    }

    // 假物理地址处的宿主指针：超出缓冲即中止当前用例
    std::uint8_t* ArenaAt(std::uint64_t physical) {
        if (physical < kFakeBase || physical - kFakeBase >= kArenaBytes) Fail("the fake address is outside the arena");
        return g_arena + (physical - kFakeBase);
    }

    // 预填缓冲：映射前的旧内容必须被整段清零覆盖，BSS 与未落盘尾部才不是"本来就零"
    void FillArena(std::uint8_t value) { std::memset(g_arena, value, kArenaBytes); }

    // 假物理地址处的一段字节是否全为期望值
    bool ArenaFilled(std::uint64_t physical, std::uint8_t value, std::uint32_t count) {
        const std::uint8_t* pointer = ArenaAt(physical);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (pointer[index] != value) return false;
        }
        return true;
    }

    // 读假物理地址处的 32 位小端值
    std::uint32_t ArenaRead32(std::uint64_t physical) {
        const std::uint8_t* pointer = ArenaAt(physical);
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(pointer[index]) << (8 * index);
        return value;
    }

    // 读假物理地址处的 64 位小端值
    std::uint64_t ArenaRead64(std::uint64_t physical) {
        const std::uint8_t* pointer = ArenaAt(physical);
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(pointer[index]) << (8 * index);
        return value;
    }

    // 期待映射成功并返回映像记录
    Kernel::ModuleImage ExpectMap(const Kernel::ContainerView& view, Kernel::ImagePlatform& source) {
        Kernel::ModuleImage image{};
        const Kernel::ImageError error = Kernel::MapModuleImage(view, source, image);
        if (error != Kernel::ImageError::None) {
            Fail(std::string("expected success, got '") + Kernel::ImageErrorText(error) + "'");
        }
        return image;
    }

    // 期待映射失败且分类匹配，同时核对失败路径把映像记录清空
    void ExpectMapError(const Kernel::ContainerView& view, Kernel::ImagePlatform& source, Kernel::ImageError expected) {
        Kernel::ModuleImage image{};
        image.base = 0xDEAD;   // 脏值：失败时必须被清空
        image.imageBytes = 0xDEAD;
        image.pageCount = 0xDEAD;
        const Kernel::ImageError actual = Kernel::MapModuleImage(view, source, image);
        if (actual != expected) {
            Fail(std::string("expected '") + Kernel::ImageErrorText(expected) + "', got '" + Kernel::ImageErrorText(actual) + "'");
        }
        Require(image.base == 0 && image.memory == nullptr && image.imageBytes == 0 && image.pageCount == 0,
            "a failed map must leave the image record empty");
    }

    // 期待重定位成功
    void ExpectRelocations(const Kernel::ContainerView& view, const Kernel::ModuleImage& image) {
        const Kernel::RelocationError error = Kernel::ApplyModuleRelocations(view, image);
        if (error != Kernel::RelocationError::None) {
            Fail(std::string("expected success, got '") + Kernel::RelocationErrorText(error) + "'");
        }
    }

    // 期待重定位失败且分类匹配
    void ExpectRelocationError(const Kernel::ContainerView& view, const Kernel::ModuleImage& image,
        Kernel::RelocationError expected) {
        const Kernel::RelocationError actual = Kernel::ApplyModuleRelocations(view, image);
        if (actual != expected) {
            Fail(std::string("expected '") + Kernel::RelocationErrorText(expected) + "', got '" + Kernel::RelocationErrorText(actual) + "'");
        }
    }

    // 一条用例：名字与用例体
    struct Case {
        std::string name;              // 用例名
        std::function<void()> body;    // 用例体
    };

    // —— 映像映射：正例 ——
    // 正例：合成夹具映射成功，基址与跨度按容器字段给出，页帧一次取齐
    void CaseMapPositive() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        Require(image.base == kFakeBase, "the module base must be the image platform base");
        Require(image.base % Kernel::kPageBytes == 0, "the module base must be 4 KiB aligned");
        Require(image.imageBytes == kImageBytes, "the image span must match the container ImageBytes");
        Require(image.imageBytes % Kernel::kPageBytes == 0, "the image span must be a 4 KiB multiple");
        Require(image.pageCount == kImageBytes / Kernel::kPageBytes, "the image page count must match the span");
        Require(image.memory == ArenaAt(kFakeBase), "the image memory pointer must be the mapped base");
        Require(pages.acquireCount == 1, "the image must be taken in one contiguous run");
        Require(pages.lastPages == image.pageCount, "the image platform must be asked for the whole span");
        Require(pages.releaseCount == 0, "a successful map must not release pages");
    }

    // 段内容逐字节：TEXT / RODATA / DATA 的文件字节按容器偏移原样落位
    void CaseMapCopiesSectionBytes() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        Require(ArenaFilled(image.base + kTextMemory, kTextFill, kTextFileBytes), "TEXT file bytes are wrong");
        Require(ArenaFilled(image.base + kRodataMemory, kRodataFill, kRodataFileBytes), "RODATA file bytes are wrong");
        Require(ArenaFilled(image.base + kDataMemory, kDataFill, kDataFileBytes), "DATA file bytes are wrong");
        Require(ArenaFilled(image.base + kTextMemory + kTextFileBytes, 0, Kernel::kContainerPageBytes - kTextFileBytes),
            "the unbacked TEXT tail must be zero");
    }

    // BSS 与空洞全零：整段清零覆盖 BSS、各段未落盘尾部与首段之前的空洞
    void CaseMapZeroesBssAndGaps() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        Require(ArenaFilled(image.base, 0, kTextMemory), "the gap page before the first section must be zero");
        Require(ArenaFilled(image.base + kBssMemory, 0, Kernel::kContainerPageBytes), "BSS must be all zero");
        Require(ArenaFilled(image.base + kDataMemory + kDataFileBytes, 0, Kernel::kContainerPageBytes - kDataFileBytes),
            "the unbacked DATA tail must be zero");
        Require(ArenaFilled(image.base + kRodataMemory + kRodataFileBytes, 0, Kernel::kContainerPageBytes - kRodataFileBytes),
            "the unbacked RODATA tail must be zero");
    }

    // 连续页：映像覆盖 ImageBytes 连续的页，最后一片可写，映射不碰映像之外
    void CaseMapContiguousSpan() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        Require(ArenaFilled(image.base + kImageBytes - 1, 0, 1), "the last byte of the image span must be mapped");
        Require(ArenaFilled(image.base + kImageBytes, kArenaFill, 1), "the map must not write past the image span");
        Require(pages.lastPages == image.pageCount, "the whole span must be one contiguous run");
    }

    // —— 映像映射：输入校验与失败分类 ——
    // 视图没有容器字节：不取页帧
    void CaseMapNullView() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.bytes = nullptr;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::NullPointer);
        Require(pages.acquireCount == 0, "an invalid view must not take pages");
    }

    // 映像平台缺少回调：三个回调任一为空都按空指针拒绝
    void CaseMapNullSource() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        source.acquire = nullptr;
        ExpectMapError(view, source, Kernel::ImageError::NullPointer);
        source = PlatformOf(pages);
        source.map = nullptr;
        ExpectMapError(view, source, Kernel::ImageError::NullPointer);
        Require(pages.acquireCount == 0, "a platform without callbacks must not take pages");
    }

    // 映像跨度为零：拒绝
    void CaseMapZeroImageBytes() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.imageBytes = 0;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::ImageBytes);
        Require(pages.acquireCount == 0, "an invalid span must not take pages");
    }

    // 映像跨度不是 4 KiB 倍数：拒绝
    void CaseMapUnalignedImageBytes() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.imageBytes = kImageBytes + 1;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::ImageBytes);
    }

    // 映像跨度超过容器总长上限：拒绝
    void CaseMapOversizedImageBytes() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.imageBytes = Kernel::kContainerMaxTotalBytes + Kernel::kContainerPageBytes;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::ImageBytes);
    }

    // 页帧不足：报告 NoPages，不归还也不留记录
    void CaseMapNoPages() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        pages.next = 0;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::NoPages);
        Require(pages.acquireCount == 1, "the image platform must be asked once");
        Require(pages.releaseCount == 0, "a failed acquire has nothing to release");
    }

    // 基址未按 4 KiB 对齐：拒绝并归还刚取到的页
    void CaseMapUnalignedBase() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        pages.next = kFakeBase + 4;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::BaseRange);
        Require(pages.releaseCount == 1 && pages.releasePhysical == pages.next, "the unaligned base pages must be released");
        Require(pages.releasePages == kImageBytes / Kernel::kPageBytes, "the release must cover the whole span");
    }

    // 基址加跨度越过低 4 GiB：拒绝并归还
    void CaseMapBasePastLimit() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        pages.next = 0xFFFFF000;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::BaseRange);
        Require(pages.releaseCount == 1 && pages.releasePhysical == pages.next, "the pages of a bad base must be released");
    }

    // 物理地址不可映射：拒绝并归还刚取到的页
    void CaseMapUnmapped() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        pages.mapFails = true;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::Unmapped);
        Require(pages.releaseCount == 1 && pages.releasePhysical == kFakeBase, "the unmapped pages must be released");
        Require(pages.releasePages == kImageBytes / Kernel::kPageBytes, "the release must cover the whole span");
    }

    // 段类型未知：拒绝，不取页帧
    void CaseMapUnknownSectionType() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[0].type = 9;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::Section);
        Require(pages.acquireCount == 0, "an invalid section table must not take pages");
    }

    // 段表为空：拒绝
    void CaseMapEmptySectionTable() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sectionCount = 0;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::Section);
    }

    // 段数超过上限：拒绝
    void CaseMapTooManySections() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sectionCount = Kernel::kContainerMaxSections + 1;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::Section);
    }

    // BSS 带文件字节：拒绝
    void CaseMapBssWithFileBytes() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[3].fileBytes = 0x10;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::Section);
    }

    // 非 BSS 段没有文件字节：拒绝
    void CaseMapMissingFileBytes() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[1].fileBytes = 0;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::Section);
    }

    // 段文件区间越过容器字节：拒绝
    void CaseMapFilePastContainer() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[0].fileBytes = 0x1000;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::Section);
    }

    // 段内存跨度为零：拒绝
    void CaseMapZeroSectionSpan() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[1].memoryBytes = 0;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::SectionSpan);
    }

    // 段内存偏移未按 4 KiB 对齐：拒绝
    void CaseMapUnalignedSectionSpan() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[1].memoryOffset = kRodataMemory + 4;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::SectionSpan);
    }

    // 段内存区间越过 ImageBytes：拒绝
    void CaseMapSectionPastImage() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[2].memoryOffset = kImageBytes;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::SectionSpan);
    }

    // 两个段的内存区间重叠：拒绝
    void CaseMapOverlappingSpans() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        view.sections[1].memoryOffset = kTextMemory;
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        ExpectMapError(view, source, Kernel::ImageError::SectionSpan);
    }

    // 归还映像：页帧按分配时的起址与页数归还，记录清空，重复调用是空操作
    void CaseMapReleaseImage() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        Kernel::ModuleImage image = ExpectMap(view, source);
        const std::uint64_t base = image.base;
        const std::uint32_t pageCount = image.pageCount;
        Kernel::ReleaseModuleImage(source, image);
        Require(pages.releaseCount == 1, "the image pages must be released once");
        Require(pages.releasePhysical == base && pages.releasePages == pageCount, "the release must use the mapped base and page count");
        Require(image.base == 0 && image.imageBytes == 0 && image.pageCount == 0, "a release must clear the image record");
        Kernel::ReleaseModuleImage(source, image);
        Require(pages.releaseCount == 1, "a second release of an empty image must do nothing");
    }

    // 归还空映像：不调用映像平台
    void CaseMapReleaseEmpty() {
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        Kernel::ModuleImage image{};
        Kernel::ReleaseModuleImage(source, image);
        Require(pages.releaseCount == 0 && pages.acquireCount == 0, "an empty image must not touch the image platform");
    }

    // —— 重定位：正例 ——
    // 正例：RELATIVE64 与 RELATIVE32 都写入 Base + Addend，32 位只覆盖 4 字节
    void CaseRelocationPositive() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocations(view, image);
        Require(ArenaRead64(image.base + kDataMemory) == image.base + 0x10, "RELATIVE64 must write Base + Addend");
        Require(ArenaRead32(image.base + kDataMemory + 8) == static_cast<std::uint32_t>(image.base + 0x20),
            "RELATIVE32 must write the low 32 bits of Base + Addend");
        Require(ArenaFilled(image.base + kDataMemory + 0x0C, kDataFill, 4), "RELATIVE32 must write only four bytes");
    }

    // RELATIVE32 边界：Base + Addend 正好取到 0xFFFFFFFF 时接受
    void CaseRelocation32Boundary() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative32),
            kDataMemory, 0xFFFFFFFFull - kFakeBase);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocations(view, image);
        Require(ArenaRead32(image.base + kDataMemory) == 0xFFFFFFFFull, "a result of 0xFFFFFFFF must be accepted");
    }

    // 目标落在 BSS 可写段内：接受，写入 Base + Addend
    void CaseRelocationBssTarget() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64),
            kBssMemory, 0x30);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocations(view, image);
        Require(ArenaRead64(image.base + kBssMemory) == image.base + 0x30, "a BSS target must be writable");
    }

    // —— 重定位：负例 ——
    // Base + Addend 溢出 64 位：拒绝，且该条目不写入
    void CaseRelocationAddressOverflow() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64),
            kDataMemory, ~static_cast<std::uint64_t>(0));
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocationError(view, image, Kernel::RelocationError::AddressOverflow);
        Require(ArenaFilled(image.base + kDataMemory, kDataFill, 8), "a rejected relocation must not write");
    }

    // RELATIVE32 结果超出 32 位：拒绝
    void CaseRelocationResult32Overflow() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative32),
            kDataMemory, 0x100000000ull - kFakeBase);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocationError(view, image, Kernel::RelocationError::Result32);
        Require(ArenaFilled(image.base + kDataMemory, kDataFill, 4), "a rejected relocation must not write");
    }

    // 目标在 TEXT 段：文本段不允许重定位
    void CaseRelocationTargetText() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64),
            kTextMemory, 0x10);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocationError(view, image, Kernel::RelocationError::TargetSection);
    }

    // 目标在 RODATA 段：只读段不允许重定位
    void CaseRelocationTargetRodata() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64),
            kRodataMemory, 0x10);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocationError(view, image, Kernel::RelocationError::TargetSection);
    }

    // 目标越过映像跨度：拒绝
    void CaseRelocationTargetPastImage() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64),
            kImageBytes, 0x10);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocationError(view, image, Kernel::RelocationError::TargetRange);
    }

    // 目标越过可写段尾：段表被改成不足一页时，越界写入必须被拒绝
    void CaseRelocationTargetPastSection() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64),
            kDataMemory + 0xFF8, 0x10);
        view.sections[2].memoryBytes = 0x800;
        ExpectRelocationError(view, image, Kernel::RelocationError::TargetSection);
    }

    // RELATIVE64 目标未按 8 字节对齐：拒绝
    void CaseRelocationTargetAlignment() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, static_cast<std::uint8_t>(Kernel::ContainerRelocationType::Relative64),
            kDataMemory + 4, 0x10);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocationError(view, image, Kernel::RelocationError::TargetAlignment);
    }

    // 未知重定位类型：拒绝，不猜测
    void CaseRelocationUnknownType() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        PatchRelocation(fixture, 0, 3, kDataMemory, 0x10);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        ExpectRelocationError(view, image, Kernel::RelocationError::UnknownType);
    }

    // 映像记录与容器视图的跨度不一致：拒绝
    void CaseRelocationImageSpanMismatch() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        const Kernel::ModuleImage mapped = ExpectMap(view, source);
        Kernel::ModuleImage image = mapped;
        image.imageBytes = kImageBytes - Kernel::kContainerPageBytes;
        ExpectRelocationError(view, image, Kernel::RelocationError::ImageMismatch);
    }

    // 映像记录的基址为零或未对齐：拒绝
    void CaseRelocationImageBaseMismatch() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        const Kernel::ModuleImage mapped = ExpectMap(view, source);
        Kernel::ModuleImage image = mapped;
        image.base = 0;
        ExpectRelocationError(view, image, Kernel::RelocationError::ImageMismatch);
        image.base = mapped.base + 8;
        ExpectRelocationError(view, image, Kernel::RelocationError::ImageMismatch);
        image = mapped;
        image.memory = nullptr;
        ExpectRelocationError(view, image, Kernel::RelocationError::ImageMismatch);
    }

    // 重定位表区越过容器字节：拒绝
    void CaseRelocationTablePastContainer() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        view.relocationCount = 100;
        ExpectRelocationError(view, image, Kernel::RelocationError::Table);
    }

    // 重定位条目数超过容器上限：拒绝
    void CaseRelocationCountTooLarge() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        view.relocationCount = Kernel::kContainerMaxRelocations + 1;
        ExpectRelocationError(view, image, Kernel::RelocationError::Table);
    }

    // 视图没有容器字节：按空指针拒绝
    void CaseRelocationNullView() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        view.bytes = nullptr;
        ExpectRelocationError(view, image, Kernel::RelocationError::NullPointer);
    }

    // 没有重定位条目：成功且不写任何字节
    void CaseRelocationNoEntries() {
        Fixture fixture = BuildProbe();
        Kernel::ContainerView view = ParseOk(fixture.bytes);
        FakePages pages;
        Kernel::ImagePlatform source = PlatformOf(pages);
        FillArena(kArenaFill);
        const Kernel::ModuleImage image = ExpectMap(view, source);
        view.relocationCount = 0;
        ExpectRelocations(view, image);
        Require(ArenaFilled(image.base + kDataMemory, kDataFill, kDataFileBytes), "a view without relocations must not write");
    }

    // 合成夹具先经真实解析器：四段、ImageBytes、入口与两条重定位
    void CaseFixtureParses() {
        Fixture fixture = BuildProbe();
        const Kernel::ContainerView view = ParseOk(fixture.bytes);
        Require(view.sectionCount == 4, "the fixture must have four sections");
        Require(view.imageBytes == kImageBytes, "the fixture ImageBytes is wrong");
        Require(view.relocationCount == 2, "the fixture must have two relocations");
        Require(view.entryInit == kTextMemory + 0x10, "the fixture entry point is wrong");
        Require(view.sections[2].type == 3 && view.sections[3].type == 4, "the fixture sections are wrong");
    }

    // 组装全部用例
    std::vector<Case> BuildCases() {
        std::vector<Case> cases;
        // ---- 映像映射正例 ----
        cases.push_back({ "映射：合成夹具成功，基址与跨度正确", CaseMapPositive });
        cases.push_back({ "映射：段文件字节逐字节落位", CaseMapCopiesSectionBytes });
        cases.push_back({ "映射：BSS、未落盘尾部与段间空洞全零", CaseMapZeroesBssAndGaps });
        cases.push_back({ "映射：连续页覆盖映像且不越界写", CaseMapContiguousSpan });
        // ---- 映像映射负例 ----
        cases.push_back({ "映射：视图没有容器字节", CaseMapNullView });
        cases.push_back({ "映射：映像平台缺少回调", CaseMapNullSource });
        cases.push_back({ "映射：ImageBytes 为零", CaseMapZeroImageBytes });
        cases.push_back({ "映射：ImageBytes 非 4 KiB 倍数", CaseMapUnalignedImageBytes });
        cases.push_back({ "映射：ImageBytes 超过上限", CaseMapOversizedImageBytes });
        cases.push_back({ "映射：页帧不足", CaseMapNoPages });
        cases.push_back({ "映射：基址未按 4 KiB 对齐并归还页", CaseMapUnalignedBase });
        cases.push_back({ "映射：基址加跨度越过 4 GiB 并归还页", CaseMapBasePastLimit });
        cases.push_back({ "映射：物理地址不可映射并归还页", CaseMapUnmapped });
        cases.push_back({ "映射：段类型未知", CaseMapUnknownSectionType });
        cases.push_back({ "映射：段表为空", CaseMapEmptySectionTable });
        cases.push_back({ "映射：段数超过上限", CaseMapTooManySections });
        cases.push_back({ "映射：BSS 带文件字节", CaseMapBssWithFileBytes });
        cases.push_back({ "映射：非 BSS 段没有文件字节", CaseMapMissingFileBytes });
        cases.push_back({ "映射：段文件区间越过容器", CaseMapFilePastContainer });
        cases.push_back({ "映射：段内存跨度为零", CaseMapZeroSectionSpan });
        cases.push_back({ "映射：段内存偏移未按 4 KiB 对齐", CaseMapUnalignedSectionSpan });
        cases.push_back({ "映射：段内存区间越过 ImageBytes", CaseMapSectionPastImage });
        cases.push_back({ "映射：段内存区间重叠", CaseMapOverlappingSpans });
        cases.push_back({ "映射：归还页帧并清空记录", CaseMapReleaseImage });
        cases.push_back({ "映射：归还空映像是空操作", CaseMapReleaseEmpty });
        // ---- 重定位正例 ----
        cases.push_back({ "重定位：RELATIVE64 与 RELATIVE32 正例", CaseRelocationPositive });
        cases.push_back({ "重定位：RELATIVE32 结果取到 0xFFFFFFFF", CaseRelocation32Boundary });
        cases.push_back({ "重定位：BSS 目标可写", CaseRelocationBssTarget });
        // ---- 重定位负例 ----
        cases.push_back({ "重定位：Base + Addend 溢出 64 位", CaseRelocationAddressOverflow });
        cases.push_back({ "重定位：RELATIVE32 结果超出 32 位", CaseRelocationResult32Overflow });
        cases.push_back({ "重定位：目标在 TEXT 段", CaseRelocationTargetText });
        cases.push_back({ "重定位：目标在 RODATA 段", CaseRelocationTargetRodata });
        cases.push_back({ "重定位：目标越过映像跨度", CaseRelocationTargetPastImage });
        cases.push_back({ "重定位：目标越过可写段尾", CaseRelocationTargetPastSection });
        cases.push_back({ "重定位：RELATIVE64 目标未按 8 字节对齐", CaseRelocationTargetAlignment });
        cases.push_back({ "重定位：未知类型", CaseRelocationUnknownType });
        cases.push_back({ "重定位：映像跨度与视图不一致", CaseRelocationImageSpanMismatch });
        cases.push_back({ "重定位：映像基址为零或未对齐", CaseRelocationImageBaseMismatch });
        cases.push_back({ "重定位：表区越过容器字节", CaseRelocationTablePastContainer });
        cases.push_back({ "重定位：条目数超过上限", CaseRelocationCountTooLarge });
        cases.push_back({ "重定位：视图没有容器字节", CaseRelocationNullView });
        cases.push_back({ "重定位：没有条目时不写字节", CaseRelocationNoEntries });
        // ---- 夹具自检 ----
        cases.push_back({ "夹具：合成容器通过真实解析器", CaseFixtureParses });
        return cases;
    }
}

// 入口：跑全部用例并汇总；失败返回 1
int main() {
    const std::vector<Case> cases = BuildCases();
    if (cases.size() != kCaseCount) {
        std::cerr << "CheckRelocation: case table has " << cases.size() << " cases but kCaseCount is " << kCaseCount << "\n";
        return 1;
    }
    for (const Case& item : cases) {
        ++g_ran;
        std::cout << "CheckRelocation: " << g_ran << "/" << kCaseCount << " " << item.name << " " << std::flush;
        try {
            item.body();
            std::cout << "[ OK ]\n";
        } catch (const std::exception& error) {
            ++g_failed;
            std::cout << "[ FAIL ] " << error.what() << "\n";
        }
    }
    std::cout << "CheckRelocation: " << (kCaseCount - g_failed) << "/" << kCaseCount << " cases passed\n";
    return g_failed == 0 ? 0 : 1;
}
