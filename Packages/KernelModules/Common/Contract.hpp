/* Contract.hpp
    模块侧契约聚合与导入/导出表定义宏：转发生成头，模块源码只 include 本头

    生成头由 MakeContract 从 Packages/Common/Contract 的两份 .def 生成，落在
    Packages/Common/Out/Contract；模块的编译选项带 -I$(CONTRACT_DIR) -I$(COMMON_INCLUDE)
    导入/导出表符号的命名与布局见生成头 Slots.hpp：
      _Contract_Export_<稳定名去点>_v<版本>   提供者槽表，PackModule 从 ELF 符号表取
      _Contract_Import_<稳定名去点>_v<版本>   消费者回填区，零初始化落 .bss
    宏把 default 可见性与 used 一并写好：模块用 -fvisibility=hidden 编译，漏掉可见性
    符号会隐藏、PackModule 取不到；漏掉 used 则 -O2 下可能被整份删掉

    两个宏都是“声明 + 初始化器”的形态，用法见 ModuleTemplate.mk：
      LP_CONTRACT_EXPORT(DevModel, 1, DevModelSlotsV1) = { Enumerate, Resources, ... };
      LP_CONTRACT_IMPORT(CoreLog, 1, CoreLogSlotsV1) = {};
    宏参数 symbolName 是**稳定名删点后的写法**，与 PackModule 的 StripDots 一致：
      Dev.Model -> DevModel、Core.Log -> CoreLog、Fs.Vfs -> FsVfs、Boot.Volume -> BootVolume
    两种宏都要传去点形式；传成 Dev_Model 会拼出 _Contract_Export_Dev_Model_v1，与生成头
    _Contract_Export_DevModel_v1 对不上，PackModule 会报符号缺失
    导出表的初始化器按槽序列出函数地址；导入回填区只写 = {}，由装载器按 8 字节槽回填
*/

#pragma once
#include <stdint.h>

#include <Entry.hpp>
#include <InterfaceId.hpp>
#include <ModuleId.hpp>
#include <Slots.hpp>
#include <Types.hpp>

// 定义提供者导出槽表：符号名 _Contract_Export_<去点稳定名>_v<版本>
// 槽表是**非 const** 的可写段对象：PackModule 要求槽表落在 Data/Bss 才能逐槽读 RELATIVE
// 重定位；初始化器只放模块内函数的地址（-Bsymbolic 本地绑定），不能放导入回填槽
// 定义不带 extern "C"：生成头 Slots.hpp 已用 extern "C" 声明同名符号，定义继承其 C 链接性
// （带 extern "C" 的定义加初始化器会被 -Werror 判成 "initialized and declared 'extern'"）
#define LP_CONTRACT_EXPORT(symbolName, version, slotsType) \
    __attribute__((visibility("default"), used)) slotsType _Contract_Export_##symbolName##_v##version

// 定义消费者导入回填区：符号名 _Contract_Import_<去点稳定名>_v<版本>
// 零初始化落 .bss；装载器按导入表把提供者的函数指针写进这些 8 字节槽，模块在 Init 里直接按槽调用
#define LP_CONTRACT_IMPORT(symbolName, version, slotsType) \
    __attribute__((visibility("default"), used)) slotsType _Contract_Import_##symbolName##_v##version
