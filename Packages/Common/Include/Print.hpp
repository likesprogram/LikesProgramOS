/* Print.hpp
    文本输出前端与多路复用目标的声明
*/

#pragma once
#include <stdint.h>

namespace Print {
    // 输出目标：把文本写入一条具体通道，如 VGA 文本、串口、调试口或内核自绘
    // 目标不做格式化、不翻译换行、不分配内存、不抛异常；写不出去就丢弃，绝不阻塞或停机
    class OutTarget {
    public:
        // 目标是否可用；仅供诊断查询，Write 不要求调用方先行判断
        virtual bool Ready() const { return true; }
        // 写入以 '\0' 结尾的文本；默认实现丢弃，避免无 C++ 运行库的镜像引用 __cxa_pure_virtual
        virtual void Write(const char* text) { (void)text; }
        // 设置后续写入使用的显示属性；默认实现丢弃：串口与调试口是流式通道，没有属性概念
        // 属性是通道的显示特性而不是文本内容，不随文本进入流，自动化抓取的日志因此保持纯文本
        virtual void SetAttribute(uint8_t attribute) { (void)attribute; }
        // 清空目标已显示的文本；默认实现什么都不做：串口与调试口是流式通道，没有清屏语义
        // 与 Write 一样不要求调用方先判断可用性，设备未就绪时静默丢弃
        virtual void ClearScreen() {}
    protected:
        // 目标由阶段二进制静态持有，不通过基类指针销毁，因此不需要虚析构与运行库支持
        ~OutTarget() = default;
    };

    // 多路复用目标：把一次写入按加入顺序扇出到多个目标
    class MultiplexTarget : public OutTarget {
    public:
        static constexpr uint32_t kMaxTargets = 4;   // 可加入的目标数量上限

        // 加入目标；已满或空指针时返回 false
        bool Add(OutTarget* target);
        // 移除全部目标
        void Clear() { m_count = 0; }
        // 当前已加入的目标数量
        uint32_t Count() const { return m_count; }
        // 任一成员可用即视为可用
        bool Ready() const override;
        // 按加入顺序写入每个目标；单个目标的丢弃不影响其他目标
        void Write(const char* text) override;
        // 按加入顺序设置每个目标的显示属性；不支持属性的成员忽略
        void SetAttribute(uint8_t attribute) override;
        // 按加入顺序让每个目标清屏；不支持清屏的目标不受影响
        void ClearScreen() override;
    private:
        OutTarget* m_targets[kMaxTargets] = {};   // 已加入的目标；非拥有，须在安装期间保持有效
        uint32_t m_count = 0;                     // 已加入的目标数量
    };

    namespace Detail {
        // 当前目标；空表示未安装，写入被静默丢弃
        extern OutTarget* currentTarget;
    }

    // 安装当前目标；传空指针回到未安装状态，可重复调用
    inline void SetTarget(OutTarget* target) { Detail::currentTarget = target; }
    // 当前目标；未安装时为空
    inline OutTarget* Target() { return Detail::currentTarget; }
    // 写入以 '\0' 结尾的文本；未安装目标或文本为空时丢弃
    inline void Write(const char* text) { if (Detail::currentTarget != nullptr && text != nullptr) Detail::currentTarget->Write(text); }
    // 设置当前目标的显示属性；未安装目标时丢弃
    // 只有 VGA 文本这类有屏幕的目标有属性概念，流式通道收到后不做任何事
    inline void SetAttribute(uint8_t attribute) { if (Detail::currentTarget != nullptr) Detail::currentTarget->SetAttribute(attribute); }
    // 清空当前目标已显示的文本；未安装目标时丢弃
    // 流式通道（串口、调试口）收到后不做任何事，清屏只对 VGA 文本这类有屏幕的目标有效
    inline void ClearScreen() { if (Detail::currentTarget != nullptr) Detail::currentTarget->ClearScreen(); }
    // 写入单个字符
    void PutChar(char character);
    // 写入 32 位十六进制，格式为 "0x" 加 8 位大写数字
    void WriteHex(uint32_t value);
}
