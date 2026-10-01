/* main.cpp
    PackImage：填充 Baleen 镜像头里的 BuildId 与 Digest，并校验整个镜像

    带完整性头的 Stub 与 Core 同构：链接或组装产物转成平坦镜像后由本工具收尾
    先写入 BuildId 与 Digest，再复核一遍；此后镜像不得再改，任何字节变化都要重跑本工具
    --verify 只校验、不改写
*/

#include <BaleenImage.h>
#include <HostIo.h>

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    // 打印用法
    void Usage() {
        std::cout <<
            "PackImage — Baleen 镜像的完整性头打包与校验\n"
            "\n"
            "用法：PackImage <镜像>            填 BuildId 与 Digest 后就地写回\n"
            "      PackImage --verify <镜像>   只校验头字段与摘要，不改写文件\n"
            "\n"
            "镜像类别按头里的格式标记判定：BaleenStub 或 BaleenCore；\n"
            "镜像须先由链接器或组装器填好长度字段与入口：头在文件偏移 0x10、入口在 0x90，\n"
            "字节表见 Packages/Baleen/Stub/README.md 第三节\n";
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
