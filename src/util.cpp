// util.cpp - 通用辅助函数实现
#include "util.h"

#include <cstdarg>

namespace resprobe {

// ---------------------------------------------------------------------------
// 字符编码
// ---------------------------------------------------------------------------
std::wstring U82W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], need);
    return out;
}

std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], need, nullptr, nullptr);
    return out;
}

std::string W2U8(const wchar_t* w) {
    if (!w) return std::string();
    return W2U8(std::wstring(w));
}

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------
std::string Iso8601FromSystemTime(const SYSTEMTIME& st) {
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

std::string NowIso8601Utc() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    return Iso8601FromSystemTime(st);
}

UINT64 MonotonicMs() {
    return (UINT64)GetTickCount64();
}

std::string UptimeText(UINT64 ms) {
    UINT64 totalSec = ms / 1000;
    UINT64 h = totalSec / 3600;
    UINT64 m = (totalSec % 3600) / 60;
    UINT64 s = totalSec % 60;
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lluh%02llum%02llus",
                (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    return std::string(buf);
}

std::string LocalTimeText() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 字符串
// ---------------------------------------------------------------------------
std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", (int)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
    return out;
}

std::string UpperAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    }
    return r;
}

std::wstring UpperW(const std::wstring& s) {
    std::wstring r = s;
    CharUpperBuffW(&r[0], (DWORD)r.size());
    return r;
}

bool ContainsInsensitiveW(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return false;
    std::wstring h = UpperW(haystack);
    std::wstring n = UpperW(needle);
    return h.find(n) != std::wstring::npos;
}

// ---------------------------------------------------------------------------
// 枚举名称
// ---------------------------------------------------------------------------
std::wstring OutputTechnologyName(UINT32 tech) {
    switch (tech) {
        case 0xFFFFFFFF: return L"OTHER";
        case 0:          return L"VGA";
        case 1:          return L"SVIDEO";
        case 2:          return L"COMPOSITE_VIDEO";
        case 3:          return L"COMPONENT_VIDEO";
        case 4:          return L"DVI";
        case 5:          return L"HDMI";
        case 6:          return L"LVDS";
        case 8:          return L"D_JPN";
        case 9:          return L"SDI";
        case 10:         return L"DISPLAYPORT_EXTERNAL";
        case 11:         return L"DISPLAYPORT_EMBEDDED";
        case 12:         return L"UDI_EXTERNAL";
        case 13:         return L"UDI_EMBEDDED";
        case 14:         return L"SDTV_DONGLE";
        case 0x80000000: return L"INTERNAL";
        case 0x80000001: return L"INDIRECT_WIRED";
        case 0x80000002: return L"INDIRECT_VIRTUAL";
        case 0x80000003: return L"DISPLAYPORT_USB_TUNNEL";
        default: {
            wchar_t buf[32];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"UNKNOWN(0x%08X)", tech);
            return std::wstring(buf);
        }
    }
}

std::wstring PixelFormatName(UINT32 fmt) {
    switch (fmt) {
        case 0: return L"8bpp";
        case 1: return L"16bpp";
        case 2: return L"32bpp";
        case 3: return L"NA";
        default: return L"UNKNOWN";
    }
}

std::wstring DisplayPowerStateName(DisplayPowerState s) {
    switch (s) {
        case DisplayPowerState::On:     return L"ON";
        case DisplayPowerState::Off:    return L"OFF";
        case DisplayPowerState::Dimmed: return L"DIM";
        default:                        return L"UNKNOWN";
    }
}

std::wstring DispHangeResultName(LONG code) {
    switch (code) {
        case  0: return L"DISP_CHANGE_SUCCESSFUL";
        case  1: return L"DISP_CHANGE_RESTART";
        case -1: return L"DISP_CHANGE_FAILED";
        case -2: return L"DISP_CHANGE_BADMODE";
        case -3: return L"DISP_CHANGE_NOTUPDATED";
        case -4: return L"DISP_CHANGE_BADFLAGS";
        case -5: return L"DISP_CHANGE_BADPARAM";
        case -6: return L"DISP_CHANGE_BADDUALVIEW";
        default: {
            wchar_t buf[32];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"UNKNOWN(%ld)", code);
            return std::wstring(buf);
        }
    }
}

std::wstring ConnectStateName(DWORD state) {
    switch (state) {
        case 0: return L"WTSActive";
        case 1: return L"WTSConnected";
        case 2: return L"WTSConnectQuery";
        case 3: return L"WTSShadow";
        case 4: return L"WTSDisconnected";
        case 5: return L"WTSIdle";
        case 6: return L"WTSListen";
        case 7: return L"WTSReset";
        case 8: return L"WTSDown";
        case 9: return L"WTSInit";
        default: return L"WTSUnknown";
    }
}

std::wstring ProtocolTypeName(DWORD type) {
    switch (type) {
        case 0: return L"Console";
        case 1: return L"ICA";
        case 2: return L"RDP";
        default: return L"Unknown";
    }
}

// ---------------------------------------------------------------------------
// 格式化
// ---------------------------------------------------------------------------
std::wstring FormatRect(const RECT& r) {
    wchar_t buf[64];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%ld,%ld %ldx%ld",
                 r.left, r.top, r.right - r.left, r.bottom - r.top);
    return std::wstring(buf);
}

std::wstring FormatModeText(UINT32 w, UINT32 h) {
    wchar_t buf[32];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%ux%u", w, h);
    return std::wstring(buf);
}

std::wstring FormatModeText(UINT32 w, UINT32 h, UINT32 hz) {
    if (hz == 0) return FormatModeText(w, h);
    wchar_t buf[48];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%ux%u@%uHz", w, h, hz);
    return std::wstring(buf);
}

std::string FormatLuid(const LUID& luid) {
    char buf[48];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08X%08X",
                (unsigned)luid.HighPart, (unsigned)luid.LowPart);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 日志（全部转 UTF-8 输出，保证中文与控制台代码页一致）
// ---------------------------------------------------------------------------
void LogLine(const std::string& utf8) {
    fputs(utf8.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void LogW(const std::wstring& text) {
    LogLine(W2U8(text));
}

}  // namespace resprobe
