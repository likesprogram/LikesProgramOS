/* Iso9660.h
    ISO9660 写入器接口：卷描述符、路径表、目录记录与 Rock Ridge/El Torito 结构
*/

#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace makeiso {
    class Image;  // 输出镜像，见 Image.h

    // 放入 ISO9660 树的一个文件（路径规整后不含前导斜杠）
    struct IsoFile {
        std::string iso_path;   // 规整后的 ISO 内路径（不含前导斜杠），extent 的键
        std::string host_path;  // 宿主文件路径
        uint64_t size = 0;      // 文件字节数
    };

    // ISO9660 卷标识与时间戳
    struct IsoVolume {
        std::string volume_id      = "LIKESPROGRAM";  // 卷标识
        std::string system_id      = "LIKESPROGRAM";  // 系统标识
        std::string publisher_id;                     // 出版者标识
        std::string preparer_id    = "MAKEISO";       // 准备者标识
        std::string application_id = "MAKEISO";       // 应用标识
        uint64_t timestamp = 0;                       // Unix 秒（UTC）；0 表示不指定，卷与目录时间戳写全零以保证可复现
    };

    // 文件在镜像中的连续区间
    struct IsoExtent {
        uint64_t offset = 0;  // 字节偏移
        uint64_t bytes = 0;   // 区间长度（字节）
    };

    // 元数据区布局；文件数据的位置由调用方在 metadata_end 之后安排
    struct IsoLayout {
        uint32_t pvd_lba = 0;             // 主卷描述符
        uint32_t boot_record_lba = 0;     // El Torito 引导记录卷描述符
        uint32_t terminator_lba = 0;      // 卷描述符集结束符
        uint32_t boot_catalog_lba = 0;    // 引导目录扇区（内容由调用方写）
        uint32_t l_path_table_lba = 0;    // 小端路径表
        uint32_t m_path_table_lba = 0;    // 大端路径表
        uint32_t path_table_sectors = 0;  // 路径表占用扇区数
        uint64_t metadata_end = 0;        // 目录区之后的第一个空闲字节，2048 对齐
    };

    // ISO9660 写入器：主卷描述符 + Rock Ridge（NM/PX/TF，主目录带 SP/ER）+ El Torito 引导记录
    // 布局固定：系统区 16 扇区（0..15），PVD/引导记录/结束符 = 16..18，引导目录 = 19，
    // 路径表随后，目录区再随后；不做 Supplementary（Joliet）卷
    class Iso9660 {
    public:
        // 绑定输出镜像与卷标识，建立根目录节点
        Iso9660(Image& image, IsoVolume volume);

        // 释放目录树
        ~Iso9660();

        // 禁止拷贝构造：目录树为独占所有权
        Iso9660(const Iso9660&) = delete;

        // 禁止拷贝赋值：目录树为独占所有权
        Iso9660& operator=(const Iso9660&) = delete;

        // 加入文件，必要时建立中间目录；返回规整后的 ISO 路径；同目录内标识符冲突即报错
        std::string AddFile(std::string_view isoPath, std::string_view hostPath, uint64_t size);

        // 分配卷描述符、路径表与目录区，返回布局
        IsoLayout PlanMetadata();

        // 指定每个文件在镜像中的连续 extent，键为 AddFile 返回的路径
        void SetFileExtents(const std::map<std::string, IsoExtent>& extents);

        // 写卷描述符、路径表、目录记录，并拷贝文件数据
        void Write();

        // 文件列表，按 ISO 路径排序；调用方按此顺序分配 extent
        const std::vector<IsoFile>& Files() const { return m_files; }

        // 镜像总长度（SetFileExtents 之后有效），2048 对齐
        uint64_t TotalBytes() const { return m_total_bytes; }

    private:
        struct Node;  // 目录树节点：ISO 标识符、extent 与父子关系，定义在 Iso9660.cpp

        // 按广度优先给目录编号，重建 m_dirs（根为 1 号）
        void NumberDirectories();

        // 取目录下参与排序的子节点，文件按 "标识符;1" 排序
        std::vector<const Node*> SortedChildren(const Node& dir) const;

        // 计算每个目录的数据长度：记录长度累加后按扇区上取整
        void ComputeDirectorySizes();

        // 返回单份路径表的字节数
        uint32_t PathTableBytes() const;

        // 生成路径表：条目含目录标识符、extent、父目录号，末尾补零到扇区边界
        std::vector<uint8_t> BuildPathTable(bool bigEndian) const;

        // 生成目录的数据区：.、.. 与全部子项记录，按扇区补零
        std::vector<uint8_t> BuildDirectoryData(const Node& dir) const;

        // 生成一条目录记录：长度、extent、数据长度、日期、标志、标识符与系统用区
        std::vector<uint8_t> BuildRecord(const std::string& id, uint32_t extent, uint32_t dataLen, uint8_t flags, const std::vector<uint8_t>& systemUse) const;

        // 生成目录记录的系统用区：SP（仅根 .）、RR、PX、TF，根 . 另加 ER
        std::vector<uint8_t> SystemUseFor(const Node& node, bool self) const;

        // 生成子项记录的系统用区：RR、NM、PX、TF
        std::vector<uint8_t> ChildSystemUse(const Node& child) const;

        // 写 PVD、El Torito 引导记录与卷描述符集结束符三个扇区
        void WriteVolumeDescriptors(uint32_t totalSectors);

        // 把根目录记录写进 PVD 的 156 字节处
        void WriteRootRecord(uint8_t* pvd) const;

        Image& m_image;                              // 输出镜像
        IsoVolume m_volume;                          // 卷标识与时间戳
        std::unique_ptr<Node> m_root;                // 根目录节点
        std::vector<IsoFile> m_files;                // 文件列表，按 ISO 路径排序
        std::map<std::string, Node*> m_file_nodes;   // 规整路径 -> 文件节点，用于回填 extent
        std::vector<Node*> m_dirs;                   // 路径表顺序，m_dirs[0] 为根
        std::map<std::string, IsoExtent> m_extents;  // 已登记的 extent，键为规整路径
        IsoLayout m_layout;                          // PlanMetadata 产出的布局
        uint32_t m_path_table_bytes = 0;             // 单份路径表的字节数
        uint64_t m_metadata_end = 0;                 // 元数据区结束字节偏移
        uint64_t m_total_bytes = 0;                  // 镜像总长度（字节）
        bool m_planned = false;                      // 是否已调用 PlanMetadata
        bool m_extents_set = false;                  // 是否已调用 SetFileExtents
    };
}
