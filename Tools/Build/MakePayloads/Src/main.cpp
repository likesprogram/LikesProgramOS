/* main.cpp
    MakePayloads：生成带 PLACEHOLDER 标识的开发占位载荷，不实现 Baleen Stub/Core/UEFI 加载器或系统卷装配
*/

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <BaleenImage.h>
#include <CoreHandoff.hpp>
#include <ImageHeader.hpp>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {
    // 条件不成立即中止
    void Require(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 按小端序写入 count 个字节，越界即中止
    void Put(Bytes& data, size_t offset, uint64_t value, size_t count) {
        Require(offset <= data.size() && count <= data.size() - offset, "写入超出镜像边界");
        for (size_t i = 0; i < count; ++i) data[offset + i] = uint8_t(value >> (8 * i));
    }

    // 按小端序读取最多 4 个字节，越界即中止
    uint32_t Get(const Bytes& data, size_t offset, size_t count) {
        Require(offset <= data.size() && count <= data.size() - offset && count <= 4, "读取超出镜像边界");
        uint32_t result = 0;
        for (size_t i = 0; i < count; ++i) result |= uint32_t(data[offset + i]) << (8 * i);
        return result;
    }

    // 在 offset 处写入文本，越界即中止
    void Text(Bytes& data, size_t offset, std::string_view text) {
        Require(offset <= data.size() && text.size() <= data.size() - offset, "文本超出镜像边界");
        std::copy(text.begin(), text.end(), data.begin() + offset);
    }

    // 整个覆盖写入文件
    void Write(const fs::path& path, const Bytes& data) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        out.close();
        Require(bool(out), "写入失败：" + path.string());
    }

    // 读入整个文件，超过 64 MiB 视为异常
    Bytes Read(const fs::path& path) {
        const auto size = fs::file_size(path);
        Require(size <= 64 * 1024 * 1024, "占位载荷超出大小限制：" + path.string());
        Bytes data(static_cast<size_t>(size));
        std::ifstream in(path, std::ios::binary);
        in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
        Require(bool(in), "读取失败：" + path.string());
        return data;
    }

    // 建一个指定长度的空文件
    void CreateEmpty(const fs::path& path, uint64_t size) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.close();
        Require(bool(out), "创建失败：" + path.string());
        fs::resize_file(path, size);
    }

    // execvp 使用独立参数，不经 shell；输出路径中的空格等不会成为命令
    void Run(std::vector<std::string> arguments) {
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::cout.flush();
        const pid_t pid = fork();
        Require(pid >= 0, "fork 失败");
        if (pid == 0) {
            execvp(argv[0], argv.data());
            const int error = errno;
            std::cerr << "MakePayloads: 无法执行 " << arguments[0] << ": " << std::strerror(error) << '\n';
            _exit(127);
        }
        int status = 0;
        pid_t result;
        do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
        Require(result == pid, "waitpid 失败");
        Require(WIFEXITED(status) && WEXITSTATUS(status) == 0, arguments[0] + " 执行失败（status=" + std::to_string(status) + "）");
    }

    // 临时暂存目录：构造时建立，析构时整个删除
    struct Staging {
        fs::path path;  // 暂存目录路径
        // 在 out 下建一个唯一命名的暂存目录
        explicit Staging(const fs::path& out) {
            std::string name = (out / ".makepayloads.XXXXXX").string();
            Require(mkdtemp(name.data()) != nullptr, "无法创建临时载荷目录");
            path = name;
        }
        // 删除暂存目录
        ~Staging() { std::error_code error; fs::remove_all(path, error); }
    };

    // 占位 Stub：往串口与电传打印 PLACEHOLDER-STUB，然后停机
    constexpr std::string_view StubAssembly = R"ASM(BITS 16
ORG 0
_Stub:
    MOV AX, 0x07E0
    MOV DS, AX
    MOV DX, 0x3FB
    MOV AL, 0x03
    OUT DX, AL
    MOV DX, 0x3F9
    XOR AL, AL
    OUT DX, AL
    MOV DX, 0x3FA
    MOV AL, 0xC7
    OUT DX, AL
    MOV SI, _Msg
