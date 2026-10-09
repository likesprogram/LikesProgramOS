/* main.cpp
    PackImage：填充 Baleen 镜像头或内核镜像数据里的 BuildId 与 Digest，并校验整个镜像

    带完整性头的 Stub 与 BaleenCore 同构：链接或组装产物转成平坦镜像后由本工具收尾
    内核镜像（LikesProgram.os）的起始是内核镜像数据：字段由链接器与汇编填出，本工具补齐
    BuildId 与 Digest 后复核一遍；此后镜像不得再改，任何字节变化都要重跑本工具
    --verify 只校验、不改写
*/

#include <BaleenImage.h>
#include <HostIo.h>
#include <KernelImage.h>

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // 打印用法
    void Usage() {
        std::cout <<
            "PackImage — Baleen 镜像与内核镜像的完整性打包与校验\n"
            "\n"
            "用法：PackImage <镜像>            填 BuildId 与 Digest 后就地写回\n"
            "      PackImage --verify <镜像>   只校验字段与摘要，不改写文件\n"
            "\n"
            "镜像类别按起点的格式标记判定：BaleenStub、BaleenCore 或内核镜像（LPOSKRN）；\n"
            "Baleen 镜像须先由链接器或组装器填好长度字段与入口：头在文件偏移 0x10、入口在 0x90，\n"
            "字节表见 Packages/Baleen/Stub/README.md 第三节；内核镜像的数据排在文件起点，\n"
            "字节表见 Docs/Specs/Baleen/内核镜像数据.md\n";
    }

    // 就地覆盖写回整个文件
    void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) throw std::runtime_error("写入失败：" + path);
    }
}

// 入口：解析参数，按 --verify 选择校验或填充
int main(int argc, char** argv) {
    try {
        bool verify = false;
        std::string path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") {
                Usage();
                return 0;
            } else if (arg == "--verify") verify = true;
            else if (path.empty()) path = arg;
            else throw std::runtime_error("无法识别的参数：" + arg);
        }
        if (path.empty()) {
            Usage();
            return 2;
        }

        std::vector<uint8_t> image = hostbuild::ReadFile(path);
        // 内核镜像：数据排在文件起点，按内核镜像数据的规则填充与校验
        if (hostbuild::IsKernelImage(image)) {
            if (verify) {
                hostbuild::VerifyKernelImage(image);
                std::cout << "PackImage：内核镜像校验通过 " << path
                          << "（" << image.size() << " 字节，BuildId " << hostbuild::KernelBuildIdText(image) << "）\n";
                return 0;
            }
            hostbuild::FillKernelIdentity(image);
            hostbuild::VerifyKernelImage(image);
            WriteFile(path, image);
            std::cout << "PackImage：已写入内核镜像的 BuildId 与 Digest " << path
                      << "（" << image.size() << " 字节，BuildId " << hostbuild::KernelBuildIdText(image) << "）\n";
            return 0;
        }
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
