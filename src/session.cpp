// session.cpp - 会话环境与远程软件特征探测
#include "session.h"
#include "util.h"

#include <wtsapi32.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <winsvc.h>

namespace resprobe {

namespace {

// 远程控制 / 虚拟显示厂商特征表
struct VendorKeyword {
    const wchar_t* keyword;
    const wchar_t* name;
    bool virtualDisplay;
};

const VendorKeyword kVendors[] = {
    { L"todesk",        L"ToDesk",                    false },
    { L"sunlogin",      L"向日葵 SunLogin",            false },
    { L"oray",          L"Oray 向日葵",                false },
    { L"anydesk",       L"AnyDesk",                   false },
    { L"teamviewer",    L"TeamViewer",                false },
    { L"rustdesk",      L"RustDesk",                  false },
    { L"radmin",        L"Radmin",                    false },
    { L"winvnc",        L"VNC (winvnc)",              false },
    { L"vnc",           L"VNC",                       false },
    { L"parsec",        L"Parsec",                    false },
    { L"mstsc",         L"Microsoft RDP 客户端",       false },
    { L"displaylink",   L"DisplayLink",               true  },
    { L"indirect",      L"Indirect Display Driver",   true  },
    { L"idddriver",     L"IDD 间接显示驱动",           true  },
    { L"idd driver",    L"IDD 间接显示驱动",           true  },
    { L"virtual display", L"虚拟显示器驱动",            true  },
    { L"usbmmidd",      L"USBMMIDD 虚拟显示器",         true  },
    { L"remote display", L"Microsoft Remote Display", true  },
    { L"spacedesk",     L"spacedesk",                 true  },
    { L"duet",          L"Duet Display",              true  },
    { L"basic display", L"Microsoft 基本显示适配器",     false },
};

const VendorKeyword* MatchVendor(const std::wstring& text) {
    for (const VendorKeyword& v : kVendors) {
        if (ContainsInsensitiveW(text, v.keyword)) return &v;
    }
    return nullptr;
}

void AddUnique(std::vector<RemoteSoftwareInfo>& list, const RemoteSoftwareInfo& info) {
    for (const RemoteSoftwareInfo& e : list) {
        if (e.name == info.name && e.kind == info.kind && e.detail == info.detail) return;
    }
    list.push_back(info);
}

// 直接使用数值常量，避免为此引入 winsock2.h（该头文件必须在 windows.h 之前包含）
const USHORT kAddrFamilyInet  = 2;
const USHORT kAddrFamilyInet6 = 23;

// AF_INET 时 IPv4 地址在 WTS_CLIENT_ADDRESS::Address[] 中的起始偏移。
//
// 取 2 的依据：RDP 实现填入的布局是 family(2) + IPv4(4) [+ port(2)] —— 前两字节是
// 地址族（与 AddressFamily 字段重复），IP 从下标 2 开始。这也是绝大多数公开样例
// （含微软示例）的读法。
//
// 注意：本文件此前存在自相矛盾 —— 注释写的是 family(2)+port(2)+addr(4)（即应从下标 4
// 取），代码却按下标 2 取。二者必有一错，而这条信息只用于人看的诊断输出，离线无法证伪。
// 因此把偏移集中为唯一常量：若在**真实 RDP 会话**中实测发现前三段是端口字节
// （形如 12345.x.y.z），说明系统填的是完整 sockaddr_in，把这里改成 4 即可。
const int kInetAddrOffset = 2;

std::wstring FormatAddress(const WTS_CLIENT_ADDRESS& a) {
    wchar_t buf[128] = { 0 };
    if (a.AddressFamily == kAddrFamilyInet) {
        const unsigned o1 = a.Address[kInetAddrOffset + 0];
        const unsigned o2 = a.Address[kInetAddrOffset + 1];
        const unsigned o3 = a.Address[kInetAddrOffset + 2];
        const unsigned o4 = a.Address[kInetAddrOffset + 3];
        if (o1 == 0 && o2 == 0 && o3 == 0 && o4 == 0) {
            // 全 0 表示服务端未提供客户端地址：不要打印一个看起来像合法地址的 0.0.0.0
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"(未提供)");
        } else {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%u.%u.%u.%u", o1, o2, o3, o4);
        }
    } else if (a.AddressFamily == kAddrFamilyInet6) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"[IPv6 客户端地址]");
    } else {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"(地址族=%u)",
                     (unsigned)a.AddressFamily);
    }
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation;
    ZeroMemory(&elevation, sizeof(elevation));
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// ---------------------------------------------------------------------------
SessionInfo QuerySessionInfo() {
    SessionInfo si;

    si.remoteSession = GetSystemMetrics(SM_REMOTESESSION) != 0;
    si.elevated = IsProcessElevated();

    DWORD sid = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sid)) si.sessionId = sid;

    DWORD consoleSid = WTSGetActiveConsoleSessionId();
    si.activeConsoleSessionId = consoleSid;
    si.consoleDisplayConnected = (consoleSid != 0xFFFFFFFF);
    si.isConsoleSession = (consoleSid != 0xFFFFFFFF && sid == consoleSid);

    LPWSTR buf = nullptr;
    DWORD bytes = 0;

    auto queryString = [&](WTS_INFO_CLASS cls) -> std::wstring {
        std::wstring r;
        buf = nullptr;
        bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                        cls, &buf, &bytes) && buf != nullptr) {
            r.assign(buf, bytes / sizeof(wchar_t));
            while (!r.empty() && r.back() == L'\0') r.pop_back();
            WTSFreeMemory(buf);
            buf = nullptr;
        }
        return r;
    };

    si.userName = queryString(WTSUserName);
    si.winStationName = queryString(WTSWinStationName);
    si.domainName = queryString(WTSDomainName);
    si.clientName = queryString(WTSClientName);

    // 协议类型（USHORT 返回值）
    buf = nullptr;
    bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSClientProtocolType, &buf, &bytes) && buf) {
        si.protocolType = (DWORD)(*(USHORT*)buf);
        WTSFreeMemory(buf);
        buf = nullptr;
    } else {
        si.protocolType = si.remoteSession ? 2 : 0;
    }
    si.protocolName = ProtocolTypeName(si.protocolType);

    // 连接状态
    buf = nullptr;
    bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSConnectState, &buf, &bytes) && buf) {
        si.connectState = (DWORD)(*(WTS_CONNECTSTATE_CLASS*)buf);
        WTSFreeMemory(buf);
        buf = nullptr;
    }
    si.connectStateName = ConnectStateName(si.connectState);

    // 客户端地址
    buf = nullptr;
    bytes = 0;
    if (si.remoteSession &&
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSClientAddress, &buf, &bytes) && buf) {
        si.clientAddress = FormatAddress(*(WTS_CLIENT_ADDRESS*)buf);
        WTSFreeMemory(buf);
        buf = nullptr;
    }

    return si;
}

