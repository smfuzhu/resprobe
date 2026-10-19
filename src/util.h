// util.h - 通用辅助函数
#pragma once

#include "common.h"

namespace resprobe {

// --- 字符编码 ---
std::string W2U8(const std::wstring& w);
std::wstring U82W(const std::string& s);
std::string W2U8(const wchar_t* w);

// --- 时间 ---
std::string NowIso8601Utc();
std::string Iso8601FromSystemTime(const SYSTEMTIME& st);
UINT64 MonotonicMs();                      // GetTickCount64()
std::string UptimeText(UINT64 ms);
std::string LocalTimeText();
std::string LocalStampText();

// --- 字符串 ---
std::string JsonEscape(const std::string& in);
std::string UpperAscii(const std::string& s);
std::wstring UpperW(const std::wstring& s);
bool ContainsInsensitiveW(const std::wstring& haystack, const std::wstring& needle);

// --- 枚举名称 ---
std::wstring OutputTechnologyName(UINT32 tech);
std::wstring PixelFormatName(UINT32 fmt);
std::wstring DisplayPowerStateName(DisplayPowerState s);
std::wstring DispHangeResultName(LONG code);
std::wstring ConnectStateName(DWORD state);
std::wstring ProtocolTypeName(DWORD type);

// --- 格式化 ---
std::wstring FormatRect(const RECT& r);
std::wstring FormatModeText(UINT32 w, UINT32 h);
std::wstring FormatModeText(UINT32 w, UINT32 h, UINT32 hz);
std::string  FormatLuid(const LUID& luid);

// --- 日志 ---
void LogLine(const std::string& utf8);
void LogW(const std::wstring& text);

}  // namespace resprobe
