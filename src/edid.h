// edid.h - 显示器 EDID 读取与解析
#pragma once

#include "common.h"

namespace resprobe {

// 规范化 DisplayConfig 返回的监视器设备路径为设备实例 ID。
// \\?\DISPLAY#DEL40E7#5&2b8f7b96&0&UID4353#{guid}  ->  DISPLAY\DEL40E7\5&2b8f7b96&0&UID4353
std::wstring NormalizeMonitorDevicePath(const std::wstring& path);

// 枚举监视器设备类，读取并解析每个设备的 EDID。
// 返回的 map 键为“大写化的设备实例 ID”。
std::map<std::wstring, EdidInfo> EnumerateMonitorEdid(std::wstring* diagOut = nullptr);

// 解析原始 EDID 字节
EdidInfo ParseEdid(const std::vector<BYTE>& bytes);

}  // namespace resprobe
