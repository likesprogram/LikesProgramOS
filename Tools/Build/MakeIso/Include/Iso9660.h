#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace makeiso {
    class Image;

    // 放入 ISO9660 树的一个文件（路径规整后不含前导斜杠）。
    struct IsoFile {
        std::string iso_path;
        std::string host_path;
        uint64_t size = 0;
    };

    struct IsoVolume {
        std::string volume_id      = "LIKESPROGRAM";
        std::string system_id      = "LIKESPROGRAM";
        std::string publisher_id;
        std::string preparer_id    = "MAKEISO";
        std::string application_id = "MAKEISO";
        // Unix 秒（UTC）。0 = 不指定：卷与目录时间戳写全零，保证同一输入产出同样的字节。
        uint64_t timestamp = 0;
    };

    struct IsoExtent {
        uint64_t offset = 0;
        uint64_t bytes = 0;
    };

    // 元数据区布局。文件数据的位置由调用方在 metadata_end 之后安排。
    struct IsoLayout {
        uint32_t pvd_lba = 0;             // 主卷描述符
        uint32_t boot_record_lba = 0;     // El Torito 引导记录卷描述符
        uint32_t terminator_lba = 0;      // 卷描述符集结束符
        uint32_t boot_catalog_lba = 0;    // 引导目录扇区（内容由调用方写）
        uint32_t l_path_table_lba = 0;    // 小端路径表
        uint32_t m_path_table_lba = 0;    // 大端路径表
        uint32_t path_table_sectors = 0;
        uint64_t metadata_end = 0;        // 目录区之后的第一个空闲字节，2048 对齐
    };

    // ISO9660 写入器：主卷描述符 + Rock Ridge（NM/PX/TF，主目录带 SP/ER）+ El Torito 引导记录。
    // 布局固定：系统区 16 扇区（0..15），PVD/引导记录/结束符 = 16..18，引导目录 = 19，
    // 路径表随后，目录区再随后；不做 Supplementary（Joliet）卷。
    class Iso9660 {
    public:
        Iso9660(Image& image, IsoVolume volume);
        ~Iso9660();

        Iso9660(const Iso9660&) = delete;
        Iso9660& operator=(const Iso9660&) = delete;

        // 加入文件，必要时建立中间目录；返回规整后的 ISO 路径。同目录内标识符冲突即报错。
        std::string AddFile(std::string_view iso_path, std::string_view host_path, uint64_t size);

        // 分配卷描述符、路径表与目录区，返回布局。
        IsoLayout PlanMetadata();

        // 指定每个文件在镜像中的连续 extent，键为 AddFile 返回的路径。
        void SetFileExtents(const std::map<std::string, IsoExtent>& extents);

        // 写卷描述符、路径表、目录记录，并拷贝文件数据。
        void Write();

        // 文件列表，按 ISO 路径排序；调用方按此顺序分配 extent。
        const std::vector<IsoFile>& files() const { return m_files; }

        // 镜像总长度（SetFileExtents 之后有效），2048 对齐。
        uint64_t total_bytes() const { return m_total_bytes; }

    private:
        struct Node;

        void NumberDirectories();
        std::vector<const Node*> SortedChildren(const Node& dir) const;
        void ComputeDirectorySizes();
        uint32_t PathTableBytes() const;
        std::vector<uint8_t> BuildPathTable(bool big_endian) const;
        std::vector<uint8_t> BuildDirectoryData(const Node& dir) const;
        std::vector<uint8_t> BuildRecord(const std::string& id, uint32_t extent, uint32_t data_len, uint8_t flags, const std::vector<uint8_t>& system_use) const;
        std::vector<uint8_t> SystemUseFor(const Node& node, bool self) const;
        std::vector<uint8_t> ChildSystemUse(const Node& child) const;
        void WriteVolumeDescriptors(uint32_t total_sectors);
        void WriteRootRecord(uint8_t* pvd) const;

        Image& m_image;
        IsoVolume m_volume;
        std::unique_ptr<Node> m_root;
        std::vector<IsoFile> m_files;
        std::map<std::string, Node*> m_file_nodes;  // 规整路径 -> 文件节点，用于回填 extent
        std::vector<Node*> m_dirs;  // 路径表顺序，m_dirs[0] 为根
        std::map<std::string, IsoExtent> m_extents;
        IsoLayout m_layout;
        uint32_t m_path_table_bytes = 0;
        uint64_t m_metadata_end = 0;
        uint64_t m_total_bytes = 0;
        bool m_planned = false;
        bool m_extents_set = false;
    };
}
