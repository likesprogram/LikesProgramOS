/* main.cpp
    CheckStorage：S1 存储抽象的宿主侧检查，按需手工执行，不构成默认构建依赖

    检查分两类：
    1. 用例检查：统一路径的语法与分量切分、共用的名字比较、偏移视图的对齐与边界、
       错误码诊断文本，以及文件系统基类的路径解析接线
    2. 双编译检查：同一份 Storage 源码在 32 位下重新编译、链接并运行同一批用例；
       另按目标侧选项（与 Baleen 各阶段一致的 -m32 -ffreestanding 无运行库选项）
       做一次链接检查，并核对产物不牵入 64 位除法辅助函数

    本工具的源码在 32 位下再编译一份作为检查对象：那一次只跑用例，不做双编译
    输入路径从可执行文件位置推断（Tools/Bin/CheckStorage 向上三级是项目根），
    --root 可覆盖；缺少 32 位工具链时按基础设施缺失报告（退出码 2），--strict 把它当作失败
*/

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <Storage/BlockDevice.hpp>
#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/Path.hpp>
#include <Storage/SubDevice.hpp>

#ifndef __i386__
#include <filesystem>
#include <fstream>
#include <iterator>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
#ifndef __i386__
    namespace fs = std::filesystem;
#endif
    // 用例总数：末尾与实跑数核对，防止用例被误删
    constexpr unsigned kCaseCount = 24;
    // 已跑用例数
    unsigned g_ran = 0;

    // 中止当前检查
    [[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
    // 条件不成立即中止
    void Require(bool value, const std::string& message) { if (!value) Fail(message); }

    // 跑一个用例：失败时打印用例名后把原因抛给调用方
    void Case(const char* name, void (*body)()) {
        ++g_ran;
        std::cout << "CheckStorage: " << g_ran << "/" << kCaseCount << " " << name << " " << std::flush;
        try {
            body();
        } catch (const std::exception& error) {
            std::cout << "失败：" << error.what() << std::endl;
            throw;
        }
        std::cout << "通过" << std::endl;
    }

    // 假块设备：内存缓冲上的固定大小逻辑块设备，模拟目标侧的对齐与边界约束
    class FakeDevice final : public Storage::BlockDevice {
    public:
        // 建一个 capacityBytes 字节的设备；逻辑块大小由调用方给定，0 或非 2 的幂用于反向用例
        FakeDevice(uint32_t blockBytes, uint64_t capacityBytes, bool writable = true)
            : m_blockBytes(blockBytes), m_writable(writable), m_data(static_cast<std::size_t>(capacityBytes), 0) {}
        // 逻辑块大小，字节
        uint32_t BlockBytes() const override { return m_blockBytes; }
        // 设备容量，字节
        uint64_t CapacityBytes() const override { return static_cast<uint64_t>(m_data.size()); }
        // 写能力
        bool Writable() const override { return m_writable; }
        // 区间读：对齐或边界非法返回 InvalidArgument，读到缓冲返回 Ok
        Storage::Error Read(uint64_t offset, void* buffer, uint32_t bytes) override {
            ++m_reads;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            if (!Aligned(offset) || !Aligned(bytes)) return Storage::Error::InvalidArgument;
            const uint64_t size = static_cast<uint64_t>(m_data.size());
            if (offset > size || static_cast<uint64_t>(bytes) > size - offset) return Storage::Error::InvalidArgument;
            std::memcpy(buffer, m_data.data() + offset, bytes);
            return Storage::Error::Ok;
        }
        // 区间写：只读设备返回 ReadOnly，对齐或边界非法返回 InvalidArgument
        Storage::Error Write(uint64_t offset, const void* buffer, uint32_t bytes) override {
            ++m_writes;
            if (!m_writable) return Storage::Error::ReadOnly;
            if (buffer == nullptr || bytes == 0) return Storage::Error::InvalidArgument;
            if (!Aligned(offset) || !Aligned(bytes)) return Storage::Error::InvalidArgument;
            const uint64_t size = static_cast<uint64_t>(m_data.size());
            if (offset > size || static_cast<uint64_t>(bytes) > size - offset) return Storage::Error::InvalidArgument;
            std::memcpy(m_data.data() + offset, buffer, bytes);
            return Storage::Error::Ok;
        }
        // 刷新：只计数
        Storage::Error Flush() override { ++m_flushes; return Storage::Error::Ok; }
        // 清空动作计数，便于核对某个动作是否触及底层
        void ResetCounters() { m_reads = 0; m_writes = 0; m_flushes = 0; }
        // 读次数
        unsigned Reads() const { return m_reads; }
        // 写次数
        unsigned Writes() const { return m_writes; }
        // 刷新次数
        unsigned Flushes() const { return m_flushes; }
        // 用可识别的字节模式填充整个设备：第 i 字节为 seed + (i & 0xFF)
        void Fill(std::uint8_t seed) {
            for (std::size_t i = 0; i < m_data.size(); ++i) m_data[i] = static_cast<std::uint8_t>(seed + (i & 0xFF));
        }
        // 底层字节，用于核对写入是否落到预期偏移
        const std::vector<std::uint8_t>& Data() const { return m_data; }

    private:
        // 按块大小对齐；块大小为零或非 2 的幂时一律判为未对齐
        bool Aligned(uint64_t value) const {
            if (m_blockBytes == 0 || (m_blockBytes & (m_blockBytes - 1)) != 0) return false;
            return (value & (m_blockBytes - 1)) == 0;
        }

        uint32_t m_blockBytes = 0;             // 逻辑块大小，字节
        bool m_writable = false;               // 写能力
        std::vector<std::uint8_t> m_data;      // 底层字节
        unsigned m_reads = 0;                  // 读次数
        unsigned m_writes = 0;                 // 写次数
        unsigned m_flushes = 0;                // 刷新次数
    };

    // 假文件系统：三层固定节点，验证基类的路径解析接线与错误码映射
    class FakeFileSystem final : public Storage::FileSystem {
    public:
        // 格式名，诊断用
        const char* Format() const override { return "fake"; }
        // 挂载：只读挂载成功，读写挂载报不支持
        Storage::Error Mount(Storage::BlockDevice& device, Storage::MountMode mode) override {
            (void)device;
            if (mode != Storage::MountMode::ReadOnly) return Storage::Error::NotSupported;
            return Storage::Error::Ok;
        }
        // 能力：支持目录遍历，其余取缺省
        Storage::FileSystemCaps Caps() const override {
            Storage::FileSystemCaps caps;
            caps.directoryListing = true;
            return caps;
        }

    protected:
        // 根目录标识：固定为 1
        Storage::Error RootId(Storage::FileId& id) override { id.value = 1; return Storage::Error::Ok; }
        // 一层查找：1 号目录下有 dir，2 号目录下有 file，3 号是常规文件
        Storage::Error FindChild(const Storage::FileId& directory, const char* name, uint32_t nameBytes,
                                 Storage::FileId& child) override {
            if (directory.value == 1 && Storage::NameEquals(name, nameBytes, "dir", 3)) { child.value = 2; return Storage::Error::Ok; }
            if (directory.value == 2 && Storage::NameEquals(name, nameBytes, "file", 4)) { child.value = 3; return Storage::Error::Ok; }
            if (directory.value == 3) return Storage::Error::NotDirectory;   // 常规文件没有子项
            return Storage::Error::NotFound;
        }
        // 事实：1 与 2 是目录，3 是 512 字节的常规文件
        Storage::Error StatId(const Storage::FileId& id, Storage::FileInfo& info) override {
            if (id.value == 1 || id.value == 2) {
                info.bytes = 0;
                info.id = id.value;
                info.directory = true;
                return Storage::Error::Ok;
            }
            if (id.value == 3) {
                info.bytes = 512;
                info.id = id.value;
                info.directory = false;
                return Storage::Error::Ok;
            }
            return Storage::Error::NotFound;
        }
    };

    // 用例：根路径合法且没有分量
    void CaseRootPath() {
        Require(Storage::CheckPath("/") == Storage::Error::Ok, "根路径应合法");
        Storage::PathWalker walker("/");
        Require(walker.Check() == Storage::Error::Ok, "根路径绑定应通过");
        Require(walker.Count() == 0 && !walker.HasName(), "根路径没有分量");
        Require(walker.Name() == nullptr && walker.NameBytes() == 0, "没有分量时名字为空");
    }

    // 用例：多层合法路径
    void CasePlainPaths() {
        for (const char* path : {"/a", "/usr/lib", "/a-b/c_d/e.f", "/a b/c"}) {
            Require(Storage::CheckPath(path) == Storage::Error::Ok, std::string("应合法的路径：") + path);
        }
    }

    // 用例：空指针与非绝对路径
    void CaseBadPaths() {
        Require(Storage::CheckPath(nullptr) == Storage::Error::InvalidArgument, "空指针应报参数非法");
        Require(Storage::CheckPath("") == Storage::Error::InvalidArgument, "空串应报参数非法");
        Require(Storage::CheckPath("usr/lib") == Storage::Error::InvalidArgument, "非绝对路径应报参数非法");
    }

    // 用例：空分量与结尾分隔符
    void CaseEmptySegments() {
        Require(Storage::CheckPath("//a") == Storage::Error::InvalidArgument, "连续分隔符产生空分量");
        Require(Storage::CheckPath("/a//b") == Storage::Error::InvalidArgument, "中间空分量应拒绝");
        Require(Storage::CheckPath("/a/") == Storage::Error::InvalidArgument, "结尾分隔符应拒绝");
        Require(Storage::CheckPath("/a/b/") == Storage::Error::InvalidArgument, "结尾分隔符应拒绝");
    }

    // 用例：点分量
    void CaseDotSegments() {
        Require(Storage::CheckPath("/.") == Storage::Error::InvalidArgument, "单点分量应拒绝");
        Require(Storage::CheckPath("/..") == Storage::Error::InvalidArgument, "双点分量应拒绝");
        Require(Storage::CheckPath("/a/./b") == Storage::Error::InvalidArgument, "路径里的点分量应拒绝");
        Require(Storage::CheckPath("/a/../b") == Storage::Error::InvalidArgument, "路径里的双点分量应拒绝");
        Require(Storage::CheckPath("/...") == Storage::Error::Ok, "三个点不是点分量");
    }

    // 用例：整条路径的长度上限；用多段构造，避开单分量上限
    void CasePathLengthLimit() {
        std::string atLimit;
        for (int i = 0; i < 4; ++i) { atLimit += "/"; atLimit += std::string(204, 'z'); }
        atLimit += "/";
        atLimit += std::string(203, 'z');
        Require(atLimit.size() == Storage::PathRules::kMaxBytes, "用例自身构造有误");
        Require(Storage::CheckPath(atLimit.c_str()) == Storage::Error::Ok, "恰好到上限的路径应合法");

        std::string overLimit;
        for (int i = 0; i < 5; ++i) { overLimit += "/"; overLimit += std::string(204, 'z'); }
        Require(overLimit.size() == Storage::PathRules::kMaxBytes + 1, "用例自身构造有误");
        Require(Storage::CheckPath(overLimit.c_str()) == Storage::Error::NameTooLong, "超出上限应报 NameTooLong");
    }

    // 用例：单个分量的长度上限
    void CaseNameLengthLimit() {
        const std::string maxName = "/" + std::string(Storage::PathRules::kMaxNameBytes, 'n');
        Require(Storage::CheckPath(maxName.c_str()) == Storage::Error::Ok, "恰好到上限的分量应合法");
        const std::string overName = "/" + std::string(Storage::PathRules::kMaxNameBytes + 1, 'n');
        Require(Storage::CheckPath(overName.c_str()) == Storage::Error::NameTooLong, "超上限分量应报 NameTooLong");
    }

    // 用例：非 ASCII 与空格按原字节使用
    void CaseRawBytes() {
        const char raw[] = {'/', ' ', static_cast<char>(0xE4), static_cast<char>(0xB8), static_cast<char>(0xAD), '\0'};
        Require(Storage::CheckPath(raw) == Storage::Error::Ok, "非 ASCII 与空格应合法");
        Storage::PathWalker walker(raw);
        Require(walker.Check() == Storage::Error::Ok && walker.Count() == 1, "应切出一个分量");
        Require(walker.NameBytes() == 4, "分量应按字节计数");
        Require(std::memcmp(walker.Name(), raw + 1, 4) == 0, "分量字节应原样保留");
    }

    // 用例：非法路径的遍历器不可用
    void CaseWalkerInvalid() {
        Storage::PathWalker walker("usr/lib");
        Require(walker.Check() == Storage::Error::InvalidArgument, "绑定应给出语法错误");
        Require(!walker.HasName() && walker.Count() == 0, "语法不通过时没有分量");
        Require(walker.Name() == nullptr && walker.NameBytes() == 0, "语法不通过时名字为空");
        walker.Advance();
        Require(!walker.HasName(), "前进不应产生分量");
    }

    // 用例：分量序列与总数
    void CaseWalkerSequence() {
        Storage::PathWalker walker("/usr/lib/x86");
        Require(walker.Check() == Storage::Error::Ok, "合法路径绑定应通过");
        Require(walker.Count() == 3, "分量总数应为 3");
        for (const char* name : {"usr", "lib", "x86"}) {
            Require(walker.HasName(), "应有待处理分量");
            Require(Storage::NameEquals(walker.Name(), walker.NameBytes(), name, static_cast<uint32_t>(std::strlen(name))), "分量内容不符");
            walker.Advance();
        }
        Require(!walker.HasName() && walker.NameBytes() == 0, "末尾不应再有分量");
        walker.Advance();
        Require(!walker.HasName(), "再次前进应保持没有分量");
    }

    // 用例：逐字节的名字比较
    void CaseNameEquals() {
        Require(Storage::NameEquals("abc", 3, "abc", 3), "相同名字应相等");
        Require(!Storage::NameEquals("abc", 3, "ABC", 3), "逐字节比较区分大小写");
        Require(!Storage::NameEquals("abc", 3, "abcd", 4), "长度不同不应相等");
        Require(!Storage::NameEquals("", 0, "a", 1), "长度不同不应相等");
        Require(Storage::NameEquals("", 0, "", 0), "两个空名字应相等");
        Require(!Storage::NameEquals(nullptr, 0, "a", 1), "空指针不应相等");
        Require(!Storage::NameEquals("a", 1, nullptr, 0), "空指针不应相等");
    }

    // 用例：ASCII 折叠的名字比较
    void CaseNameEqualsIgnoreCase() {
        Require(Storage::NameEqualsIgnoreCase("AbC", 3, "aBc", 3), "折叠后应相等");
        Require(Storage::NameEqualsIgnoreCase("abc", 3, "abc", 3), "同串应相等");
        Require(!Storage::NameEqualsIgnoreCase("abc", 3, "abd", 3), "内容不同不应相等");
        Require(!Storage::NameEqualsIgnoreCase("abc", 3, "ab", 2), "长度不同不应相等");
        Require(!Storage::NameEqualsIgnoreCase(nullptr, 0, "a", 1), "空指针不应相等");
    }

    // 用例：非 ASCII 字节在折叠比较里按原值
    void CaseNameEqualsIgnoreCaseRaw() {
        const char left[] = {static_cast<char>(0xC4), static_cast<char>(0x80)};
        const char same[] = {static_cast<char>(0xC4), static_cast<char>(0x80)};
        const char other[] = {static_cast<char>(0xC5), static_cast<char>(0x80)};
        Require(Storage::NameEqualsIgnoreCase(left, 2, same, 2), "相同非 ASCII 字节应相等");
        Require(!Storage::NameEqualsIgnoreCase(left, 2, other, 2), "不同非 ASCII 字节不应相等");
        Require(!Storage::NameEqualsIgnoreCase(left, 1, left, 2), "长度不同不应相等");
    }

    // 用例：单字节 ASCII 折叠
    void CaseAsciiFold() {
        Require(Storage::ToUpperAscii('a') == 'A' && Storage::ToUpperAscii('z') == 'Z', "小写应折叠为大写");
        Require(Storage::ToUpperAscii('A') == 'A' && Storage::ToUpperAscii('0') == '0', "非小写应原样");
        Require(Storage::ToUpperAscii(static_cast<char>(0xE4)) == static_cast<char>(0xE4), "非 ASCII 应原样");
        Require(Storage::ToLowerAscii('A') == 'a' && Storage::ToLowerAscii('Z') == 'z', "大写应折叠为小写");
        Require(Storage::ToLowerAscii('a') == 'a' && Storage::ToLowerAscii('9') == '9', "非大写应原样");
        Require(Storage::ToLowerAscii(static_cast<char>(0xE4)) == static_cast<char>(0xE4), "非 ASCII 应原样");
    }

    // 用例：未绑定的偏移视图
    void CaseSubDeviceUnbound() {
        Storage::SubDevice view;
        Require(!view.Bound(), "默认构造应为未绑定");
        Require(view.BlockBytes() == 0 && view.CapacityBytes() == 0, "未绑定时事实查询为零");
        Require(!view.Writable(), "未绑定时不可写");
        std::uint8_t buffer[512] = {};
        Require(view.Read(0, buffer, 512) == Storage::Error::InvalidArgument, "未绑定时读应报参数非法");
        Require(view.Write(0, buffer, 512) == Storage::Error::InvalidArgument, "未绑定时写应报参数非法");
        Require(view.Flush() == Storage::Error::InvalidArgument, "未绑定时刷新应报参数非法");
    }

    // 用例：绑定的对齐与边界校验，任一项不通过都不留下绑定
    void CaseSubDeviceOpenChecks() {
        FakeDevice device(512, 65536);
        Storage::SubDevice view;
        Require(Storage::SubDevice::Open(device, 256, 512, view) == Storage::Error::InvalidArgument, "起点未对齐应拒绝");
        Require(!view.Bound(), "拒绝后不应留下绑定");
        Require(Storage::SubDevice::Open(device, 512, 0, view) == Storage::Error::InvalidArgument, "零长度应拒绝");
        Require(Storage::SubDevice::Open(device, 66048, 512, view) == Storage::Error::InvalidArgument, "起点越界应拒绝");
        Require(Storage::SubDevice::Open(device, 65536, 512, view) == Storage::Error::InvalidArgument, "起点等于容量且长度非零应拒绝");
        Require(Storage::SubDevice::Open(device, 65024, 1024, view) == Storage::Error::InvalidArgument, "区间越过容量应拒绝");
        const std::uint64_t nearTop = ~static_cast<std::uint64_t>(511);
        Require(Storage::SubDevice::Open(device, nearTop, 512, view) == Storage::Error::InvalidArgument, "64 位起点近顶应拒绝");
        Require(Storage::SubDevice::Open(device, 0, ~static_cast<std::uint64_t>(0), view) == Storage::Error::InvalidArgument, "长度取最大应拒绝");
        Require(!view.Bound(), "全部拒绝后不应留下绑定");
    }

    // 用例：底层块大小非 2 的幂或为零时拒绝绑定
    void CaseSubDeviceOpenBlockSize() {
        Storage::SubDevice view;
        FakeDevice odd(500, 4096);
        Require(Storage::SubDevice::Open(odd, 0, 512, view) == Storage::Error::InvalidArgument, "非 2 的幂块大小应拒绝");
        FakeDevice none(0, 4096);
        Require(Storage::SubDevice::Open(none, 0, 512, view) == Storage::Error::InvalidArgument, "零块大小应拒绝");
    }

    // 用例：绑定成功的事实与转发
    void CaseSubDeviceFacts() {
        FakeDevice device(512, 65536);
        device.Fill(0x40);
        Storage::SubDevice view;
        Require(Storage::SubDevice::Open(device, 4096, 8192, view) == Storage::Error::Ok, "对齐区间应绑定成功");
        Require(view.Bound(), "绑定后应为已绑定");
        Require(view.BlockBytes() == 512, "块大小应取底层");
        Require(view.CapacityBytes() == 8192, "容量应为区间长度");
        Require(view.Writable() == device.Writable(), "写能力应随底层");
        std::uint8_t buffer[512] = {};
        device.ResetCounters();
        Require(view.Read(512, buffer, 512) == Storage::Error::Ok, "区间内读应成功");
        Require(device.Reads() == 1, "读应转发到底层一次");
        const std::uint8_t first = static_cast<std::uint8_t>(0x40 + (4608 & 0xFF));
        const std::uint8_t last = static_cast<std::uint8_t>(0x40 + (5119 & 0xFF));
        Require(buffer[0] == first && buffer[511] == last, "读到的字节应来自底层对应偏移");
        Require(view.Flush() == Storage::Error::Ok, "刷新应转发到底层");
        Require(device.Flushes() == 1, "刷新应转发到底层一次");
    }

    // 用例：区间读的非法参数与边界
    void CaseSubDeviceReadChecks() {
        FakeDevice device(512, 65536);
        Storage::SubDevice view;
        Require(Storage::SubDevice::Open(device, 4096, 8192, view) == Storage::Error::Ok, "对齐区间应绑定成功");
        std::uint8_t buffer[512] = {};
        Require(view.Read(256, buffer, 512) == Storage::Error::InvalidArgument, "偏移未对齐应拒绝");
        Require(view.Read(0, buffer, 513) == Storage::Error::InvalidArgument, "长度未按块对齐应拒绝");
        Require(view.Read(0, buffer, 0) == Storage::Error::InvalidArgument, "零长度应拒绝");
        Require(view.Read(0, nullptr, 512) == Storage::Error::InvalidArgument, "空指针应拒绝");
        Require(view.Read(8192, buffer, 512) == Storage::Error::InvalidArgument, "起点等于区间长应拒绝");
        Require(view.Read(7680, buffer, 1024) == Storage::Error::InvalidArgument, "区间尾部越界应拒绝");
        Require(view.Read(8192 - 512, buffer, 512) == Storage::Error::Ok, "区间最后一块应可读");
        std::vector<std::uint8_t> whole(8192, 0);
        Require(view.Read(0, whole.data(), static_cast<uint32_t>(whole.size())) == Storage::Error::Ok, "整段应可读");
    }

    // 用例：区间写的转发与只读拒绝
    void CaseSubDeviceWriteChecks() {
        FakeDevice device(512, 65536);
        Storage::SubDevice view;
        Require(Storage::SubDevice::Open(device, 4096, 8192, view) == Storage::Error::Ok, "对齐区间应绑定成功");
        std::uint8_t pattern[512];
        for (unsigned i = 0; i < 512; ++i) pattern[i] = static_cast<std::uint8_t>(i);
        device.ResetCounters();
        Require(view.Write(0, pattern, 512) == Storage::Error::Ok, "可写底层的区间写应成功");
        Require(device.Writes() == 1, "写应转发到底层一次");
        Require(device.Data()[4096] == pattern[0] && device.Data()[4607] == pattern[511], "写应落到区间起点的底层偏移");
        Require(view.Write(256, pattern, 512) == Storage::Error::InvalidArgument, "未对齐写应拒绝");

        FakeDevice readOnly(512, 65536, false);
        Storage::SubDevice readOnlyView;
        Require(Storage::SubDevice::Open(readOnly, 0, 4096, readOnlyView) == Storage::Error::Ok, "只读底层也应能建视图");
        Require(!readOnlyView.Writable(), "只读底层的视图不可写");
        readOnly.ResetCounters();
        Require(readOnlyView.Write(0, pattern, 512) == Storage::Error::ReadOnly, "只读底层的写应报 ReadOnly");
        Require(readOnly.Writes() == 0, "只读拒绝不应触及底层");
    }

    // 用例：错误码的诊断文本
    void CaseErrorMessages() {
        const Storage::Error codes[] = {
            Storage::Error::Ok, Storage::Error::NotFound, Storage::Error::NotDirectory, Storage::Error::IsDirectory,
            Storage::Error::NotSupported, Storage::Error::Corrupt, Storage::Error::Io, Storage::Error::ReadOnly,
            Storage::Error::NoSpace, Storage::Error::NameTooLong, Storage::Error::InvalidArgument,
        };
        constexpr std::size_t count = sizeof(codes) / sizeof(codes[0]);
        const char* texts[count] = {};
        for (std::size_t i = 0; i < count; ++i) {
            texts[i] = Storage::ErrorMessage(codes[i]);
            Require(texts[i] != nullptr && texts[i][0] != '\0', "错误文本不应为空");
            Require(std::strcmp(texts[i], "unknown error") != 0, "已定义错误码应有专属文本");
        }
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t j = i + 1; j < count; ++j) {
                Require(std::strcmp(texts[i], texts[j]) != 0, "不同错误码的文本不应相同");
            }
        }
        Require(std::strcmp(Storage::ErrorMessage(Storage::Error::Ok), "ok") == 0, "Ok 的文本应为 ok");
        Require(std::strcmp(Storage::ErrorMessage(Storage::Error::NotFound), "not found") == 0, "NotFound 的文本应为 not found");
        Require(std::strcmp(Storage::ErrorMessage(Storage::Error::InvalidArgument), "invalid argument") == 0, "InvalidArgument 的文本应稳定");
        Require(std::strcmp(Storage::ErrorMessage(static_cast<Storage::Error>(999)), "unknown error") == 0, "未知取值应给兜底文本");
    }

    // 用例：文件系统基类的默认实现与能力位
    void CaseFileSystemDefaults() {
        FakeFileSystem fake;
        Require(std::strcmp(fake.Format(), "fake") == 0, "格式名应取自实现");
        const Storage::FileSystemCaps caps = fake.Caps();
        Require(!caps.writable && caps.directoryListing && !caps.symlinks, "能力位应取自实现");
        Require(caps.nameCase == Storage::NameCase::Sensitive, "缺省名字比较语义应区分大小写");

        FakeDevice volume(512, 65536);
        Require(fake.Mount(volume, Storage::MountMode::ReadWrite) == Storage::Error::NotSupported, "只读实现应拒绝读写挂载");
        Require(fake.Mount(volume, Storage::MountMode::ReadOnly) == Storage::Error::Ok, "只读挂载应成功");

        Storage::FileSystem& base = fake;
        std::uint8_t buffer[512] = {};
        uint32_t read = 0;
        const Storage::FileId root{1};
        Require(base.Read(root, 0, buffer, 512, read) == Storage::Error::NotSupported, "未覆盖的读应报不支持");
        uint64_t start = 0;
        char name[64] = {};
        uint32_t nameLength = 0;
        Storage::FileInfo info;
        Require(base.ReadDirectory(root, start, name, static_cast<uint32_t>(sizeof(name)), nameLength, info) == Storage::Error::NotSupported, "未覆盖的遍历应报不支持");
        base.Unmount();
    }

    // 用例：基类的路径解析接线
    void CaseFileSystemResolve() {
        FakeFileSystem fake;
        Storage::FileSystem& base = fake;
        Storage::FileId id;
        Require(base.Open("/", id) == Storage::Error::Ok && id.value == 1, "根路径应解析到根目录");
        Require(base.Open("/dir", id) == Storage::Error::Ok && id.value == 2, "一层路径应解析");
        Require(base.Open("/dir/file", id) == Storage::Error::Ok && id.value == 3, "两层路径应解析");
        Require(base.Open("/dir/nope", id) == Storage::Error::NotFound, "不存在的名字应上报 NotFound");
        Require(base.Open("/nope", id) == Storage::Error::NotFound, "不存在的顶层名字应上报 NotFound");
        Require(base.Open("/dir/file/x", id) == Storage::Error::NotDirectory, "中间项不是目录应上报 NotDirectory");
        Require(base.Open("dir/file", id) == Storage::Error::InvalidArgument, "非绝对路径应上报参数非法");
    }

    // 用例：按路径查询对象事实
    void CaseFileSystemStat() {
        FakeFileSystem fake;
        Storage::FileSystem& base = fake;
        Storage::FileInfo info;
        Require(base.Stat("/dir/file", info) == Storage::Error::Ok, "常规文件应可查询");
        Require(info.bytes == 512 && info.id == 3 && !info.directory, "常规文件事实应取自实现");
        Require(base.Stat("/dir", info) == Storage::Error::Ok, "目录应可查询");
        Require(info.bytes == 0 && info.id == 2 && info.directory, "目录事实应取自实现");
        Require(base.Stat("/dir/file/x", info) == Storage::Error::NotDirectory, "查询应传播解析错误");
    }

    // 跑全部用例并核对数量
    void RunCases() {
        Case("path 根路径", CaseRootPath);
        Case("path 合法多层", CasePlainPaths);
        Case("path 空指针与非绝对", CaseBadPaths);
        Case("path 空分量与尾分隔符", CaseEmptySegments);
        Case("path 点分量", CaseDotSegments);
        Case("path 长度上限", CasePathLengthLimit);
        Case("path 单分量上限", CaseNameLengthLimit);
        Case("path 非 ASCII 与空格", CaseRawBytes);
        Case("walker 非法路径不可用", CaseWalkerInvalid);
        Case("walker 分量序列", CaseWalkerSequence);
        Case("name 逐字节比较", CaseNameEquals);
        Case("name ASCII 折叠", CaseNameEqualsIgnoreCase);
        Case("name 非 ASCII 折叠按原值", CaseNameEqualsIgnoreCaseRaw);
        Case("ascii 折叠单字节", CaseAsciiFold);
        Case("subdevice 未绑定", CaseSubDeviceUnbound);
        Case("subdevice 绑定校验", CaseSubDeviceOpenChecks);
        Case("subdevice 块大小非法", CaseSubDeviceOpenBlockSize);
        Case("subdevice 绑定事实与转发", CaseSubDeviceFacts);
        Case("subdevice 读的非法参数", CaseSubDeviceReadChecks);
        Case("subdevice 写与只读", CaseSubDeviceWriteChecks);
        Case("error 诊断文本", CaseErrorMessages);
        Case("filesystem 默认实现", CaseFileSystemDefaults);
        Case("filesystem 路径解析", CaseFileSystemResolve);
        Case("filesystem 事实查询", CaseFileSystemStat);
        Require(g_ran == kCaseCount, "用例数量与登记不符");
        std::cout << "CheckStorage: " << g_ran << " 项用例全部通过" << std::endl;
    }

