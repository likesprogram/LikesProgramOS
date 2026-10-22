/* Manifest.hpp
    内核集清单（LPMS）的结构定义：字节表偏移、固定值与尺寸约束

    字节表偏移、固定值与尺寸约束由本文件定义；宿主侧工具按本文件生成与签名，
    引导选择器按同一份偏移解析与验签，两侧共用这一处定义
*/

#pragma once
#include <stdint.h>

namespace KernelSet {
    namespace ManifestLayout {
        // 格式标记，字节顺序为 L P M S
        constexpr uint32_t kMagic = 0x534D504Cu;
        // 当前版本
        constexpr uint16_t kVersion = 1;
        // 头部固定长度
        constexpr uint16_t kHeaderBytes = 0x40;
        // 签名区固定长度：算法 1 + 密钥编号 1 + 保留 2 + 签名值 64
        constexpr uint32_t kSignatureAreaBytes = 68;
        // 签名值长度（Ed25519）
        constexpr uint32_t kSignatureValueBytes = 64;
        // 单条产物条目的固定部分长度
        constexpr uint32_t kEntryFixedBytes = 44;
        // 产物相对路径上限，字节
        constexpr uint32_t kEntryPathMaxBytes = 255;
        // 产物条目数上限
        constexpr uint32_t kEntryMaxCount = 32;
        // 清单总长度上限
        constexpr uint32_t kManifestMaxBytes = 0x10000;
        // 签名算法取值：1 为 Ed25519；其他值拒绝，没有“不验证”取值
        constexpr uint8_t kAlgorithmEd25519 = 1;
        // 产物类型取值：1 为 .os，2 为 .osm，3 为 Modules.list（内核侧书写的模块加载列表，非装载载荷）
        constexpr uint8_t kTypeOs = 1;
        constexpr uint8_t kTypeOsm = 2;
        constexpr uint8_t kTypeBootList = 3;

        // 头部与条目区字段偏移，相对清单起点
        constexpr uint32_t kFieldMagic = 0x000;
        constexpr uint32_t kFieldVersion = 0x004;
        constexpr uint32_t kFieldHeaderBytes = 0x006;
        constexpr uint32_t kFieldBytes = 0x008;
        constexpr uint32_t kFieldFlags = 0x00C;
        constexpr uint32_t kFieldBuildId = 0x010;
        constexpr uint32_t kBuildIdBytes = 32;
        constexpr uint32_t kFieldSecurityVersion = 0x030;
        constexpr uint32_t kFieldArtifactCount = 0x038;
        constexpr uint32_t kFieldReserved16 = 0x03A;
        constexpr uint32_t kFieldReserved32 = 0x03C;
        constexpr uint32_t kFieldArtifacts = 0x040;

        // 产物条目内偏移，相对条目起点
        constexpr uint32_t kEntryType = 0x00;
        constexpr uint32_t kEntryPathBytes = 0x01;
        constexpr uint32_t kEntryReserved = 0x03;
        constexpr uint32_t kEntryDataBytes = 0x04;
        constexpr uint32_t kEntryDigest = 0x0C;
        constexpr uint32_t kEntryDigestBytes = 32;
        constexpr uint32_t kEntryPath = 0x2C;

        // 签名区字段偏移，相对签名区起点；签名区紧随最后一个条目
        constexpr uint32_t kSignatureAlgorithm = 0x00;
        constexpr uint32_t kSignatureKeyId = 0x01;
        constexpr uint32_t kSignatureReserved = 0x02;
        constexpr uint32_t kSignatureValue = 0x04;
    }
}