.Next:
    MOV AL, [SI]
    TEST AL, AL
    JZ .Hang
    OUT 0xE9, AL
    PUSH AX
.WaitTx:
    MOV DX, 0x3FD
    IN AL, DX
    TEST AL, 0x20
    JZ .WaitTx
    POP AX
    MOV DX, 0x3F8
    OUT DX, AL
    MOV AH, 0x0E
    MOV BX, 0x0007
    INT 0x10
    INC SI
    JMP .Next
.Hang:
    HLT
    JMP .Hang
_Msg: DB "PLACEHOLDER-STUB (BaleenStub not implemented yet)", 13, 10, 0
)ASM";

    // 占位 Core 的尾部填充长度：跨过 64KiB，让 Stub 的高位装载分两轮走完，而不是只读一个扇区
    constexpr std::size_t kPlaceholderCoreBytes = 0x11000;
    // Core 映像的装入物理地址，取值与 Contract.inc 的 CORE_LOAD 一致；入口偏移取自镜像头布局
    constexpr uint32_t kCoreLoad = 0x100000;

    // 占位 Core：Stub 把它装到 1MiB 并按 32 位保护模式进入，入口时 ESI 指向交权块
    // 交权块可用就经 Stub 的控制台打印标识；不可用时退回直接写 0xE9 与 COM1，标记仍要出现
    // @...@ 是待替换的交权块常量，由 FillHandoffConstants 用 CoreHandoff.hpp 的取值填上
    constexpr std::string_view CoreAssembly = R"ASM(BITS 32
ORG @CORE_ENTRY@
_Entry:
    CMP DWORD [ESI], @HANDOFF_MAGIC@
    JNE .Direct
    CMP DWORD [ESI + 4], @HANDOFF_VERSION@
    JNE .Direct
    CMP DWORD [ESI + 8], @HANDOFF_BYTES@
    JNE .Direct
    MOV EAX, [ESI + @HANDOFF_WRITE@]
    MOV EBX, _Msg
    PUSH EBX
    CALL EAX
    ADD ESP, 4
    JMP .Hang
.Direct:
    ; 交权块不可用：直接写 0xE9 与 COM1，并写明走的是退路
    MOV EBX, _MsgDirect
.Next:
    MOV AL, [EBX]
    TEST AL, AL
    JZ .Hang
    OUT 0xE9, AL
.WaitTx:
    MOV DX, 0x3FD
    IN AL, DX
    TEST AL, 0x20
    JZ .WaitTx
    MOV AL, [EBX]
    MOV DX, 0x3F8
    OUT DX, AL
    INC EBX
    JMP .Next
.Hang:
    HLT
    JMP .Hang
_Msg: DB "PLACEHOLDER-CORE-IMAGE (BaleenCore not implemented yet)", 13, 10, 0
_MsgDirect: DB "PLACEHOLDER-CORE-IMAGE (handoff unavailable)", 13, 10, 0
)ASM";

    // 把占位 Core 汇编里的 @...@ 换成头文件里的常量：交权块字段与入口地址
    // 偏移只在头文件里写一次：结构体一改，这里与 Stub 侧的断言会同时失败
    std::string FillAssemblyConstants(std::string_view source) {
        // 按 0x 加 8 位大写十六进制格式化，与汇编里的字面量写法一致
        const auto hex = [](uint32_t value) {
            std::string text = "0x";
            for (int shift = 28; shift >= 0; shift -= 4) text += "0123456789ABCDEF"[(value >> shift) & 0xF];
            return text;
        };
        // 逐个替换所有出现处
        const auto replaceAll = [](std::string& text, std::string_view token, const std::string& value) {
            for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size())) text.replace(at, token.size(), value);
        };
        std::string text(source);
        replaceAll(text, "@HANDOFF_MAGIC@", hex(Baleen::CoreHandoffLayout::kMagic));
        replaceAll(text, "@HANDOFF_VERSION@", hex(Baleen::CoreHandoffLayout::kVersion));
        replaceAll(text, "@HANDOFF_BYTES@", hex(Baleen::CoreHandoffLayout::kBytes));
        replaceAll(text, "@HANDOFF_WRITE@", hex(Baleen::CoreHandoffLayout::kWrite));
        replaceAll(text, "@CORE_ENTRY@", hex(kCoreLoad + Baleen::ImageHeaderLayout::kEntryOffset));
        return text;
    }

    // 占位 EFI 应用：往串口打印 MAKEISO-EFI-BOOT-OK，然后停机
    constexpr std::string_view EfiAssembly = R"ASM(BITS 64