// ---------------------------------------------------------------------------
std::vector<RemoteSoftwareInfo> DetectRemoteSoftware(
    const std::vector<DisplayTarget>& targets) {

    std::vector<RemoteSoftwareInfo> list;

    // --- 1. 当前活动的显示输出（来自 DisplayConfig） ---
    for (const DisplayTarget& t : targets) {
        std::wstring blob = t.gdiDeviceString + L" " + t.gdiDeviceId + L" " +
                            t.monitorFriendlyName + L" " + t.monitorInstanceId + L" " +
                            t.monitorDevicePath + L" " + t.outputTechnologyName;

        const VendorKeyword* v = MatchVendor(blob);
        if (v) {
            RemoteSoftwareInfo info;
            info.name = v->name;
            info.kind = L"display-adapter";
            info.detail = t.gdiDeviceString.empty() ? t.monitorFriendlyName : t.gdiDeviceString;
            info.isVirtualDisplay = v->virtualDisplay;
            if (!info.detail.empty()) AddUnique(list, info);
        }

        // 间接/虚拟输出技术本身就是强证据
        if (t.outputTechnology == 0x80000001 /* INDIRECT_WIRED */ ||
            t.outputTechnology == 0x80000002 /* INDIRECT_VIRTUAL */) {
            RemoteSoftwareInfo info;
            info.name = L"间接显示驱动输出";
            info.kind = L"display-adapter";
            info.detail = t.gdiDeviceName + L" 输出技术=" + t.outputTechnologyName;
            info.isVirtualDisplay = true;
            AddUnique(list, info);
        }

        if (t.gdiMirroringDriver) {
            RemoteSoftwareInfo info;
            info.name = L"镜像/间接显示驱动";
            info.kind = L"display-adapter";
            info.detail = t.gdiDeviceName + L" 携带 DISPLAY_DEVICE_MIRRORING_DRIVER 标志";
            info.isVirtualDisplay = true;
            AddUnique(list, info);
        }
    }

    // --- 2. 监视器设备类（哪怕当前未激活也能发现虚拟显示器） ---
    {
        HDEVINFO hDev = SetupDiGetClassDevsW(&kGuidDevClassMonitor, nullptr, nullptr,
                                             DIGCF_PRESENT);
        if (hDev != INVALID_HANDLE_VALUE) {
            for (DWORD i = 0;; ++i) {
                SP_DEVINFO_DATA did;
                ZeroMemory(&did, sizeof(did));
                did.cbSize = sizeof(did);
                if (!SetupDiEnumDeviceInfo(hDev, i, &did)) break;

                wchar_t iid[512] = { 0 };
                SetupDiGetDeviceInstanceIdW(hDev, &did, iid, _countof(iid), nullptr);

                wchar_t desc[256] = { 0 };
                SetupDiGetDeviceRegistryPropertyW(hDev, &did, SPDRP_DEVICEDESC,
                                                  nullptr, (PBYTE)desc, sizeof(desc), nullptr);

                std::wstring blob = std::wstring(iid) + L" " + desc;
                const VendorKeyword* v = MatchVendor(blob);
                if (v) {
                    RemoteSoftwareInfo info;
                    info.name = v->name;
                    info.kind = L"monitor-device";
                    info.detail = std::wstring(desc) + L" | " + iid;
                    info.isVirtualDisplay = v->virtualDisplay;
                    AddUnique(list, info);
                }
            }
            SetupDiDestroyDeviceInfoList(hDev);
        }
    }

    // --- 3. 显示适配器设备类：枚举器为 ROOT 说明是软件/虚拟设备 ---
    {
        HDEVINFO hDev = SetupDiGetClassDevsW(&kGuidDevClassDisplay, nullptr, nullptr,
                                             DIGCF_PRESENT);
        if (hDev != INVALID_HANDLE_VALUE) {
            for (DWORD i = 0;; ++i) {
                SP_DEVINFO_DATA did;
                ZeroMemory(&did, sizeof(did));
                did.cbSize = sizeof(did);
                if (!SetupDiEnumDeviceInfo(hDev, i, &did)) break;

                wchar_t iid[512] = { 0 };
                SetupDiGetDeviceInstanceIdW(hDev, &did, iid, _countof(iid), nullptr);

                wchar_t desc[256] = { 0 };
                SetupDiGetDeviceRegistryPropertyW(hDev, &did, SPDRP_DEVICEDESC,
                                                  nullptr, (PBYTE)desc, sizeof(desc), nullptr);

                wchar_t enumerator[64] = { 0 };
                SetupDiGetDeviceRegistryPropertyW(hDev, &did, SPDRP_ENUMERATOR_NAME,
                                                  nullptr, (PBYTE)enumerator,
                                                  sizeof(enumerator), nullptr);

                std::wstring blob = std::wstring(iid) + L" " + desc + L" " + enumerator;
                const VendorKeyword* v = MatchVendor(blob);
                bool rootEnum = ContainsInsensitiveW(enumerator, L"ROOT");

                if (v || rootEnum) {
                    RemoteSoftwareInfo info;
                    info.name = v ? v->name : L"软件/虚拟显示适配器";
                    info.kind = L"display-adapter";
                    info.detail = std::wstring(desc) + L" | " + iid +
                                  L" | 枚举器=" + enumerator;
                    info.isVirtualDisplay = v ? v->virtualDisplay : true;
                    AddUnique(list, info);
                }
            }
            SetupDiDestroyDeviceInfoList(hDev);
        }
    }

    // --- 4. 进程 ---
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            ZeroMemory(&pe, sizeof(pe));
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    std::wstring exe = pe.szExeFile;
                    const VendorKeyword* v = MatchVendor(exe);
                    if (v) {
                        RemoteSoftwareInfo info;
                        info.name = v->name;
                        info.kind = L"process";
                        info.detail = exe + L" (PID " + std::to_wstring(pe.th32ProcessID) + L")";
                        info.isVirtualDisplay = false;
                        AddUnique(list, info);
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    // --- 5. 服务（失败不影响结果） ---
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (scm) {
            // M-5：当服务数量超过一次调用的缓冲区容量时，EnumServicesStatusExW 返回
            // FALSE 并置 ERROR_MORE_DATA（同时更新 resume 与 needed）。旧代码只看一次
            // 返回值就收工，在服务较多的机器上会**整类漏检**远程软件的服务痕迹。
            // 这里按 resume 续枚举，必要时扩大缓冲区；用轮次上限兜底防死循环。
            std::vector<BYTE> buf(64 * 1024);
            DWORD resume = 0;

            for (int round = 0; round < 64; ++round) {
                DWORD needed = 0;
                DWORD returned = 0;
                SetLastError(ERROR_SUCCESS);
                const BOOL ok = EnumServicesStatusExW(
                    scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                    buf.data(), (DWORD)buf.size(), &needed, &returned, &resume, nullptr);

                if (returned > 0) {
                    auto* items =
                        reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buf.data());
                    for (DWORD i = 0; i < returned; ++i) {
                        std::wstring blob = std::wstring(items[i].lpServiceName) + L" " +
                                            items[i].lpDisplayName;
                        const VendorKeyword* v = MatchVendor(blob);
                        if (v) {
                            RemoteSoftwareInfo info;
                            info.name = v->name;
                            info.kind = L"service";
                            info.detail = items[i].lpDisplayName;
                            info.isVirtualDisplay = false;
                            AddUnique(list, info);
                        }
                    }
                }

                if (ok) break;                                    // 枚举完成
                if (GetLastError() != ERROR_MORE_DATA) break;      // 其它错误：放弃

                // 缓冲区不足以装下剩余项：扩容后重来（resume 才有效）
                if (needed > buf.size()) buf.resize(needed);
                // 否则用同一缓冲区 + resume 继续取下一批
            }
            CloseServiceHandle(scm);
        }
    }

    return list;
}

}  // namespace resprobe