#ifdef __i386__
    // 32 位版本的用例运行：本工具源码在 32 位下再编译一份，作为双编译检查的运行对象
    int RunCasesMain() {
        try {
            RunCases();
        } catch (const std::exception&) {
            return 1;
        }
        return 0;
    }
#else
    // 32 位目标侧入口源码：按目标侧选项编译链接的探针，只引用符号不运行
    constexpr const char* kTargetEntrySource = R"SRC(
#include <Storage/Error.hpp>
#include <Storage/FileSystem.hpp>
#include <Storage/Path.hpp>
#include <Storage/SubDevice.hpp>

// 目标侧链接入口：引用各层符号，验证目标侧选项下能编译并链接
extern "C" void Storage_Target_Entry() {
    const char* const path = "/a/b";
    (void)Storage::CheckPath(path);
    Storage::PathWalker walker(path);
    (void)walker.Count();
    (void)Storage::NameEqualsIgnoreCase("a", 1, "A", 1);
    (void)Storage::ErrorMessage(Storage::Error::Io);
    Storage::SubDevice device;
    (void)device.Bound();
    for (;;) {}
}
)SRC";

    // CheckStorage 的命令行选项
    struct Options {
        fs::path root;             // 项目根，空表示从可执行文件位置推断
        fs::path work;             // 工作目录，空表示用临时目录
        bool keep = false;         // 无论成败都保留工作目录
        bool strict = false;       // 缺少 32 位工具链视为失败
        bool casesOnly = false;    // 只跑用例，跳过双编译检查
    };

    // 读环境变量，未设置或为空时取默认值
    std::string Env(const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return value && *value ? value : fallback;
    }

    // 打印用法
    void Usage() {
        std::cout << "CheckStorage：S1 存储抽象的宿主侧检查\n"
                     "用法：CheckStorage [选项]\n"
                     "  --root <目录>   项目根，默认从可执行文件位置推断\n"
                     "  --work <目录>   工作目录，默认建在临时目录下\n"
                     "  --keep          无论成败都保留工作目录\n"
                     "  --strict        缺少 32 位工具链时返回失败（默认返回 2）\n"
                     "  --cases-only    只跑用例，跳过 32 位双编译检查\n"
                     "  -h, --help      显示本说明\n"
                     "环境变量：CXX 指定 C++ 编译器（默认 g++）、NM 指定符号查看器（默认 nm）\n"
                     "退出码：0 全部通过、1 有失败、2 缺少 32 位工具链（未加 --strict）\n";
    }

    // 解析命令行
    Options Parse(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            if (argument == "-h" || argument == "--help") { Usage(); std::exit(0); }
            if (argument == "--keep") { options.keep = true; continue; }
            if (argument == "--strict") { options.strict = true; continue; }
            if (argument == "--cases-only") { options.casesOnly = true; continue; }
            if (argument == "--root" || argument == "--work") {
                Require(i + 1 < argc, std::string("选项缺少参数：") + std::string(argument));
                const fs::path value = argv[++i];
                if (argument == "--root") options.root = value; else options.work = value;
                continue;
            }
            Fail(std::string("未知选项：") + std::string(argument));
        }
        return options;
    }

    // 从可执行文件位置推断项目根：Tools/Bin/CheckStorage 向上三级
    fs::path DefaultRoot() {
        std::error_code error;
        const fs::path self = fs::read_symlink("/proc/self/exe", error);
        Require(!error && !self.empty(), "无法定位本工具的可执行文件，请用 --root 指定项目根");
        return fs::absolute(self).parent_path().parent_path().parent_path();
    }

    // 准备本次检查的工作目录
    fs::path MakeWorkDir(const fs::path& requested) {
        if (!requested.empty()) {
            std::error_code error;
            fs::create_directories(requested, error);
            Require(fs::is_directory(requested), "无法创建工作目录 " + requested.string());
            return fs::absolute(requested);
        }
        const fs::path base = fs::temp_directory_path() / ("checkstorage." + std::to_string(::getpid()));
        std::error_code error;
        fs::remove_all(base, error);
        fs::create_directories(base, error);
        Require(fs::is_directory(base), "无法创建工作目录 " + base.string());
        return base;
    }

    // 运行一个命令，标准输出与标准错误写进日志，返回退出码；exec 失败返回 127
    int Run(const std::vector<std::string>& command, const fs::path& log) {
        const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        Require(fd >= 0, "无法创建日志 " + log.string());
        const pid_t pid = ::fork();
        if (pid < 0) { ::close(fd); Fail("fork 失败"); }
        if (pid == 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
            std::vector<char*> argv;
            argv.reserve(command.size() + 1);
            for (const auto& argument : command) argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }
        ::close(fd);
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid) Fail("等待子进程失败");
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
    }

    // 在基础命令后追加参数，返回完整命令
    std::vector<std::string> With(const std::vector<std::string>& base, std::initializer_list<std::string> tail) {
        std::vector<std::string> command = base;
        command.insert(command.end(), tail.begin(), tail.end());
        return command;
    }

    // 读取文本文件，读不到时返回空串
    std::string ReadTextOrEmpty(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 整个覆盖写入文本
    void WriteText(const fs::path& path, std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(bool(output), "无法写入 " + path.string());
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        Require(bool(output), "写入失败 " + path.string());
    }

    // 打印日志尾部：失败时给出最后几行线索
    void ShowLogTail(const fs::path& log, unsigned lines) {
        std::ifstream input(log);
        std::vector<std::string> all;
        std::string line;
        while (std::getline(input, line)) all.push_back(line);
        const std::size_t first = all.size() > lines ? all.size() - lines : 0;
        for (std::size_t i = first; i < all.size(); ++i) std::cerr << "  | " << all[i] << std::endl;
    }

    // 32 位双编译与链接检查：返回 0 通过、2 缺少 32 位工具链，失败抛异常
    int Check32Bit(const fs::path& root, const std::string& cxx, const fs::path& work) {
        const fs::path storageSrc = root / "Packages/Common/Src/Storage";
        const fs::path storageInc = root / "Packages/Common/Include";
        const fs::path selfSrc = root / "Tools/Test/CheckStorage/Src/main.cpp";
        Require(fs::exists(storageSrc / "Path.cpp"), "找不到 Storage 源码：" + storageSrc.string());
        Require(fs::exists(storageInc / "Storage/Path.hpp"), "找不到 Storage 头文件：" + storageInc.string());
        Require(fs::exists(selfSrc), "找不到本工具源码：" + selfSrc.string());

        // 探测 32 位工具链：编译一个最小程序
        const fs::path probeSrc = work / "probe.cpp";
        WriteText(probeSrc, "int main() { return 0; }\n");
        if (Run({cxx, "-m32", "-std=c++20", "-O2", "-o", (work / "probe").string(), probeSrc.string()}, work / "probe.log") != 0) return 2;

        // 第一步：同一份用例在 32 位下编译、链接并运行
        const std::vector<std::string> hostCompile{cxx, "-m32", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                                                    "-Werror", "-I" + storageInc.string()};
        std::vector<std::string> objects;
        for (const char* name : {"Error.cpp", "Path.cpp", "SubDevice.cpp", "FileSystem.cpp"}) {
            const fs::path object = work / (std::string(name) + ".32.o");
            if (Run(With(hostCompile, {"-c", (storageSrc / name).string(), "-o", object.string()}), work / "compile32.log") != 0) {
                std::cerr << "CheckStorage: 32 位编译失败：" << (storageSrc / name) << std::endl;
                ShowLogTail(work / "compile32.log", 8);
                Fail("32 位编译失败");
            }
            objects.push_back(object.string());
        }
        const fs::path selfObject = work / "main.32.o";
        if (Run(With(hostCompile, {"-c", selfSrc.string(), "-o", selfObject.string()}), work / "compile32.log") != 0) {
            std::cerr << "CheckStorage: 32 位编译失败：" << selfSrc << std::endl;
            ShowLogTail(work / "compile32.log", 8);
            Fail("32 位编译失败");
        }
        objects.push_back(selfObject.string());

        const fs::path runner = work / "CheckStorage32";
        std::vector<std::string> link{cxx, "-m32", "-o", runner.string()};
        link.insert(link.end(), objects.begin(), objects.end());
        if (Run(link, work / "link32.log") != 0) {
            std::cerr << "CheckStorage: 32 位链接失败" << std::endl;
            ShowLogTail(work / "link32.log", 8);
            Fail("32 位链接失败");
        }
        if (Run({runner.string()}, work / "run32.log") != 0) {
            std::cerr << "CheckStorage: 32 位用例运行失败" << std::endl;
            ShowLogTail(work / "run32.log", 8);
            Fail("32 位用例运行失败");
        }
        std::cout << "CheckStorage: 32 位编译、链接与用例运行 通过" << std::endl;

        // 第二步：目标侧选项下的无运行库链接检查，选项与 Baleen 各阶段一致
        const fs::path entrySrc = work / "target_entry.cpp";
        WriteText(entrySrc, kTargetEntrySource);
        const std::vector<std::string> targetCompile{cxx, "-m32", "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions",
                                                     "-fno-rtti", "-fno-pic", "-fno-pie", "-fno-stack-protector",
                                                     "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror",
                                                     "-I" + storageInc.string()};
        std::vector<std::string> targetObjects;
        for (const char* name : {"Error.cpp", "Path.cpp", "SubDevice.cpp", "FileSystem.cpp"}) {
            const fs::path object = work / (std::string(name) + ".target.o");
            if (Run(With(targetCompile, {"-c", (storageSrc / name).string(), "-o", object.string()}), work / "compile-target.log") != 0) {
                std::cerr << "CheckStorage: 目标侧编译失败：" << (storageSrc / name) << std::endl;
                ShowLogTail(work / "compile-target.log", 8);
                Fail("目标侧编译失败");
            }
            targetObjects.push_back(object.string());
        }
        const fs::path entryObject = work / "target_entry.o";
        if (Run(With(targetCompile, {"-c", entrySrc.string(), "-o", entryObject.string()}), work / "compile-target.log") != 0) {
            std::cerr << "CheckStorage: 目标侧编译失败：" << entrySrc << std::endl;
            ShowLogTail(work / "compile-target.log", 8);
            Fail("目标侧编译失败");
        }
        targetObjects.push_back(entryObject.string());

        const fs::path targetElf = work / "target-check.elf";
        std::vector<std::string> targetLink{cxx, "-m32", "-nostdlib", "-static", "-Wl,--no-undefined", "-Wl,--fatal-warnings",
                                            "-Wl,-e,Storage_Target_Entry", "-o", targetElf.string()};
        targetLink.insert(targetLink.end(), targetObjects.begin(), targetObjects.end());
        if (Run(targetLink, work / "link-target.log") != 0) {
            std::cerr << "CheckStorage: 目标侧链接失败（-m32 -ffreestanding -nostdlib）" << std::endl;
            ShowLogTail(work / "link-target.log", 12);
            Fail("目标侧链接失败");
        }

        const std::string nm = Env("NM", "nm");
        if (Run({nm, targetElf.string()}, work / "nm.log") != 0) {
            std::cerr << "CheckStorage: 无法读取目标侧产物的符号表（NM=" << nm << "）" << std::endl;
            Fail("符号表读取失败");
        }
        const std::string symbols = ReadTextOrEmpty(work / "nm.log");
        for (const char* helper : {"__udivdi3", "__umoddi3", "__divdi3", "__moddi3", "__divmoddi4"}) {
            Require(symbols.find(helper) == std::string::npos, std::string("目标侧产物牵入 64 位除法辅助函数：") + helper);
        }
        std::cout << "CheckStorage: 目标侧无运行库链接与 64 位除法检查 通过" << std::endl;
        return 0;
    }

    // 64 位宿主的完整流程：跑用例并按参数决定是否做双编译检查
    int RunHostMain(int argc, char** argv) {
        fs::path work;
        bool keepWork = false;
        try {
            const Options options = Parse(argc, argv);
            keepWork = options.keep;
            RunCases();
            if (options.casesOnly) return 0;

            work = MakeWorkDir(options.work);
            const fs::path root = options.root.empty() ? DefaultRoot() : fs::absolute(options.root);
            const int result = Check32Bit(root, Env("CXX", "g++"), work);
            if (result == 2) {
                std::cout << "CheckStorage: 未执行 32 位检查（缺少 32 位工具链 -m32）" << std::endl;
                if (keepWork) std::cout << "CheckStorage: 工作目录保留在 " << work.string() << std::endl;
                else fs::remove_all(work);
                return options.strict ? 1 : 2;
            }
            if (keepWork) std::cout << "CheckStorage: 工作目录保留在 " << work.string() << std::endl;
            else fs::remove_all(work);
            std::cout << "CheckStorage: 全部检查通过" << std::endl;
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "CheckStorage: 失败：" << error.what() << std::endl;
            if (!work.empty()) std::cerr << "CheckStorage: 工作目录保留在 " << work.string() << "（便于查错）" << std::endl;
            return 1;
        }
    }
#endif
}

// 进程入口：按目标位宽分派到对应的运行函数；32 位版本只跑用例，由 64 位宿主驱动
#ifdef __i386__
int main() { return RunCasesMain(); }
#else
int main(int argc, char** argv) { return RunHostMain(argc, argv); }
#endif