ORG 0
_Entry:
    MOV DX, 0x3FB
    MOV AL, 0x03
    OUT DX, AL
    MOV DX, 0x3F9
    XOR AL, AL
    OUT DX, AL
    MOV DX, 0x3FA
    MOV AL, 0xC7
    OUT DX, AL
    MOV DX, 0x3FC
    MOV AL, 0x03
    OUT DX, AL
    LEA RBX, [REL _Msg]
.Next:
    MOV AL, [RBX]
    TEST AL, AL
    JZ .Hang
.Wait:
    MOV DX, 0x3FD
    IN AL, DX
    TEST AL, 0x20
    JZ .Wait
    MOV AL, [RBX]
    MOV DX, 0x3F8
    OUT DX, AL
    INC RBX
    JMP .Next
.Hang:
    JMP .Hang
_Msg: DB "MAKEISO-EFI-BOOT-OK", 13, 10
      DB "PLACEHOLDER-EFI (BaleenUefi not implemented yet)", 13, 10, 0
ALIGN 8
_RelocSlot: DQ 0
)ASM";

    // 组装占位 Core 的完整镜像：入口前缀 + 保留区 + 完整性头 + 代码，补零到固定长度后填身份
    // 占位件也要让 Stub 的头与摘要校验通过：占位是内容性质，不是跳过校验的理由
    Bytes BuildCoreImage(const Bytes& code) {
        const std::size_t entryAt = Baleen::ImageHeaderLayout::kEntryOffset;
        Require(code.size() + entryAt <= kPlaceholderCoreBytes, "占位 Core 代码超出映像长度上限");
        Bytes image(entryAt + code.size(), 0);
        // 文件起点的 16 位近跳转：位移相对指令末端，从装入点的偏移 0 跳到入口
        image[0] = Baleen::ImageHeaderLayout::kNearJumpOpcode;
        Put(image, 1, entryAt - Baleen::ImageHeaderLayout::kPrefixBytes, 2);
        const std::size_t head = Baleen::ImageHeaderLayout::kOffset;
        Text(image, head + Baleen::ImageHeaderLayout::kFieldMagic, std::string_view("BLNCORE\0", 8));
        Put(image, head + Baleen::ImageHeaderLayout::kFieldVersion, Baleen::ImageHeaderLayout::kVersion, 2);
        Put(image, head + Baleen::ImageHeaderLayout::kFieldHeaderBytes, Baleen::ImageHeaderLayout::kBytes, 2);
        Put(image, head + Baleen::ImageHeaderLayout::kFieldFlags, Baleen::ImageHeaderLayout::kFlags, 4);
        Put(image, head + Baleen::ImageHeaderLayout::kFieldEntryOffset, entryAt, 4);
        Put(image, head + Baleen::ImageHeaderLayout::kFieldDigestAlgorithm, Baleen::ImageHeaderLayout::kDigestSha256, 2);
        Put(image, head + Baleen::ImageHeaderLayout::kFieldBuildIdBytes, Baleen::ImageHeaderLayout::kBuildIdBytes, 2);
        std::copy(code.begin(), code.end(), image.begin() + entryAt);
        image.resize(kPlaceholderCoreBytes, 0);
        // 长度按补零后的文件算：占位件没有未落盘的静态内存，MemoryBytes 与文件长度相同
        Put(image, head + Baleen::ImageHeaderLayout::kFieldImageBytes, image.size(), 4);
        Put(image, head + Baleen::ImageHeaderLayout::kFieldMemoryBytes, image.size(), 4);
        makeiso::FillImageIdentity(image);
        // 自己先验一遍：占位件不经过 PackImage，任何字段写错都在这里暴露
        makeiso::VerifyImage(image, makeiso::ImageKind::Core);
        return image;
    }

    // 把一段 NASM 源码写成文件并汇编成二进制
    void Assemble(const fs::path& directory, const std::string& name, std::string_view source) {
        const fs::path assembly = directory / (name + ".asm");
        Write(assembly, Bytes(source.begin(), source.end()));
        Run({"nasm", "-f", "bin", "-Ox", "-o", (directory / (name + ".bin")).string(), assembly.string()});
    }

    // 把实模式代码包成最小的 PE32+ EFI 应用：两个节，代码用 RIP 相对寻址，唯一 DIR64 指向末尾保留槽
    // 规范：https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
    Bytes EfiPe(const Bytes& code) {
        Require(code.size() >= 8 && code.size() < 4096 && code.size() % 8 == 0, "EFI 占位代码大小非法");
        const size_t textRaw = (code.size() + 511) / 512 * 512;
        const size_t relocRaw = 512 + textRaw;
        Bytes pe(relocRaw + 512, 0);
        Text(pe, 0, "MZ");
        Put(pe, 0x3c, 0x40, 4);
        Text(pe, 0x40, std::string_view("PE\0\0", 4));
        Put(pe, 0x44, 0x8664, 2); // AMD64
        Put(pe, 0x46, 2, 2);
        Put(pe, 0x54, 0xf0, 2);
        Put(pe, 0x56, 0x22, 2); // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
        const size_t opt = 0x58;
        Put(pe, opt, 0x20b, 2);
        Put(pe, opt + 2, 1, 1);
        Put(pe, opt + 4, textRaw, 4);
        Put(pe, opt + 8, 512, 4);
        Put(pe, opt + 16, 0x1000, 4); // EntryPoint
        Put(pe, opt + 20, 0x1000, 4);
        Put(pe, opt + 24, 0x400000, 8);
        Put(pe, opt + 32, 0x1000, 4);
        Put(pe, opt + 36, 0x200, 4);
        Put(pe, opt + 40, 6, 2);
        Put(pe, opt + 48, 6, 2);
        Put(pe, opt + 56, 0x3000, 4);
        Put(pe, opt + 60, 0x200, 4);
        Put(pe, opt + 68, 10, 2); // EFI_APPLICATION
        Put(pe, opt + 72, 0x100000, 8);
        Put(pe, opt + 80, 0x1000, 8);
        Put(pe, opt + 88, 0x100000, 8);
        Put(pe, opt + 96, 0x1000, 8);
        Put(pe, opt + 108, 16, 4);
        Put(pe, opt + 152, 0x2000, 4);
        Put(pe, opt + 156, 12, 4);
        // 写一个节表项
        auto section = [&](size_t offset, std::string_view name, size_t size, size_t rva, size_t rawSize, size_t rawOffset, uint32_t flags) {
            Text(pe, offset, name);
            Put(pe, offset + 8, size, 4);
            Put(pe, offset + 12, rva, 4);
            Put(pe, offset + 16, rawSize, 4);
            Put(pe, offset + 20, rawOffset, 4);
            Put(pe, offset + 36, flags, 4);
        };
        section(opt + 0xf0, ".text", code.size(), 0x1000, textRaw, 512, 0x60000020);
        section(opt + 0xf0 + 40, ".reloc", 12, 0x2000, 512, relocRaw, 0x42000040);
        std::copy(code.begin(), code.end(), pe.begin() + 512);
        Put(pe, relocRaw, 0x1000, 4);
        Put(pe, relocRaw + 4, 12, 4);
        Put(pe, relocRaw + 8, (10u << 12) | (code.size() - 8), 2);
        // 最后一个 WORD 为 IMAGE_REL_BASED_ABSOLUTE，将块补齐到四字节
        return pe;
    }

    // 只接受本工具刚格式化的 FAT32；不提供通用文件系统编辑接口
    void InstallEfi(const fs::path& image, const Bytes& efi) {
        Bytes fat = Read(image);
        Require(Get(fat, 510, 2) == 0xaa55 && Get(fat, 11, 2) == 512 && Get(fat, 13, 1) == 1 && Get(fat, 22, 2) == 0, "mkfs.vfat 未生成预期 FAT32");
        const uint32_t reserved = Get(fat, 14, 2);
        const uint32_t copies = Get(fat, 16, 1);
        const uint32_t sectorsPerFat = Get(fat, 36, 4);
        const uint32_t root = Get(fat, 44, 4);
        const size_t fatStart = size_t(reserved) * 512;
        const size_t fatSize = size_t(sectorsPerFat) * 512;
        const size_t dataStart = fatStart + size_t(copies) * fatSize;
        Require(copies == 2 && root == 2 && sectorsPerFat > 0 && dataStart < fat.size(), "FAT32 布局非法");
        const size_t clusters = (fat.size() - dataStart) / 512;
        const size_t fileClusters = (efi.size() + 511) / 512;
        const uint32_t firstFile = 5;
        const size_t used = 2 + fileClusters;
        Require(clusters >= 65525 && firstFile + fileClusters < clusters + 2 && (clusters + 2) * 4 <= fatSize, "FAT32 空间不足");
        // 簇号到字节偏移
        auto offset = [&](uint32_t cluster) { return dataStart + size_t(cluster - 2) * 512; };
        for (uint32_t cluster = 3; cluster < firstFile + fileClusters; ++cluster) {
            Require((Get(fat, fatStart + cluster * 4, 4) & 0x0fffffff) == 0, "FAT32 卷不是空白卷");
            const uint32_t next = cluster >= firstFile && cluster + 1 < firstFile + fileClusters ? cluster + 1 : 0x0fffffff;
            for (uint32_t copy = 0; copy < copies; ++copy) Put(fat, fatStart + copy * fatSize + cluster * 4, next, 4);
        }
        // 写一个 FAT 目录项
        auto entry = [&](uint32_t directory, size_t slot, std::string_view name, uint8_t attributes, uint32_t cluster, uint32_t size) {
            Require(name.size() <= 11 && slot < 16, "FAT 目录项非法");
            const size_t at = offset(directory) + slot * 32;
            std::fill_n(fat.begin() + at, 32, 0);
            std::fill_n(fat.begin() + at, 11, ' ');
            Text(fat, at, name);
            Put(fat, at + 11, attributes, 1);
            const uint16_t date = ((2024 - 1980) << 9) | (1 << 5) | 1;
            Put(fat, at + 16, date, 2);
            Put(fat, at + 18, date, 2);
            Put(fat, at + 24, date, 2);
            Put(fat, at + 20, cluster >> 16, 2);
            Put(fat, at + 26, cluster & 0xffff, 2);
            Put(fat, at + 28, size, 4);
        };
        size_t rootSlot = 0;
        while (rootSlot < 16 && fat[offset(root) + rootSlot * 32] != 0) ++rootSlot;
        entry(root, rootSlot, "EFI", 0x10, 3, 0);
        entry(3, 0, ".", 0x10, 3, 0);
        entry(3, 1, "..", 0x10, 0, 0); // FAT 根目录的父簇必须是 0
        entry(3, 2, "BOOT", 0x10, 4, 0);
        entry(4, 0, ".", 0x10, 4, 0);
        entry(4, 1, "..", 0x10, 3, 0);
        entry(4, 2, "BOOTX64 EFI", 0x20, firstFile, uint32_t(efi.size()));
        std::copy(efi.begin(), efi.end(), fat.begin() + offset(firstFile));
        // 更新主/备 FSInfo 的空闲计数与 next-free 提示
        const uint32_t info = Get(fat, 48, 2);
        const uint32_t backup = Get(fat, 50, 2);
        for (uint32_t sector : {info, backup + info}) {
            Require(sector < reserved, "FSInfo 超出保留区");
            const size_t at = size_t(sector) * 512;
            Require(Get(fat, at, 4) == 0x41615252 && Get(fat, at + 484, 4) == 0x61417272, "FSInfo 签名非法");
            const uint32_t free = Get(fat, at + 488, 4);
            if (free != 0xffffffff) {
                Require(free >= used, "FSInfo 空闲计数不足");
                Put(fat, at + 488, free - used, 4);
            }
            Put(fat, at + 492, firstFile + fileClusters, 4);
        }
        Write(image, fat);
    }

    // 取 SOURCE_DATE_EPOCH 作为构建时间，未设置时用固定值
    std::string Epoch() {
        const char* value = std::getenv("SOURCE_DATE_EPOCH");
        std::string text = value && *value ? value : "1700000000";
        uint64_t seconds = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
        Require(error == std::errc{} && end == text.data() + text.size() && seconds <= std::numeric_limits<int32_t>::max(), "SOURCE_DATE_EPOCH 必须是 0..2147483647 的十进制秒数");
        return text;
    }

    // 返回输出目录里还缺哪些载荷；已有的必须仍带着占位标识，否则报错
    std::vector<std::string> CheckExisting(const fs::path& output) {
        std::vector<std::string> missing;
        for (const char* name : {"BaleenStub.bin", "BaleenCore.bin", "Efi.img", "SystemVolume.img", "BaleenLayout.bin"}) {
            const fs::path path = output / name;
            const auto status = fs::symlink_status(path);
            if (status.type() == fs::file_type::not_found) { missing.emplace_back(name); continue; }
            Require(fs::is_regular_file(status), "同名路径不是普通文件：" + path.string());
            const Bytes data = Read(path);
            const std::string_view bytes(reinterpret_cast<const char*>(data.data()), data.size());
            bool placeholder = false;
            if (std::string_view(name) == "SystemVolume.img") {
                // UUID 只识别生成来源，不能证明仍为空卷；所有已有文件均只复用
                constexpr uint8_t uuid[] = {0x11,0x11,0x11,0x11,0x22,0x22,0x33,0x33, 0x44,0x44,0x55,0x55,0x55,0x55,0x55,0x55};
                placeholder = data.size() == 16 * 1024 * 1024 && Get(data, 1024 + 56, 2) == 0xef53 && std::equal(std::begin(uuid), std::end(uuid), data.begin() + 1024 + 104);
            } else if (std::string_view(name) == "BaleenStub.bin" || std::string_view(name) == "BaleenCore.bin") {
                // 按完整性头与占位标识判定：占位 Core 带头，旧格式（无头）要删掉重生成
                const makeiso::ImageClass cls = makeiso::ClassifyImage(data);
                const makeiso::ImageKind expected = std::string_view(name) == "BaleenStub.bin" ? makeiso::ImageKind::Stub : makeiso::ImageKind::Core;
                placeholder = cls.kind == expected && cls.placeholder && (expected == makeiso::ImageKind::Stub || cls.header);
            } else {
                const std::string_view marker = std::string_view(name) == "Efi.img" ? "MAKEISO-EFI-BOOT-OK" : "PLACEHOLDER BaleenLayout.bin";
                placeholder = bytes.find(marker) != std::string_view::npos;
            }
            // 占位件换过格式时旧文件认不出来，这里只说明要删：本工具不覆写已有文件
            Require(placeholder, "已有文件不是当前格式的占位件（旧格式占位件需先删除），或请使用专门输出目录：" + path.string());
        }
        return missing;
    }

    // 补齐输出目录里缺失的占位载荷，已存在的一律不动
    void Generate(const fs::path& output) {
        const std::string epoch = Epoch();
        const auto missing = CheckExisting(output);
        if (missing.empty()) {
            std::cout << "MakePayloads: 复用已有占位载荷 " << output << "（不改写任何文件）\n";
            return;
        }
        // mkfs.ext4 不保证读取 SOURCE_DATE_EPOCH；同时设置其时间覆盖变量
        Require(setenv("SOURCE_DATE_EPOCH", epoch.c_str(), 1) == 0 && setenv("E2FSPROGS_FAKE_TIME", epoch.c_str(), 1) == 0 && setenv("TZ", "UTC", 1) == 0 && setenv("LC_ALL", "C", 1) == 0, "设置构建时间失败");
        fs::create_directories(output);
        Staging staging(output);
        const auto& dir = staging.path;
        Assemble(dir, "BaleenStub", StubAssembly);
        // 占位 Core 是真正的 32 位可执行映像：Stub 会校验它的头与摘要、把它装到 1MiB 并跳进去，
        // 入口前缀与完整性头由 BuildCoreImage 组装；尾部补零跨过 64KiB，高位装载的分批与拷贝路径也会被走到
        Assemble(dir, "coreapp", FillAssemblyConstants(CoreAssembly));
        Write(dir / "BaleenCore.bin", BuildCoreImage(Read(dir / "coreapp.bin")));
        Assemble(dir, "efiapp", EfiAssembly);
        const Bytes efi = EfiPe(Read(dir / "efiapp.bin"));
        CreateEmpty(dir / "Efi.img", 36 * 1024 * 1024);
        Run({"mkfs.vfat", "-F", "32", "-s", "1", "-n", "ESP", "--invariant", (dir / "Efi.img").string()});
        InstallEfi(dir / "Efi.img", efi);
        CreateEmpty(dir / "SystemVolume.img", 16 * 1024 * 1024);
        Run({"mkfs.ext4", "-q", "-F", "-U", "11111111-2222-3333-4444-555555555555", "-E", "hash_seed=66666666-7777-8888-9999-aaaaaaaaaaaa,root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0", (dir / "SystemVolume.img").string()});
        Bytes layout(512, 0);
        Text(layout, 0, "PLACEHOLDER BaleenLayout.bin\nBaleen layout descriptor is not specified yet; this file only fills the product slot.\n");
        Write(dir / "BaleenLayout.bin", layout);
        // 只补缺项。link 原子地拒绝已存在的目标，包括并行构建刚发布的文件；
        // staging 位于同一文件系统，不会出现 rename 覆盖用户修改的系统卷
        for (const auto& name : missing) if (link((dir / name).c_str(), (output / name).c_str()) != 0) throw std::runtime_error("发布失败（不覆盖已存在目标）：" + (output / name).string() + ": " + std::strerror(errno));
        std::cout << "MakePayloads: 已补齐 " << missing.size() << " 个占位件到 " << output
                  << "（已有文件保持原样）\n"
                  << "MakePayloads: 仅验证引导链，不包含真实 Stub/Core/UEFI 加载器或内核。\n";
    }
}

// 入口：参数是输出目录，--help 打印用法
int main(int argc, char** argv) {
    try {
        if (argc != 2 || std::string_view(argv[1]) == "--help") {
            std::cout << "用法：MakePayloads <输出目录>\n"
                         "仅生成带 PLACEHOLDER 标识的载荷；需要 nasm、mkfs.vfat、mkfs.ext4。\n"
                         "SOURCE_DATE_EPOCH 默认 1700000000。\n";
            return argc == 2 ? 0 : 2;
        }
        Require(*argv[1] != '\0', "输出目录不能为空");
        Generate(fs::absolute(argv[1]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MakePayloads: " << error.what() << '\n';
        return 1;
    }
}
