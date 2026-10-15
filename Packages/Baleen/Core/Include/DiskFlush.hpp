/* DiskFlush.hpp
    写缓存刷新：把启动介质所在控制器的写缓存刷到介质

    交权块里没有缓存刷新原语（BIOS 的 INT 13h 不提供），因此这条路径直接对控制器下发命令，
    按平台取一条：
      1. AHCI（现代平台与 q35 默认路径）：扫 PCI 找类 01 / 子类 06 / 接口 01 的控制器，
         读 BAR5 取得 ABAR，对每个"接有 ATA 设备且命令槽空闲"的端口下发 ATA FLUSH CACHE（0xE7）
      2. 传统 IDE（PIIX 一类，Bochs 与老机器）：先扫 PCI 确认存在类 01 / 子类 01 的控制器，
         再用 IDENTIFY DEVICE 甄别 ATA 盘（ATAPI 会以 ERR 拒绝它），对在位者下发同一条命令；
         没有 IDE 控制器就绝不碰那些端口
    两条路径都跳过 ATAPI 与没有设备的端口，因此光盘、USB、NVMe 只会得到 Unsupported，
    维持"写成功 + 读回校验"的边界；等待一律有界，绝不无限等
    端口 I/O 与绝对地址访问经 Platform 的原语，无运行库依赖；结论只用于诊断与 Flush 的返回值
*/

#pragma once
#include <stdint.h>

namespace Baleen {
    namespace Core {
        namespace DiskFlush {
            // 可刷新的路径
            enum class Path : uint8_t {
                None,       // 没有可刷新的路径
                Ahci,       // AHCI 控制器上的 ATA 端口
                LegacyAta,  // 传统 IDE 端口上的 ATA 盘
            };

            // 刷新结论
            enum class Result : uint8_t {
                Unsupported,   // 没有可刷新的路径
                Succeeded,     // 已下发并完成刷新
                Failed,        // 路径在，但命令出错或等待超时
            };

            // 路径的短文本，供诊断行使用
            const char* PathText(Path path);
            // 结论的短文本，供诊断行使用
            const char* ResultText(Result result);

            // 探测：只判断有没有可刷新的路径，不下发任何命令
            Path Probe();

            // 刷新：按 AHCI、传统 IDE 的次序取第一条可用路径下发命令
            // 一条路径失败不影响另一条；超时有界，绝不无限等待
            Result Flush();
        }
    }
}
