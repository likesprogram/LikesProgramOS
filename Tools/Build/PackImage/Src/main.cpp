/* main.cpp
    PackImage：填充 Baleen 镜像头里的 BuildId 与 Digest，或校验 Baleen 镜像与内核镜像

    带完整性头的 Stub 与 BaleenCore 同构：链接或组装产物转成平坦镜像后由本工具收尾
    内核镜像（LikesProgram.os）只校验、不填写：它的 BuildId 是内核集的构建级身份，
    由 MakeManifest 读原型、派生并写进最终 .os；本工具核对结构、身份两段的状态与摘要，
    带 --expect-build-id 时再与给定值逐字节比较
    --verify 只校验、不改写文件
*/

#include <BaleenImage.h>
#include <HostIo.h>
#include <KernelImage.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // 打印用法
    void Usage() {
        std::cout <<
            "PackImage — Baleen 镜像与内核镜像的完整性打包与校验\n"
            "\n"
            "用法：PackImage <镜像>                                    只适用于 Baleen 镜像：填 BuildId 与 Digest 后就地写回\n"
            "      PackImage --verify <镜像>                            只校验字段、身份与摘要，不改写文件\n"
            "      PackImage --verify --expect-build-id <64 位十六进制> <内核镜像>\n"
            "                                                           内核镜像的 BuildId 必须等于给定值\n"
            "\n"
            "镜像类别按起点的格式标记判定：BaleenStub、BaleenCore 或内核镜像（LPOSKRN）；\n"
            "Baleen 镜像须先由链接器或组装器填好长度字段与入口：头在文件偏移 0x10、入口在 0x90，\n"
            "头字段与入口位置固定。内核镜像的 BuildId 与 Digest 由\n"
            "MakeManifest 在生成内核集时写入，本工具不派生也不填写：BuildId 与 Digest 两段全零是\n"
            "“未分配身份的原型”，只做结构校验\n";
    }

    // 就地覆盖写回整个文件
    void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) throw std::runtime_error("写入失败：" + path);
    }

    // 内核镜像：只校验，身份由 MakeManifest 生成；原型与已分配身份分别打印
    int VerifyKernel(const std::string& path, const std::vector<uint8_t>& image, const std::string& expectedText) {
        std::array<uint8_t, hostbuild::kBuildIdBytes> expected{};
        std::span<const uint8_t> expectedSpan;
        if (!expectedText.empty()) {
            expected = hostbuild::ParseBuildIdText(expectedText);
            expectedSpan = expected;
        }
        const hostbuild::KernelIdentityState state = hostbuild::VerifyKernelImage(image, expectedSpan);
        if (state == hostbuild::KernelIdentityState::Prototype) {
            // 原型是构建链的中间产物：结构合法即可，身份由 MakeManifest 分配后再校验
            std::cout << "PackImage：内核镜像校验通过 " << path << "（" << image.size()
                      << " 字节，未分配身份的原型：BuildId 与 Digest 全零）\n";
            return 0;
        }
        std::cout << "PackImage：内核镜像校验通过 " << path << "（" << image.size()
                  << " 字节，BuildId " << hostbuild::KernelBuildIdText(image)
                  << (expectedText.empty() ? "" : "，与 --expect-build-id 一致") << "）\n";
        return 0;
    }
}

// 入口：解析参数，按镜像类别与 --verify 选择填充、校验或拒绝
int main(int argc, char** argv) {
    try {
        bool verify = false;
        std::string expectedText;
        std::string path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") {
                Usage();
                return 0;
            } else if (arg == "--verify") verify = true;
            else if (arg == "--expect-build-id") {
                if (i + 1 >= argc) throw std::runtime_error("选项 --expect-build-id 缺少参数");
                expectedText = argv[++i];
            } else if (path.empty()) path = arg;
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        if (path.empty()) {
            Usage();
            return 2;
        }
        if (!expectedText.empty() && !verify) throw std::runtime_error("--expect-build-id 只在校验模式（--verify）下可用");

        std::vector<uint8_t> image = hostbuild::ReadFile(path);
        // 内核镜像：数据排在文件起点，只校验；不带 --verify 直接拒绝，避免把原型写成正式产物
        if (hostbuild::IsKernelImage(image)) {
            if (!verify) throw std::runtime_error("内核镜像的身份（BuildId 与 Digest）由 MakeManifest 在生成内核集时写入；PackImage 只校验，请加 --verify：" + path);
            return VerifyKernel(path, image, expectedText);
        }
        if (!expectedText.empty()) throw std::runtime_error("--expect-build-id 只用于内核镜像：" + path);
        const hostbuild::ImageClass cls = hostbuild::ClassifyImage(image);
        if (!cls.header) throw std::runtime_error("镜像没有可识别的完整性头（" + cls.reason + "）：" + path);
        if (verify) {
            hostbuild::VerifyImage(image, cls.kind);
            std::cout << "PackImage：" << hostbuild::ImageKindName(cls.kind) << " 镜像校验通过 " << path
                      << "（" << image.size() << " 字节，BuildId " << hostbuild::ImageBuildIdText(image) << "）\n";
            return 0;
        }
        hostbuild::FillImageIdentity(image);
        hostbuild::VerifyImage(image, cls.kind);
        WriteFile(path, image);
        std::cout << "PackImage：已写入 BuildId 与 Digest " << path
                  << "（" << hostbuild::ImageKindName(cls.kind) << " 镜像，" << image.size()
                  << " 字节，BuildId " << hostbuild::ImageBuildIdText(image) << "）\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PackImage: " << error.what() << "\n";
        return 1;
    }
}
