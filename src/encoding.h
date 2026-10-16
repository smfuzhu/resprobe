// encoding.h - 混合编码（UTF-8 与 OEM/GBK 并存）文本解码
//
// 这些函数原先定义在 actions.cpp 的匿名命名空间里，外部（尤其是单元测试）无法
// 链接。这里把它们抽成独立模块，逻辑一字未改 —— 只为让"混合编码解码"这条
// 关键路径获得一个不依赖真实 Windows 主机与管理员权限的回归入口。
#pragma once

#include "common.h"

namespace resprobe {

// 整体按单一代码页解码。strict=true 时任何非法字节都导致失败（返回空串）。
std::wstring TryDecodeWhole(const std::string& bytes, UINT cp, bool strict);

// 逐字符混合解码：把"UTF-8 段 + OEM 代码页段"拼接的字节流各自还原。
std::wstring DecodeMixed(const std::string& bytes);

// 先整体严格按首选编码解码，失败再交给逐字符混合解码器。
std::wstring DecodeBestEffort(const std::string& bytes, UINT preferred);

}  // namespace resprobe
