/* Image.h
    输出镜像的按绝对字节偏移读写接口
*/

#pragma once
#include <cstdint>
#include <fstream>
#include <span>
#include <string>

namespace makeiso {
    // 输出镜像：只按绝对字节偏移写入，长度由自身跟踪
    // 孔洞一律显式写零，同一输入产出同样的字节，满足可度量、可复现的要求
    class Image {
    public:
        // 创建并截断 path 对应的镜像文件
        explicit Image(const std::string& path);

        // 读取当前镜像长度（字节）
        uint64_t Size() const { return m_size; }

        // 把镜像长度至少扩到 bytes，新增部分写零
        void ExtendTo(uint64_t bytes);

        // 写入 bytes 到 offset，自动扩展镜像长度；offset/bytes 可跨孔洞
        void Write(uint64_t offset, std::span<const uint8_t> bytes);

        // 从 offset 起把 bytes 个字节填成 value，自动扩展镜像长度
        void Fill(uint64_t offset, uint64_t bytes, uint8_t value);

        // 把宿主文件整体拷贝到 offset，返回拷贝的字节数
        uint64_t CopyFile(uint64_t offset, const std::string& path);

    private:
        // 移动文件指针到 offset，自动扩展镜像长度
        void Seek(uint64_t offset);

        std::fstream m_file;  // 二进制读写，截断为 0
        uint64_t m_size = 0;  // 当前镜像长度（字节）
    };
}
