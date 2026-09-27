#include "Iso9660.h"

#include "Image.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <stdexcept>

namespace makeiso {
    namespace {
        constexpr uint32_t kBlock = 2048;
        constexpr uint32_t kPvdLba = 16;
        constexpr uint32_t kBootRecordLba = 17;
        constexpr uint32_t kTerminatorLba = 18;
        constexpr uint32_t kBootCatalogLba = 19;
        constexpr uint32_t kFirstPathTableLba = 20;
        constexpr std::size_t kMaxIdentifier = 31;

        uint32_t AlignUp(uint32_t value, uint32_t align) { return (value + align - 1) / align * align; }

        uint64_t AlignUp64(uint64_t value, uint64_t align) { return (value + align - 1) / align * align; }

        void Put16Le(uint8_t* p, uint16_t v) {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>(v >> 8);
        }

        void Put16Be(uint8_t* p, uint16_t v) {
            p[0] = static_cast<uint8_t>(v >> 8);
            p[1] = static_cast<uint8_t>(v & 0xFF);
        }

        void Put32Le(uint8_t* p, uint32_t v) {
            for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }

        void Put32Be(uint8_t* p, uint32_t v) {
            for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * (3 - i))) & 0xFF);
        }

        void PutBoth16(uint8_t* p, uint16_t v) {
            Put16Le(p, v);
            Put16Be(p + 2, v);
        }

        void PutBoth32(uint8_t* p, uint32_t v) {
            Put32Le(p, v);
            Put32Be(p + 4, v);
        }

        bool ToUtc(uint64_t timestamp, std::tm& out) {
            const std::time_t t = static_cast<std::time_t>(timestamp);
            return gmtime_r(&t, &out) != nullptr;
        }

        // 17 字节卷描述符日期：'YYYYMMDDHHMMSSCC' + 时区字节（0 = UTC）。0 = 不指定，写全零。
        void PutVolumeDate(uint8_t* p, uint64_t timestamp) {
            std::memset(p, 0, 17);
            if (timestamp == 0) return;
            std::tm tm{};
            if (!ToUtc(timestamp, tm)) return;
            // 先夹到合法范围，保证 17 字节字段刚好放得下。
            const int year = std::clamp(tm.tm_year + 1900, 0, 9999);
            const int month = std::clamp(tm.tm_mon + 1, 1, 12);
            const int day = std::clamp(tm.tm_mday, 1, 31);
            const int hour = std::clamp(tm.tm_hour, 0, 23);
            const int minute = std::clamp(tm.tm_min, 0, 59);
            const int second = std::clamp(tm.tm_sec, 0, 59);
            char buf[17];
            std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d00", year, month, day, hour, minute, second);
            std::memcpy(p, buf, 16);
            p[16] = 0;
        }

        // 7 字节目录记录日期：年（自 1900）、月、日、时、分、秒、时区（0 = UTC）。
        void PutRecordDate(uint8_t* p, uint64_t timestamp) {
            std::memset(p, 0, 7);
            if (timestamp == 0) return;
            std::tm tm{};
            if (!ToUtc(timestamp, tm)) return;
            p[0] = static_cast<uint8_t>(tm.tm_year);
            p[1] = static_cast<uint8_t>(tm.tm_mon + 1);
            p[2] = static_cast<uint8_t>(tm.tm_mday);
            p[3] = static_cast<uint8_t>(tm.tm_hour);
            p[4] = static_cast<uint8_t>(tm.tm_min);
            p[5] = static_cast<uint8_t>(tm.tm_sec);
            p[6] = 0;
        }

        std::string SanitizeChars(std::string_view name) {
            std::string out;
            out.reserve(name.size());
            for (const char raw : name) {
                const auto c = static_cast<unsigned char>(raw);
                if (c >= 'a' && c <= 'z') out.push_back(static_cast<char>(c - 'a' + 'A'));
                else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out.push_back(static_cast<char>(c));
                else out.push_back('_');
            }
            return out;
        }

        // ISO 标识符：目录不超 31 字符；文件按 8.3 收敛，真实名字由 Rock Ridge 的 NM 保留。
        std::string IsoIdentifier(std::string_view name, bool is_dir) {
            if (is_dir) {
                std::string id = SanitizeChars(name);
                if (id.empty()) id = "_";
                if (id.size() > kMaxIdentifier) id.resize(kMaxIdentifier);
                return id;
            }
            std::string_view base = name;
            std::string_view ext;
            const auto dot = name.rfind('.');
            if (dot != std::string_view::npos && dot != 0) {
                base = name.substr(0, dot);
                ext = name.substr(dot + 1);
            }
            std::string b = SanitizeChars(base);
            std::string e = SanitizeChars(ext);
            if (b.empty()) b = "_";
            if (b.size() > 8) b.resize(8);
            if (e.size() > 3) e.resize(3);
            return e.empty() ? b : b + "." + e;
        }

        std::string MakeUnique(std::set<std::string>& used, std::string id) {
            if (used.insert(id).second) return id;
            for (int n = 1; n < 100000; ++n) {
                const std::string suffix = std::to_string(n);
                std::string candidate = id;
                if (candidate.size() + suffix.size() > kMaxIdentifier) candidate.resize(kMaxIdentifier - suffix.size());
                candidate += suffix;
                if (used.insert(candidate).second) return candidate;
            }
            throw std::runtime_error("ISO 标识符冲突无法消解：" + id);
        }

        void AppendRr(std::vector<uint8_t>& out, const char* signature, std::span<const uint8_t> data) {
            out.push_back(static_cast<uint8_t>(signature[0]));
            out.push_back(static_cast<uint8_t>(signature[1]));
            out.push_back(static_cast<uint8_t>(data.size() + 4));
            out.push_back(1);
            out.insert(out.end(), data.begin(), data.end());
        }

        void AppendRrPx(std::vector<uint8_t>& out, uint32_t mode, uint32_t links) {
            // RRIP_1991A 的 PX：mode、links、uid、gid，各 8 字节双端序，共 32 字节数据。
            uint8_t data[32];
            PutBoth32(data + 0, mode);
            PutBoth32(data + 8, links);
            PutBoth32(data + 16, 0);  // uid
            PutBoth32(data + 24, 0);  // gid
            AppendRr(out, "PX", data);
        }

        void AppendRrTf(std::vector<uint8_t>& out, uint64_t timestamp) {
            uint8_t data[8] = {0x02, 0, 0, 0, 0, 0, 0, 0};  // 只带修改时间
            PutRecordDate(data + 1, timestamp);
            AppendRr(out, "TF", data);
        }

        void AppendRrNm(std::vector<uint8_t>& out, std::string_view name) {
            std::vector<uint8_t> data;
            data.reserve(name.size() + 1);
            data.push_back(0);  // 完整名字，无续接、非 . / ..
            data.insert(data.end(), name.begin(), name.end());
            AppendRr(out, "NM", data);
        }

        void AppendRrEr(std::vector<uint8_t>& out) {
            // ER 的标识串是读取方识别 RRIP 的依据；描述与来源只是说明文字，取短值以便整条记录
            // 留在 255 字节以内（本写入器不生成 CE 续接项）。
            static constexpr std::string_view kId = "RRIP_1991A";
            static constexpr std::string_view kDesc = "THE ROCK RIDGE INTERCHANGE PROTOCOL";
            static constexpr std::string_view kSource = "MAKEISO";
            // ER：标识长度、描述长度、来源长度、扩展版本四个字段在前，三个字符串依次在后。
            std::vector<uint8_t> data;
            data.push_back(static_cast<uint8_t>(kId.size()));
            data.push_back(static_cast<uint8_t>(kDesc.size()));
            data.push_back(static_cast<uint8_t>(kSource.size()));
            data.push_back(1);  // 扩展版本
            data.insert(data.end(), kId.begin(), kId.end());
            data.insert(data.end(), kDesc.begin(), kDesc.end());
            data.insert(data.end(), kSource.begin(), kSource.end());
            AppendRr(out, "ER", data);
        }

        // RR 项的标志位：如实声明记录里带了哪些 RRIP 字段，读取方按它决定是否采用 NM 等项。
        constexpr uint8_t kRrFlagPx = 0x01;
        constexpr uint8_t kRrFlagNm = 0x08;
        constexpr uint8_t kRrFlagTf = 0x80;

        std::size_t RecordLength(std::size_t id_len, std::size_t su_len) {
            std::size_t len = 33 + id_len + (id_len % 2 == 0 ? 1 : 0) + su_len;
            if (len % 2 != 0) ++len;
            return len;
        }

        void PutText(uint8_t* p, std::size_t size, std::string_view text) {
            std::memset(p, ' ', size);
            const std::size_t n = std::min(size, text.size());
            std::memcpy(p, text.data(), n);
        }
    }

    struct Iso9660::Node {
        std::string name;    // 真实名字，写进 Rock Ridge 的 NM
        std::string iso_id;  // ISO 标识符；文件在记录里补 ";1"
        bool is_dir = false;
        std::string host_path;
        uint64_t size = 0;      // 文件字节数
        uint32_t extent = 0;    // 目录与文件共用
        uint32_t data_len = 0;  // 目录数据长度（扇区整数倍）
        uint16_t dir_number = 0;
        uint16_t parent_number = 0;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        std::set<std::string> used_ids;
    };

    Iso9660::Iso9660(Image& image, IsoVolume volume) : m_image(image), m_volume(std::move(volume)) {
        m_root = std::make_unique<Node>();
        m_root->is_dir = true;
        m_root->dir_number = 1;
        m_root->parent_number = 1;
        m_dirs.push_back(m_root.get());
    }

    Iso9660::~Iso9660() = default;

    std::string Iso9660::AddFile(std::string_view iso_path, std::string_view host_path, uint64_t size) {
        std::vector<std::string> components;
        std::string current;
        for (const char c : iso_path) {
            if (c == '/') {
                if (!current.empty()) {
                    components.push_back(current);
                    current.clear();
                }
            } else current.push_back(c);
        }
        if (!current.empty()) components.push_back(current);
        if (components.empty()) throw std::runtime_error("ISO 路径为空");
        for (const std::string& part : components) if (part == "." || part == "..") throw std::runtime_error("ISO 路径不接受 . 或 ..：" + std::string(iso_path));

        Node* dir = m_root.get();
        for (std::size_t i = 0; i + 1 < components.size(); ++i) {
            Node* next = nullptr;
            for (const auto& child : dir->children) {
                if (child->is_dir && child->name == components[i]) {
                    next = child.get();
                    break;
                }
            }
            if (next == nullptr) {
                auto node = std::make_unique<Node>();
                node->name = components[i];
                node->is_dir = true;
                node->parent = dir;
                node->iso_id = MakeUnique(dir->used_ids, IsoIdentifier(node->name, true));
                next = node.get();
                dir->children.push_back(std::move(node));
            }
            dir = next;
        }

        for (const auto& child : dir->children) if (child->name == components.back()) throw std::runtime_error("ISO 路径重复：" + std::string(iso_path));

        auto node = std::make_unique<Node>();
        node->name = components.back();
        node->host_path = std::string(host_path);
        node->size = size;
        node->parent = dir;
        node->iso_id = MakeUnique(dir->used_ids, IsoIdentifier(node->name, false));
        Node* file_node = node.get();
        dir->children.push_back(std::move(node));

        std::string normalized;
        for (const std::string& part : components) {
            if (!normalized.empty()) normalized.push_back('/');
            normalized += part;
        }
        m_files.push_back(IsoFile{normalized, std::string(host_path), size});
        m_file_nodes[normalized] = file_node;
        return normalized;
    }

    IsoLayout Iso9660::PlanMetadata() {
        if (m_planned) throw std::logic_error("PlanMetadata 只能调用一次");
        std::sort(m_files.begin(), m_files.end(), [](const IsoFile& a, const IsoFile& b) { return a.iso_path < b.iso_path; });

        NumberDirectories();
        ComputeDirectorySizes();

        m_path_table_bytes = PathTableBytes();
        const uint32_t path_sectors = AlignUp(m_path_table_bytes, kBlock) / kBlock;

        IsoLayout layout;
        layout.pvd_lba = kPvdLba;
        layout.boot_record_lba = kBootRecordLba;
        layout.terminator_lba = kTerminatorLba;
        layout.boot_catalog_lba = kBootCatalogLba;
        layout.l_path_table_lba = kFirstPathTableLba;
        layout.m_path_table_lba = kFirstPathTableLba + path_sectors;
        layout.path_table_sectors = path_sectors;

        uint32_t next = layout.m_path_table_lba + path_sectors;
        for (Node* dir : m_dirs) {
            dir->extent = next;
            next += dir->data_len / kBlock;
        }
        m_metadata_end = static_cast<uint64_t>(next) * kBlock;
        layout.metadata_end = m_metadata_end;

        m_layout = layout;
        m_planned = true;
        return layout;
    }

    void Iso9660::SetFileExtents(const std::map<std::string, IsoExtent>& extents) {
        if (!m_planned) throw std::logic_error("SetFileExtents 之前必须先 PlanMetadata");
        m_extents = extents;

        std::vector<std::pair<uint64_t, uint64_t>> intervals;
        uint64_t end = m_metadata_end;
        for (const IsoFile& file : m_files) {
            const auto it = extents.find(file.iso_path);
            if (it == extents.end()) throw std::runtime_error("缺少 extent：" + file.iso_path);
            const IsoExtent& extent = it->second;
            if (extent.bytes < file.size) throw std::runtime_error("extent 小于文件长度：" + file.iso_path);
            if (extent.offset % kBlock != 0) throw std::runtime_error("extent 未按 2048 对齐：" + file.iso_path);
            if (extent.offset < m_metadata_end) throw std::runtime_error("extent 落在元数据区内：" + file.iso_path);
            if (extent.offset / kBlock > 0xFFFFFFFFull) throw std::runtime_error("extent 超出 ISO9660 的 32 位 LBA 范围：" + file.iso_path);
            m_file_nodes.at(file.iso_path)->extent = static_cast<uint32_t>(extent.offset / kBlock);
            if (file.size > 0) intervals.emplace_back(extent.offset, extent.offset + extent.bytes);
            end = std::max(end, AlignUp64(extent.offset + extent.bytes, kBlock));
        }
        std::sort(intervals.begin(), intervals.end());
        for (std::size_t i = 1; i < intervals.size(); ++i) if (intervals[i].first < intervals[i - 1].second) throw std::runtime_error("文件 extent 互相重叠");
        m_total_bytes = AlignUp64(end, kBlock);
        m_extents_set = true;
    }

    void Iso9660::NumberDirectories() {
        m_dirs.clear();
        m_dirs.push_back(m_root.get());
        m_root->dir_number = 1;
        m_root->parent_number = 1;

        uint16_t next_number = 2;
        std::vector<Node*> level{m_root.get()};
        while (!level.empty()) {
            std::vector<Node*> next_level;
            for (Node* dir : level) {
                for (const Node* child : SortedChildren(*dir)) {
                    if (!child->is_dir) continue;
                    auto* node = const_cast<Node*>(child);
                    node->dir_number = next_number++;
                    node->parent_number = dir->dir_number;
                    m_dirs.push_back(node);
                    next_level.push_back(node);
                }
            }
            level = std::move(next_level);
        }
        if (m_dirs.size() > 0xFFFF) throw std::runtime_error("目录数量超过路径表上限");
    }

    std::vector<const Iso9660::Node*> Iso9660::SortedChildren(const Node& dir) const {
        std::vector<const Node*> children;
        children.reserve(dir.children.size());
        for (const auto& child : dir.children) children.push_back(child.get());
        std::sort(children.begin(), children.end(), [](const Node* a, const Node* b) {
            const std::string a_id = a->is_dir ? a->iso_id : a->iso_id + ";1";
            const std::string b_id = b->is_dir ? b->iso_id : b->iso_id + ";1";
            return a_id < b_id;
        });
        return children;
    }

    void Iso9660::ComputeDirectorySizes() {
        for (Node* dir : m_dirs) {
            uint64_t consumed = 0;
            const auto account = [&consumed](std::size_t len) {
                if (consumed % kBlock + len > kBlock) consumed = AlignUp64(consumed, kBlock);
                consumed += len;
            };

            account(RecordLength(1, SystemUseFor(*dir, true).size()));
            account(RecordLength(1, SystemUseFor(*dir, false).size()));
            for (const Node* child : SortedChildren(*dir)) {
                const std::size_t id_len = (child->is_dir ? child->iso_id : child->iso_id + ";1").size();
                account(RecordLength(id_len, ChildSystemUse(*child).size()));
            }
            dir->data_len = static_cast<uint32_t>(AlignUp64(consumed, kBlock));
        }
    }

    uint32_t Iso9660::PathTableBytes() const {
        uint32_t total = 0;
        for (const Node* dir : m_dirs) {
            const std::size_t id_len = (dir == m_root.get()) ? 1 : dir->iso_id.size();
            total += static_cast<uint32_t>(8 + id_len + (id_len % 2 != 0 ? 1 : 0));
        }
        return total;
    }

    std::vector<uint8_t> Iso9660::BuildPathTable(bool big_endian) const {
        std::vector<uint8_t> table;
        table.reserve(m_path_table_bytes);
        for (const Node* dir : m_dirs) {
            const bool is_root = dir == m_root.get();
            const std::string id = is_root ? std::string(1, '\0') : dir->iso_id;
            table.push_back(static_cast<uint8_t>(id.size()));
            table.push_back(0);
            uint8_t buf[4];
            if (big_endian) {
                Put32Be(buf, dir->extent);
                table.insert(table.end(), buf, buf + 4);
                Put16Be(buf, dir->parent_number);
                table.insert(table.end(), buf, buf + 2);
            } else {
                Put32Le(buf, dir->extent);
                table.insert(table.end(), buf, buf + 4);
                Put16Le(buf, dir->parent_number);
                table.insert(table.end(), buf, buf + 2);
            }
            table.insert(table.end(), id.begin(), id.end());
            if (id.size() % 2 != 0) table.push_back(0);
        }
        table.resize(AlignUp64(table.size(), kBlock), 0);
        return table;
    }

    std::vector<uint8_t> Iso9660::SystemUseFor(const Node& node, bool self) const {
        std::vector<uint8_t> su;
        const bool is_root_dot = self && &node == m_root.get();
        if (is_root_dot) {
            // SP 必须是主目录 "." 的第一个 SUSP 项；0xBE 0xEF 是 SUSP 的识别字节。
            static constexpr std::array<uint8_t, 7> kSp{'S', 'P', 7, 1, 0xBE, 0xEF, 0};
            su.insert(su.end(), kSp.begin(), kSp.end());
        }
        // RR 项的标志位要如实声明本记录里带了哪些 RRIP 字段，读取方按它决定是否采用。
        const uint8_t rr_flags = kRrFlagPx | kRrFlagTf;
        AppendRr(su, "RR", std::span<const uint8_t>(&rr_flags, 1));
        AppendRrPx(su, node.is_dir ? 0040755u : 0100444u, node.is_dir ? 2u : 1u);
        AppendRrTf(su, m_volume.timestamp);
        if (is_root_dot) AppendRrEr(su);
        return su;
    }

    std::vector<uint8_t> Iso9660::ChildSystemUse(const Node& child) const {
        std::vector<uint8_t> su;
        const uint8_t rr_flags = kRrFlagPx | kRrFlagNm | kRrFlagTf;
        AppendRr(su, "RR", std::span<const uint8_t>(&rr_flags, 1));
        AppendRrNm(su, child.name);
        AppendRrPx(su, child.is_dir ? 0040755u : 0100444u, child.is_dir ? 2u : 1u);
        AppendRrTf(su, m_volume.timestamp);
        return su;
    }

    std::vector<uint8_t> Iso9660::BuildRecord(const std::string& id, uint32_t extent, uint32_t data_len, uint8_t flags, const std::vector<uint8_t>& system_use) const {
        const std::size_t id_len = id.size();
        const std::size_t len = RecordLength(id_len, system_use.size());
        if (len > 255) throw std::runtime_error("目录记录超过 255 字节，需要 CE 续接项：" + id);
        std::vector<uint8_t> record(len, 0);
        record[0] = static_cast<uint8_t>(len);
        record[1] = 0;
        PutBoth32(record.data() + 2, extent);
        PutBoth32(record.data() + 10, data_len);
        PutRecordDate(record.data() + 18, m_volume.timestamp);
        record[25] = flags;
        PutBoth16(record.data() + 28, 1);
        record[32] = static_cast<uint8_t>(id_len);
        std::memcpy(record.data() + 33, id.data(), id_len);
        const std::size_t su_offset = 33 + id_len + (id_len % 2 == 0 ? 1 : 0);
        std::memcpy(record.data() + su_offset, system_use.data(), system_use.size());
        return record;
    }

    std::vector<uint8_t> Iso9660::BuildDirectoryData(const Node& dir) const {
        std::vector<std::vector<uint8_t>> records;
        records.push_back(BuildRecord(std::string(1, '\0'), dir.extent, dir.data_len, 0x02, SystemUseFor(dir, true)));
        const Node& parent = dir.parent != nullptr ? *dir.parent : dir;
        records.push_back(BuildRecord(std::string(1, '\1'), parent.extent, parent.data_len, 0x02, SystemUseFor(dir, false)));
        for (const Node* child : SortedChildren(dir)) {
            const std::string id = child->is_dir ? child->iso_id : child->iso_id + ";1";
            const uint32_t data_len = static_cast<uint32_t>(child->is_dir ? child->data_len : child->size);
            records.push_back(BuildRecord(id, child->extent, data_len, static_cast<uint8_t>(child->is_dir ? 0x02 : 0x00), ChildSystemUse(*child)));
        }

        std::vector<uint8_t> data;
        data.reserve(dir.data_len);
        for (const auto& record : records) {
            if (data.size() % kBlock + record.size() > kBlock) data.resize(static_cast<std::size_t>(AlignUp64(data.size(), kBlock)), 0);
            data.insert(data.end(), record.begin(), record.end());
        }
        data.resize(dir.data_len, 0);
        return data;
    }

    void Iso9660::WriteRootRecord(uint8_t* pvd) const {
        uint8_t* p = pvd + 156;
        p[0] = 34;
        p[1] = 0;
        PutBoth32(p + 2, m_root->extent);
        PutBoth32(p + 10, m_root->data_len);
        PutRecordDate(p + 18, m_volume.timestamp);
        p[25] = 0x02;
        PutBoth16(p + 28, 1);
        p[32] = 1;
        p[33] = 0x00;
    }

    void Iso9660::WriteVolumeDescriptors(uint32_t total_sectors) {
        std::vector<uint8_t> pvd(kBlock, 0);    pvd[0] = 1;
        std::memcpy(pvd.data() + 1, "CD001", 5);
        pvd[6] = 1;
        PutText(pvd.data() + 8, 32, m_volume.system_id);
        PutText(pvd.data() + 40, 32, SanitizeChars(m_volume.volume_id));
        PutBoth32(pvd.data() + 80, total_sectors);
        PutBoth16(pvd.data() + 120, 1);  // 卷集大小
        PutBoth16(pvd.data() + 124, 1);  // 卷顺序号
        PutBoth16(pvd.data() + 128, kBlock);
        PutBoth32(pvd.data() + 132, m_path_table_bytes);
        Put32Le(pvd.data() + 140, m_layout.l_path_table_lba);
        Put32Le(pvd.data() + 144, 0);
        Put32Be(pvd.data() + 148, m_layout.m_path_table_lba);
        Put32Be(pvd.data() + 152, 0);
        WriteRootRecord(pvd.data());
        PutText(pvd.data() + 190, 128, m_volume.volume_id);
        PutText(pvd.data() + 318, 128, m_volume.publisher_id);
        PutText(pvd.data() + 446, 128, m_volume.preparer_id);
        PutText(pvd.data() + 574, 128, m_volume.application_id);
        PutText(pvd.data() + 702, 37, "");   // 版权文件：未指定
        PutText(pvd.data() + 739, 37, "");   // 摘要文件：未指定
        PutText(pvd.data() + 776, 37, "");   // 书目文件：未指定
        PutVolumeDate(pvd.data() + 813, m_volume.timestamp);
        PutVolumeDate(pvd.data() + 830, m_volume.timestamp);
        PutVolumeDate(pvd.data() + 847, 0);  // 失效日期：不指定
        PutVolumeDate(pvd.data() + 864, 0);  // 生效日期：不指定
        pvd[881] = 1;                        // 文件结构版本
        m_image.Write(static_cast<uint64_t>(kPvdLba) * kBlock, pvd);

        std::vector<uint8_t> boot_record(kBlock, 0);
        boot_record[0] = 0;
        std::memcpy(boot_record.data() + 1, "CD001", 5);
        boot_record[6] = 1;
        std::memcpy(boot_record.data() + 7, "EL TORITO SPECIFICATION", 23);
        PutText(boot_record.data() + 39, 32, m_volume.volume_id);
        Put32Le(boot_record.data() + 71, m_layout.boot_catalog_lba);
        m_image.Write(static_cast<uint64_t>(kBootRecordLba) * kBlock, boot_record);

        std::vector<uint8_t> terminator(kBlock, 0);
        terminator[0] = 255;
        std::memcpy(terminator.data() + 1, "CD001", 5);
        terminator[6] = 1;
        m_image.Write(static_cast<uint64_t>(kTerminatorLba) * kBlock, terminator);
    }

    void Iso9660::Write() {
        if (!m_extents_set) throw std::logic_error("Write 之前必须先 SetFileExtents");
        if (m_total_bytes / kBlock > 0xFFFFFFFFull) throw std::runtime_error("镜像超过 ISO9660 的 32 位卷空间上限");
        WriteVolumeDescriptors(static_cast<uint32_t>(m_total_bytes / kBlock));

        const std::vector<uint8_t> l_table = BuildPathTable(false);
        const std::vector<uint8_t> m_table = BuildPathTable(true);
        m_image.Write(static_cast<uint64_t>(m_layout.l_path_table_lba) * kBlock, l_table);
        m_image.Write(static_cast<uint64_t>(m_layout.m_path_table_lba) * kBlock, m_table);

        for (const Node* dir : m_dirs) {
            const std::vector<uint8_t> data = BuildDirectoryData(*dir);
            m_image.Write(static_cast<uint64_t>(dir->extent) * kBlock, data);
        }

        for (const IsoFile& file : m_files) {
            if (file.size == 0) continue;
            const IsoExtent& extent = m_extents.at(file.iso_path);
            const uint64_t copied = m_image.CopyFile(extent.offset, file.host_path);
            if (copied != file.size) throw std::runtime_error("文件长度与登记不一致：" + file.iso_path);
        }
        m_image.ExtendTo(m_total_bytes);
    }
}
