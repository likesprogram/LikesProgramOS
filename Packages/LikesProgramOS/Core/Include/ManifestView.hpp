/* ManifestView.hpp
    内核集清单副本的有界视图：只解析固定头部与产物条目区，不重验签名、不重算摘要

    内核拿到的是 Baleen 验签通过、签名值 64 字节已归零的那一份清单副本；本视图只做结构核对：
    头部固定字段与保留字段、条目区边界、签名区边界、条目类型与路径语法，全部解析都有界
    （条目数 ≤ 32、路径 ≤ 255 字节、清单 ≤ 64 KiB），不做任何按内容长度的分配

    视图里的路径与摘要都指向清单副本内部的字节，不复制、不以空字符结尾；调用方按 pathBytes
    比较与打印。字节布局与语义由 Packages/Common/Include/Manifest.hpp 的常量单点定义，
    本文件与它同源
*/

#pragma once
#include <stdint.h>

#include <Manifest.hpp>

namespace Kernel {
    // 清单产物条目视图：指向清单副本内部，字段与字节表一一对应
    struct ManifestEntry {
        uint8_t type;            // 产物类型：1 = .os，2 = .osm，3 = Modules.list
        uint32_t pathBytes;      // 相对路径字节数，取值 1..255
        uint64_t dataBytes;      // 产物字节数，不得为 0
        const uint8_t* path;     // 相对集目录的路径起点，不含结尾空字符
        const uint8_t* digest;   // 32 字节 SHA-256 摘要起点
    };

    // 清单视图：条目数固定上界，内容指向交接的清单副本
    struct ManifestView {
        const uint8_t* bytes = nullptr;   // 清单副本起点
        uint32_t length = 0;              // 清单字节数
        uint32_t entryCount = 0;          // 产物条目数，取值 2..32
        uint8_t buildId[32] = {};         // 集标识，全零禁止
        uint64_t securityVersion = 0;     // 安全版本，只作事实播报
        ManifestEntry entries[KernelSet::ManifestLayout::kEntryMaxCount] = {};   // 条目，前 entryCount 条有效
    };

    // 有界解析清单副本：头部、保留字段、条目区与签名区边界、条目类型与路径语法、条目唯一性
    // 成功返回空指针；失败返回原因文本（不打印、不停机，编号由调用方按失败点选）
    const char* ParseManifest(const uint8_t* bytes, uint32_t length, ManifestView& view);

    // 按字节查找产物条目：路径逐字节相等才命中；找不到返回空
    const ManifestEntry* FindManifestEntry(const ManifestView& view, const uint8_t* path, uint32_t pathBytes);

    // 相对路径语法校验：非空、不超上限、不以 '/' 开头、分量非空且不是 "." 或 ".."、不含 NUL
    bool PathValid(const uint8_t* path, uint32_t pathBytes);

    // 产物路径是否落在 Modules/ 下且文件名带版本（最后一段至少有一个 '.'）
    bool ModulePathValid(const uint8_t* path, uint32_t pathBytes);
}
