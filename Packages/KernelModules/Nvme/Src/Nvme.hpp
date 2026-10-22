/* Nvme.hpp
    NVMe 读路径：控制器初始化（CAP/VS/CC/CSTS、Admin 队列）、Identify、I/O 队列与 READ

    中断不用：命令完成轮询完成队列的阶段位（phase tag），次数有界；超时按 Result::Timeout
    上报，错误状态按 NVMe 状态码映射 Storage::Error；寄存器经 RegisterAccess 注入：
    目标侧是映射后的 MMIO，宿主夹具是伪寄存器阵列，逻辑本身不碰 volatile 指针
    队列内存（Admin SQ/CQ、I/O SQ/CQ 与数据弹跳页）由调用方按页分配并在 Controller 里
    登记物理地址与虚拟地址；本文件只写队列条目，不申请内存
    读路径按数据弹跳页分块：每条命令一个 PRP1，超过页或 MDTS 的请求拆成多条命令
*/

#pragma once
#include <stdint.h>

#include <Contract.hpp>

namespace Nvme {
    // 控制器寄存器访问：offset 是 BAR0 内的字节偏移，读写都是 32 位
    struct RegisterAccess {
        uint32_t (*read)(void* context, uint32_t offset);             // 读一个 32 位寄存器
        void (*write)(void* context, uint32_t offset, uint32_t value); // 写一个 32 位寄存器
        void* context;                                                // 透传给两个回调的上下文
    };

    // 初始化结果：调用方按它决定返回码与诊断
    enum class Result : uint32_t {
        Ok = 0,          // 初始化完成，控制器可读
        NotSupported,    // VS 主版本不受支持、队列深度不足或 LBA 格式不支持
        Corrupt,         // Identify 数据非法：容量为零、LBA 格式下标越界
        Timeout,         // RDY 等待或命令完成轮询超时
        Io,              // 控制器错误状态、命令状态非零或队列不可用
    };

    // 控制器运行期事实：寄存器访问、队列缓冲与 Identify 得到的能力
    struct Controller {
        RegisterAccess registers = {};   // 寄存器访问
        uint8_t* adminSq = nullptr;      // Admin 提交队列虚拟地址
        uint8_t* adminCq = nullptr;      // Admin 完成队列虚拟地址
        uint8_t* ioSq = nullptr;         // I/O 提交队列虚拟地址
        uint8_t* ioCq = nullptr;         // I/O 完成队列虚拟地址
        uint8_t* data = nullptr;         // 数据弹跳页虚拟地址
        uint64_t adminSqPhysical = 0;    // Admin 提交队列物理地址
        uint64_t adminCqPhysical = 0;    // Admin 完成队列物理地址
        uint64_t ioSqPhysical = 0;       // I/O 提交队列物理地址
        uint64_t ioCqPhysical = 0;       // I/O 完成队列物理地址
        uint64_t dataPhysical = 0;       // 数据弹跳页物理地址
        uint32_t pageBytes = 0;          // 页大小，数据分块按它
        uint32_t doorbellStride = 4;     // 门铃步长：4 << CAP.DSTRD
        uint32_t adminDepth = 0;         // Admin 队列条目数
        uint32_t ioDepth = 0;            // I/O 队列条目数
        uint32_t adminSqTail = 0;        // Admin 提交队列软件尾指针
        uint32_t adminCqHead = 0;        // Admin 完成队列软件头指针
        uint32_t adminCqPhase = 1;       // Admin 完成队列期望阶段位
        uint32_t ioSqTail = 0;           // I/O 提交队列软件尾指针
        uint32_t ioCqHead = 0;           // I/O 完成队列软件头指针
        uint32_t ioCqPhase = 1;          // I/O 完成队列期望阶段位
        uint32_t commandId = 0;          // 命令标识计数器
        uint32_t enabled = 0;            // CC.EN 已置位且 RDY 已就绪
        uint32_t lbaSize = 0;            // 逻辑块大小（Identify Namespace 的 LBA 格式）
        uint64_t blocks = 0;             // 命名空间逻辑块数（NSZE）
        uint32_t maxTransferBytes = 0;   // 单条命令数据上限；0 表示不设
        char model[41] = {};             // Identify Controller 的型号字符串
    };

    // 初始化控制器：校验 CAP/VS、建 Admin 队列、Identify 控制器与命名空间、建 I/O 队列
    Result Initialize(Controller& controller);

    // 从命名空间读一段介质：偏移与长度按逻辑块对齐；分块经数据弹跳页搬运
    Storage::Error Read(Controller& controller, uint64_t offset, void* buffer, uint32_t bytes);

    // 停止控制器：删除 I/O 队列（尽力而为）、清 CC.EN 并等 RDY 清零
    void Shutdown(Controller& controller);

    // 把 NVMe 命令状态码映射成存储面错误码
    Storage::Error MapStatus(uint8_t status);

    // 生成标识行文本：`nvme: <型号> sectors=0x<块数> block=0x<块大小>`，返回写入长度
    uint32_t DescribeDevice(const Controller& controller, char* output, uint32_t capacity);

    // 块设备调用表：context 是 Controller 指针；驱动不导出接口，ops 随绑定事实交给 DevModel
    extern const BlockDeviceOps kOps;
}
