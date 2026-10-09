// edid.cpp - 显示器 EDID 读取与解析
#include "edid.h"
#include "util.h"

#include <setupapi.h>

namespace resprobe {

// ---------------------------------------------------------------------------
std::wstring NormalizeMonitorDevicePath(const std::wstring& path) {
    std::wstring s = path;

    if (s.rfind(L"\\\\?\\", 0) == 0 || s.rfind(L"\\??\\", 0) == 0) {
        s = s.substr(4);
    }

    size_t guidPos = s.find(L"#{");
    if (guidPos != std::wstring::npos) {
        s = s.substr(0, guidPos);
    }

    for (wchar_t& c : s) {
        if (c == L'#') c = L'\\';
    }

    while (!s.empty() && s.back() == L'\\') s.pop_back();

    return s;
}

// ---------------------------------------------------------------------------
static std::wstring HexDump(const std::vector<BYTE>& b, size_t maxBytes) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    size_t n = b.size() < maxBytes ? b.size() : maxBytes;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[(b[i] >> 4) & 0x0F]);
        out.push_back(kHex[b[i] & 0x0F]);
    }
    return U82W(out);
}

// ---------------------------------------------------------------------------
static std::wstring ReadDescriptorText(const BYTE* d) {
    // 监视器描述符：d[0..1]==0, d[2]==0, d[3]==tag, d[4]==0, 文本位于 d[5..17]
    std::wstring s;
    for (int i = 5; i < 18; ++i) {
        BYTE c = d[i];
        if (c == 0x0A || c == 0x00) break;
        if (c < 0x20 || c > 0x7E) continue;
        s.push_back((wchar_t)c);
    }
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
EdidInfo ParseEdid(const std::vector<BYTE>& b) {
    EdidInfo out;
    out.present = true;
    out.byteCount = b.size();
    out.rawHex = HexDump(b, 256);

    if (b.size() < 128) {
        out.valid = false;
        return out;
    }

    static const BYTE kHeader[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (memcmp(b.data(), kHeader, 8) != 0) {
        out.valid = false;
        return out;
    }

    // 校验和（仅第一个块）
    BYTE sum = 0;
    for (int i = 0; i < 128; ++i) sum = (BYTE)(sum + b[i]);
    out.checksumOk = (sum == 0);

    // valid 只表示"头部合法"，与校验和分开表达：头部合法但校验和错误的 EDID 仍然保留
    // （原始字节是有价值的证据），但 checksumOk 必须把这个问题暴露到控制台与 JSON，
    // 不能像旧代码那样算完就丢。
    out.valid = true;

    out.pnpIdRaw = (UINT16)((b[8] << 8) | b[9]);
    out.productCodeRaw = (UINT16)(b[10] | (b[11] << 8));
    out.serialNumber = (UINT32)b[12] | ((UINT32)b[13] << 8) |
                       ((UINT32)b[14] << 16) | ((UINT32)b[15] << 24);

    // 三字母厂商代码
    {
        UINT16 mfg = out.pnpIdRaw;
        wchar_t c1 = (wchar_t)(((mfg >> 10) & 0x1F) + L'A' - 1);
        wchar_t c2 = (wchar_t)(((mfg >> 5) & 0x1F) + L'A' - 1);
        wchar_t c3 = (wchar_t)((mfg & 0x1F) + L'A' - 1);
        out.manufacturer.clear();
        out.manufacturer.push_back(c1);
        out.manufacturer.push_back(c2);
        out.manufacturer.push_back(c3);
    }

    out.extensionCount = (int)b[126];

    // 四个 18 字节描述符，位于 54..125
    for (int i = 0; i < 4; ++i) {
        const BYTE* d = b.data() + 54 + i * 18;

        bool isTiming = (d[0] != 0 || d[1] != 0);
        if (isTiming) {
            UINT32 pclk = (UINT32)(d[0] | (d[1] << 8));
            UINT16 hactive = (UINT16)(((d[4] >> 4) << 8) | d[2]);
            UINT16 vactive = (UINT16)(((d[7] >> 4) << 8) | d[5]);
            if (i == 0 && pclk != 0) {
                out.preferredPixelClock10kHz = pclk;
                out.preferredWidth = hactive;
                out.preferredHeight = vactive;
            }
            continue;
        }

        BYTE tag = d[3];
        if (tag == 0xFC) {
            out.monitorName = ReadDescriptorText(d);
        } else if (tag == 0xFF) {
            out.serialText = ReadDescriptorText(d);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
std::map<std::wstring, EdidInfo> EnumerateMonitorEdid(std::wstring* diagOut) {
    std::map<std::wstring, EdidInfo> result;

    HDEVINFO hDev = SetupDiGetClassDevsW(
        &kGuidDevClassMonitor, nullptr, nullptr, DIGCF_PRESENT);
    if (hDev == INVALID_HANDLE_VALUE) {
        if (diagOut) *diagOut = L"SetupDiGetClassDevs(GUID_DEVCLASS_MONITOR) 失败";
        return result;
    }

    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA did;
        ZeroMemory(&did, sizeof(did));
        did.cbSize = sizeof(did);

        if (!SetupDiEnumDeviceInfo(hDev, i, &did)) {
            break;
        }

        wchar_t instanceId[512] = { 0 };
        if (!SetupDiGetDeviceInstanceIdW(hDev, &did, instanceId, _countof(instanceId), nullptr)) {
            continue;
        }

        std::wstring key = UpperW(instanceId);

        HKEY hKey = SetupDiOpenDevRegKey(hDev, &did, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (hKey == INVALID_HANDLE_VALUE) {
            EdidInfo info;
            info.present = false;
            info.registryPath = instanceId;
            result[key] = info;
            continue;
        }

        std::vector<BYTE> bytes;
        DWORD type = 0;
        DWORD size = 0;
        LONG rc = RegQueryValueExW(hKey, L"EDID", nullptr, &type, nullptr, &size);
        if (rc == ERROR_SUCCESS && size > 0 && size <= 4096) {
            bytes.resize(size);
            rc = RegQueryValueExW(hKey, L"EDID", nullptr, &type, bytes.data(), &size);
            if (rc != ERROR_SUCCESS) bytes.clear();
            else bytes.resize(size);
        } else {
            bytes.clear();
        }
        RegCloseKey(hKey);

        if (bytes.empty()) {
            EdidInfo info;
            info.present = false;
            info.registryPath = instanceId;
            result[key] = info;
        } else {
            EdidInfo info = ParseEdid(bytes);
            info.registryPath = instanceId;
            result[key] = info;
        }
    }

    SetupDiDestroyDeviceInfoList(hDev);

    if (diagOut) {
        wchar_t buf[128];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                     L"监视器设备类枚举到 %u 个设备", (unsigned)result.size());
        *diagOut = buf;
    }
    return result;
}

}  // namespace resprobe
