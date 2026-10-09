// display.cpp - 显示输出拓扑 / 模式表 / EDID 关联探测
#include "display.h"
#include "edid.h"
#include "util.h"

#include <cstdio>

namespace resprobe {

namespace {

const int kMaxModeEnumeration = 4000;

struct GdiAdapterRec {
    std::wstring name;
    DISPLAY_DEVICEW dd;
};

struct MonitorRectRec {
    RECT rc{};
    bool primary = false;
    bool valid = false;
};

struct MonitorEnumCtx {
    std::map<std::wstring, MonitorRectRec> map;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<MonitorEnumCtx*>(lp);
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorRectRec rec;
        rec.rc = mi.rcMonitor;
        rec.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        rec.valid = true;
        ctx->map[UpperW(mi.szDevice)] = rec;
    }
    return TRUE;
}

std::vector<GdiAdapterRec> EnumerateGdiAdapters() {
    std::vector<GdiAdapterRec> out;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        GdiAdapterRec rec;
        rec.name = dd.DeviceName;
        rec.dd = dd;
        out.push_back(rec);
    }
    return out;
}

// 填充 GDI 模式表信息
void FillGdiModes(DisplayTarget& t, UINT32 refW, UINT32 refH) {
    if (t.gdiDeviceName.empty()) return;

    const wchar_t* dev = t.gdiDeviceName.c_str();

    // 当前设置
    {
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, &dm, 0)) {
            t.gdiCurrentMode = FormatModeText(dm.dmPelsWidth, dm.dmPelsHeight);
            t.gdiCurrentRefreshHz = dm.dmDisplayFrequency;
            t.gdiCurrentBitsPerPel = dm.dmBitsPerPel;
        }
    }

    // 注册表中的设置
    {
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(dev, ENUM_REGISTRY_SETTINGS, &dm, 0)) {
            t.gdiRegistryMode = FormatModeText(dm.dmPelsWidth, dm.dmPelsHeight);
        }
    }

    // 模式表
    std::vector<std::wstring> uniqueRes;
    for (int i = 0; i < kMaxModeEnumeration; ++i) {
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(dev, (DWORD)i, &dm, 0)) {
            break;
        }
        ++t.modeCount;

        std::wstring res = FormatModeText(dm.dmPelsWidth, dm.dmPelsHeight);
        if (std::find(uniqueRes.begin(), uniqueRes.end(), res) == uniqueRes.end()) {
            if (uniqueRes.size() < 64) uniqueRes.push_back(res);
            else t.modeListTruncated = true;
        }

        if (dm.dmPelsWidth == refW && dm.dmPelsHeight == refH) {
            t.referenceModeAvailable = true;
        }
        if ((dm.dmPelsWidth == 1024 && dm.dmPelsHeight == 768) ||
            (dm.dmPelsWidth == 800 && dm.dmPelsHeight == 600) ||
            (dm.dmPelsWidth == 640 && dm.dmPelsHeight == 480)) {
            t.fallbackModeAvailable = true;
        }
    }

    if (t.modeCount >= kMaxModeEnumeration) t.modeListTruncated = true;
    t.modeResolutions.swap(uniqueRes);
}

}  // namespace

// ---------------------------------------------------------------------------
std::vector<DisplayTarget> EnumerateDisplays(UINT32 refW, UINT32 refH,
                                             std::wstring* diagOut,
                                             int* totalPathsOut,
                                             bool includePlaceholderPaths) {
    std::vector<DisplayTarget> out;
    std::string notes;

    // ---- 1. EDID 表 ----
    std::map<std::wstring, EdidInfo> edidTable = EnumerateMonitorEdid();
    std::vector<std::pair<UINT16, UINT16>> edidRawKeys;
    for (const auto& kv : edidTable) {
        edidRawKeys.push_back(std::make_pair(kv.second.pnpIdRaw, kv.second.productCodeRaw));
    }

    // ---- 2. GDI 适配器 ----
    std::vector<GdiAdapterRec> adapters = EnumerateGdiAdapters();

    // ---- 3. 桌面矩形 ----
    MonitorEnumCtx monCtx;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monCtx));

    // ---- 4. DisplayConfig 路径 ----
    UINT32 numPaths = 0;
    UINT32 numModes = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes);
    if (rc != ERROR_SUCCESS) {
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "GetDisplayConfigBufferSizes 失败 rc=%ld", (long)rc);
        notes += buf;
        if (totalPathsOut) *totalPathsOut = 0;
        if (diagOut) *diagOut = U82W(notes);
        return out;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    paths.resize(numPaths);
    modes.resize(numModes);

    rc = QueryDisplayConfig(QDC_ALL_PATHS,
                            &numPaths, paths.empty() ? nullptr : paths.data(),
                            &numModes, modes.empty() ? nullptr : modes.data(),
                            nullptr);
    if (rc != ERROR_SUCCESS) {
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "QueryDisplayConfig 失败 rc=%ld", (long)rc);
        notes += buf;
        if (totalPathsOut) *totalPathsOut = 0;
        if (diagOut) *diagOut = U82W(notes);
        return out;
    }
    paths.resize(numPaths);
    modes.resize(numModes);

    char tmp[160];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE,
                "DisplayConfig: %u 条路径 / %u 条模式; EDID 设备 %u 个",
                (unsigned)paths.size(), (unsigned)modes.size(),
                (unsigned)edidTable.size());
    notes += tmp;

    // 适配器设备路径按 LUID 缓存，避免每条路径重复查询
    std::map<std::string, std::wstring> adapterPathCache;

    // ---- 5. 逐路径填充 ----
    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DisplayTarget t;

        t.active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        t.targetAvailable = (p.targetInfo.targetAvailable != FALSE);
        t.targetStatusFlags = p.targetInfo.statusFlags;
        t.adapterLuid = p.targetInfo.adapterId;
        t.targetId = p.targetInfo.id;
        t.sourceId = p.sourceInfo.id;
        t.outputTechnology = (UINT32)p.targetInfo.outputTechnology;
        t.outputTechnologyName = OutputTechnologyName(t.outputTechnology);

        // 适配器设备路径（按 LUID 缓存）
        {
            std::string luidKey = FormatLuid(p.targetInfo.adapterId);
            auto cit = adapterPathCache.find(luidKey);
            if (cit != adapterPathCache.end()) {
                t.adapterDevicePath = cit->second;
            } else {
                DISPLAYCONFIG_ADAPTER_NAME an;
                ZeroMemory(&an, sizeof(an));
                an.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
                an.header.size = sizeof(an);
                an.header.adapterId = p.targetInfo.adapterId;
                an.header.id = 0;   // 查适配器名时 id 必须为 0，否则会返回错误的适配器路径
                std::wstring path;
                if (DisplayConfigGetDeviceInfo(&an.header) == ERROR_SUCCESS) {
                    path = an.adapterDevicePath;
                }
                adapterPathCache[luidKey] = path;
                t.adapterDevicePath = path;
            }
        }

        // target 名称 / 监视器路径
        {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
            ZeroMemory(&tn, sizeof(tn));
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tn.header.size = sizeof(tn);
            tn.header.adapterId = p.targetInfo.adapterId;
            tn.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
                t.monitorFriendlyName = tn.monitorFriendlyDeviceName;
                t.monitorDevicePath = tn.monitorDevicePath;
                t.monitorInstanceId = NormalizeMonitorDevicePath(tn.monitorDevicePath);
                t.edidManufactureId = tn.edidManufactureId;
                t.edidProductCodeId = tn.edidProductCodeId;
                t.connectorInstance = tn.connectorInstance;
                t.friendlyNameFromEdid = (tn.flags.friendlyNameFromEdid != 0);
            }
        }

        // source 对应的 GDI 设备名
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            ZeroMemory(&sn, sizeof(sn));
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof(sn);
            sn.header.adapterId = p.sourceInfo.adapterId;
            sn.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS) {
                t.gdiDeviceName = sn.viewGdiDeviceName;
            }
        }

        // 首选模式
        {
            DISPLAYCONFIG_TARGET_PREFERRED_MODE pm;
            ZeroMemory(&pm, sizeof(pm));
            pm.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
            pm.header.size = sizeof(pm);
            pm.header.adapterId = p.targetInfo.adapterId;
            pm.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&pm.header) == ERROR_SUCCESS) {
                t.hasPreferred = true;
                t.preferredWidth = pm.width;
                t.preferredHeight = pm.height;
                t.preferredRefreshNumerator = pm.targetMode.targetVideoSignalInfo.vSyncFreq.Numerator;
                t.preferredRefreshDenominator = pm.targetMode.targetVideoSignalInfo.vSyncFreq.Denominator;
            }
        }

        // 当前模式
        if (p.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            p.sourceInfo.modeInfoIdx < modes.size()) {
            const DISPLAYCONFIG_MODE_INFO& m = modes[p.sourceInfo.modeInfoIdx];
            if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                t.hasSourceMode = true;
                t.width = m.sourceMode.width;
                t.height = m.sourceMode.height;
                t.pixelFormat = (UINT32)m.sourceMode.pixelFormat;
            }
        }
        if (p.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            p.targetInfo.modeInfoIdx < modes.size()) {
            const DISPLAYCONFIG_MODE_INFO& m = modes[p.targetInfo.modeInfoIdx];
            if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                t.hasTargetMode = true;
                t.refreshNumerator = m.targetMode.targetVideoSignalInfo.vSyncFreq.Numerator;
                t.refreshDenominator = m.targetMode.targetVideoSignalInfo.vSyncFreq.Denominator;
            }
        }

        // GDI 适配器匹配
        for (const GdiAdapterRec& a : adapters) {
            if (_wcsicmp(a.name.c_str(), t.gdiDeviceName.c_str()) == 0) {
                t.gdiFound = true;
                t.gdiAttached = (a.dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
                t.gdiPrimary = (a.dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                t.gdiMirroringDriver = (a.dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0;
                t.gdiRemote = (a.dd.StateFlags & DISPLAY_DEVICE_REMOTE) != 0;
                t.gdiDeviceString = a.dd.DeviceString;
                t.gdiDeviceId = a.dd.DeviceID;
                break;
            }
        }

        // 桌面矩形
        {
            auto it = monCtx.map.find(UpperW(t.gdiDeviceName));
            if (it != monCtx.map.end() && it->second.valid) {
                t.gdiDesktopRectValid = true;
                t.gdiDesktopRect = it->second.rc;
                if (it->second.primary) t.gdiPrimary = true;
            }
        }

        // EDID 关联：以监视器设备实例 ID 精确匹配为准。
        // 仅当 DisplayConfig 未给出设备路径时，才退化为厂商 + 产品代码匹配。
        // 这样“显示器断电导致设备节点消失”会真实表现为节点缺失，不会被其它设备顶替。
        {
            auto it = edidTable.find(UpperW(t.monitorInstanceId));
            if (it != edidTable.end()) {
                t.edid = it->second;
                t.monitorDeviceNodePresent = true;
            } else if (t.monitorInstanceId.empty() &&
                       (t.edidManufactureId != 0 || t.edidProductCodeId != 0)) {
                const EdidInfo* found = nullptr;
                int matches = 0;
                for (const auto& kv : edidTable) {
                    if (kv.second.pnpIdRaw == t.edidManufactureId &&
                        kv.second.productCodeRaw == t.edidProductCodeId) {
                        found = &kv.second;
                        ++matches;
                    }
                }
                if (found && matches == 1) {
                    t.edid = *found;
                    t.monitorDeviceNodePresent = true;
                }
            }
        }

        // GDI 模式表：只对活动输出采集，避免为大量非活动路径枚举模式
        if (t.active && !t.gdiDeviceName.empty()) {
            FillGdiModes(t, refW, refH);
            // 有模式列表、却没有 EDID / 监视器设备节点 —— 这是驱动给出的通用回退列表
            t.modeListIsGenericFallback =
                (t.modeCount > 0 && !t.edid.present && !t.monitorDeviceNodePresent);
        }

        // 过滤掉既未激活、也没有连接监视器的“占位”路径
        if (IsMeaningfulDisplayTarget(t) || includePlaceholderPaths) {
            out.push_back(t);
        }
    }

    if (totalPathsOut) *totalPathsOut = (int)paths.size();
    if (diagOut) *diagOut = U82W(notes);
    return out;
}

// ---------------------------------------------------------------------------
static bool LooksLikeVirtualAdapterText(const std::wstring& text) {
    if (text.empty()) return false;
    return ContainsInsensitiveW(text, L"USB Mobile Monitor") ||
           ContainsInsensitiveW(text, L"IddDriver") ||
           ContainsInsensitiveW(text, L"Virtual Display") ||
           ContainsInsensitiveW(text, L"Indirect Display") ||
           ContainsInsensitiveW(text, L"usbmm") ||
           // 软件枚举的适配器（ROOT 枚举器）一律按虚拟显示处理
           ContainsInsensitiveW(text, L"ROOT#DISPLAY") ||
           ContainsInsensitiveW(text, L"ROOT\\DISPLAY");
}

bool IsVirtualDisplayTarget(const DisplayTarget& t) {
    if (t.gdiMirroringDriver) return true;
    // 优先适配器设备路径：镜像拓扑下 gdiDeviceString 会串成物理适配器描述
    if (LooksLikeVirtualAdapterText(t.adapterDevicePath)) return true;
    return LooksLikeVirtualAdapterText(t.gdiDeviceString);
}

// ---------------------------------------------------------------------------
// 链路健康的唯一判据：EDID 与监视器设备节点都在。
//
// 只看 EDID 是不够的：显示器断电后监视器设备节点在注册表里**仍然存在**，
// 反过来也存在"EDID 可读但设备节点已消失"的中间态。要求两者同时满足，
// 才能保证 ClassifyOutputNature / EvaluateTopology / 守卫三处结论一致。
bool IsLinkHealthy(const DisplayTarget& t) {
    return t.edid.present && t.monitorDeviceNodePresent;
}

// ---------------------------------------------------------------------------
std::wstring OutputNatureName(OutputNature n) {
    switch (n) {
        case OutputNature::Virtual:         return L"虚拟/间接显示输出";
        case OutputNature::HealthyPhysical: return L"物理输出（链路正常）";
        case OutputNature::DeadPhysical:    return L"物理输出（链路已断）";
        case OutputNature::Mixed:           return L"克隆拓扑（虚拟 + 物理混合）";
        default:                            return L"未知";
    }
}

OutputNature ClassifyOutputNature(const std::vector<DisplayTarget>& targets,
                                  const std::wstring& gdiDevice) {
    if (gdiDevice.empty()) return OutputNature::Unknown;

    const std::wstring want = UpperW(gdiDevice);
    int virt = 0, healthy = 0, dead = 0;

    for (const DisplayTarget& t : targets) {
        if (!t.active) continue;
        if (UpperW(t.gdiDeviceName) != want) continue;

        if (IsVirtualDisplayTarget(t)) { ++virt; continue; }
        // "链路健康"用统一判据（EDID + 设备节点），见 display.h 的说明
        if (IsLinkHealthy(t)) { ++healthy; continue; }
        ++dead;
    }

    if (virt == 0 && healthy == 0 && dead == 0) return OutputNature::Unknown;
    if (virt > 0 && (healthy > 0 || dead > 0)) return OutputNature::Mixed;
    if (virt > 0)                              return OutputNature::Virtual;
    if (healthy > 0 && dead > 0)               return OutputNature::Mixed;
    if (healthy > 0)                           return OutputNature::HealthyPhysical;
    return OutputNature::DeadPhysical;
}

// ---------------------------------------------------------------------------
bool IsMeaningfulDisplayTarget(const DisplayTarget& t) {
    return t.active || !t.monitorDevicePath.empty() || t.edid.present ||
           t.monitorDeviceNodePresent;
}

// ---------------------------------------------------------------------------
DisplayPowerState InferPowerStateFromTopology(const std::vector<DisplayTarget>& targets) {
    bool anyActive = false;
    bool anyActiveWithEdid = false;
    bool anyActiveWithoutEdid = false;

    for (const DisplayTarget& t : targets) {
        if (!t.active) continue;
        anyActive = true;
        if (t.edid.present) anyActiveWithEdid = true;
        else anyActiveWithoutEdid = true;
    }

    if (!anyActive) return DisplayPowerState::Off;
    if (anyActiveWithoutEdid && !anyActiveWithEdid) return DisplayPowerState::Off;
    if (anyActiveWithEdid) return DisplayPowerState::On;
    return DisplayPowerState::Unknown;
}

// ---------------------------------------------------------------------------
std::wstring PrimaryModeText(const DisplaySnapshot& snap) {
    for (const DisplayTarget& t : snap.targets) {
        if (t.active && t.gdiPrimary) return FormatModeText(t.width, t.height);
    }
    for (const DisplayTarget& t : snap.targets) {
        if (t.active) return FormatModeText(t.width, t.height);
    }
    return L"n/a";
}

// ---------------------------------------------------------------------------
DisplaySnapshot CaptureSnapshot(const std::string& trigger, UINT32 refW, UINT32 refH) {
    std::wstring diag;
    int totalPaths = 0;
    std::vector<DisplayTarget> targets =
        EnumerateDisplays(refW, refH, &diag, &totalPaths);
    return CaptureSnapshotFrom(targets, totalPaths, trigger);
}

DisplaySnapshot CaptureSnapshotFrom(const std::vector<DisplayTarget>& targets,
                                    int totalPathCount,
                                    const std::string& trigger) {
    DisplaySnapshot s;
    s.timeUtc = NowIso8601Utc();
    s.uptimeMs = MonotonicMs();
    s.trigger = trigger;

    s.targets = targets;
    s.totalPathCount = totalPathCount;

    s.powerStateInferred = InferPowerStateFromTopology(s.targets);
    s.powerState = s.powerStateInferred;   // 监听模式下会被电源通知的真实状态覆盖
    s.remoteSession = GetSystemMetrics(SM_REMOTESESSION) != 0;

    DWORD sid = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sid)) s.sessionId = sid;

    for (const DisplayTarget& t : s.targets) {
        // 只有"有意义"的输出才可能带 gdiPrimary（全量列表里的占位路径不会是主屏）
        if (t.active && t.gdiPrimary) {
            s.primaryGdiDevice = t.gdiDeviceName;
            s.primaryMode = FormatModeText(t.width, t.height);
            break;
        }
    }
    if (s.primaryMode.empty()) {
        s.primaryMode = PrimaryModeText(s);
    }
    return s;
}

// ---------------------------------------------------------------------------
static std::string ModeOf(const DisplayTarget& t) {
    return W2U8(FormatModeText(t.width, t.height));
}

void DiffSnapshots(const DisplaySnapshot& before, const DisplaySnapshot& after,
                   std::vector<std::string>* changedFields,
                   std::vector<std::string>* transitions) {
    if (!changedFields || !transitions) return;

    // 以 gdiDeviceName / 适配器+目标 ID 为键建立索引
    auto keyOf = [](const DisplayTarget& t) -> std::string {
        if (!t.gdiDeviceName.empty()) return UpperAscii(W2U8(t.gdiDeviceName));
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "T:%08X%08X:%u",
                    (unsigned)t.adapterLuid.HighPart, (unsigned)t.adapterLuid.LowPart,
                    (unsigned)t.targetId);
        return std::string(buf);
    };

    std::map<std::string, const DisplayTarget*> beforeMap;
    for (const DisplayTarget& t : before.targets) beforeMap[keyOf(t)] = &t;

    std::map<std::string, const DisplayTarget*> afterMap;
    for (const DisplayTarget& t : after.targets) afterMap[keyOf(t)] = &t;

    for (const auto& kv : afterMap) {
        const std::string& k = kv.first;
        const DisplayTarget& a = *kv.second;

        auto it = beforeMap.find(k);
        if (it == beforeMap.end()) {
            changedFields->push_back(k + ".appeared");
            transitions->push_back(k + ": (不存在) -> " +
                                   (a.active ? ModeOf(a) : std::string("已连接未激活")));
            continue;
        }

        const DisplayTarget& b = *it->second;
        std::string name = !a.gdiDeviceName.empty() ? W2U8(a.gdiDeviceName) : k;

        if (b.active != a.active) {
            changedFields->push_back(k + ".active");
            transitions->push_back(name + ": active " +
                                   (b.active ? "true" : "false") + " -> " +
                                   (a.active ? "true" : "false"));
        }
        if (b.active && a.active && (b.width != a.width || b.height != a.height)) {
            changedFields->push_back(k + ".resolution");
            transitions->push_back(name + ": " + ModeOf(b) + " -> " + ModeOf(a));
        }
        if (b.pixelFormat != a.pixelFormat) {
            changedFields->push_back(k + ".pixelFormat");
            transitions->push_back(name + ": " + W2U8(PixelFormatName(b.pixelFormat)) +
                                   " -> " + W2U8(PixelFormatName(a.pixelFormat)));
        }
        if (b.refreshNumerator != a.refreshNumerator ||
            b.refreshDenominator != a.refreshDenominator) {
            changedFields->push_back(k + ".refreshRate");
        }
        if (b.monitorDeviceNodePresent != a.monitorDeviceNodePresent) {
            changedFields->push_back(k + ".monitorDeviceNodePresent");
            transitions->push_back(name + ": 监视器设备节点 " +
                                   (b.monitorDeviceNodePresent ? "存在" : "不存在") + " -> " +
                                   (a.monitorDeviceNodePresent ? "存在" : "不存在（链路已断开）"));
        }
        if (b.edid.present != a.edid.present) {
            changedFields->push_back(k + ".edidPresent");
            transitions->push_back(name + ": EDID " +
                                   (b.edid.present ? "present" : "absent") + " -> " +
                                   (a.edid.present ? "present" : "absent"));
        } else if (b.edid.present && a.edid.present) {
            if (b.edid.preferredWidth != a.edid.preferredWidth ||
                b.edid.preferredHeight != a.edid.preferredHeight) {
                changedFields->push_back(k + ".edidPreferredMode");
                transitions->push_back(name + ": EDID 首选时序 " +
                                       W2U8(FormatModeText(b.edid.preferredWidth, b.edid.preferredHeight)) +
                                       " -> " +
                                       W2U8(FormatModeText(a.edid.preferredWidth, a.edid.preferredHeight)));
            }
        }
        // 首选模式崩塌：失去 EDID 后驱动会把该输出的首选模式降级
        if (b.hasPreferred && a.hasPreferred &&
            (b.preferredWidth != a.preferredWidth || b.preferredHeight != a.preferredHeight)) {
            changedFields->push_back(k + ".preferredMode");
            transitions->push_back(name + ": 驱动首选模式 " +
                                   W2U8(FormatModeText(b.preferredWidth, b.preferredHeight)) +
                                   " -> " +
                                   W2U8(FormatModeText(a.preferredWidth, a.preferredHeight)));
        }
        if (b.modeListIsGenericFallback != a.modeListIsGenericFallback) {
            changedFields->push_back(k + ".modeListIsGenericFallback");
            transitions->push_back(name + ": 模式表 " +
                                   (b.modeListIsGenericFallback ? "通用回退列表" : "显示器实际能力列表") +
                                   " -> " +
                                   (a.modeListIsGenericFallback ? "通用回退列表（EDID 已丢失）"
                                                                : "显示器实际能力列表"));
        }
        if (b.gdiMirroringDriver != a.gdiMirroringDriver) {
            changedFields->push_back(k + ".mirroringDriver");
        }
        if (b.modeCount != a.modeCount) {
            changedFields->push_back(k + ".modeCount");
            transitions->push_back(name + ": 模式数量 " + std::to_string(b.modeCount) +
                                   " -> " + std::to_string(a.modeCount));
        }
        if (b.referenceModeAvailable != a.referenceModeAvailable) {
            changedFields->push_back(k + ".referenceModeAvailable");
            transitions->push_back(name + ": 参考分辨率可用 " +
                                   (b.referenceModeAvailable ? "是" : "否") + " -> " +
                                   (a.referenceModeAvailable ? "是" : "否"));
        }
        if (b.gdiAttached != a.gdiAttached) {
            changedFields->push_back(k + ".attachedToDesktop");
        }
        if (b.monitorFriendlyName != a.monitorFriendlyName) {
            changedFields->push_back(k + ".monitorFriendlyName");
            transitions->push_back(name + ": 监视器名 " + W2U8(b.monitorFriendlyName) +
                                   " -> " + W2U8(a.monitorFriendlyName));
        }
        if (b.outputTechnology != a.outputTechnology) {
            changedFields->push_back(k + ".outputTechnology");
        }
        if (b.gdiDesktopRectValid && a.gdiDesktopRectValid &&
            (b.gdiDesktopRect.left != a.gdiDesktopRect.left ||
             b.gdiDesktopRect.top != a.gdiDesktopRect.top)) {
            changedFields->push_back(k + ".desktopPosition");
        }
    }

    for (const auto& kv : beforeMap) {
        if (afterMap.find(kv.first) == afterMap.end()) {
            changedFields->push_back(kv.first + ".disappeared");
            std::string name = !kv.second->gdiDeviceName.empty()
                                   ? W2U8(kv.second->gdiDeviceName)
                                   : kv.first;
            transitions->push_back(name + ": " + ModeOf(*kv.second) + " -> (已断开)");
        }
    }

    if (before.powerState != after.powerState) {
        changedFields->push_back("powerState");
        transitions->push_back("显示器电源状态(电源通知): " +
                               W2U8(DisplayPowerStateName(before.powerState)) + " -> " +
                               W2U8(DisplayPowerStateName(after.powerState)));
    }
    if (before.powerStateInferred != after.powerStateInferred) {
        changedFields->push_back("powerStateInferred");
        transitions->push_back("显示器电源状态(拓扑推断): " +
                               W2U8(DisplayPowerStateName(before.powerStateInferred)) + " -> " +
                               W2U8(DisplayPowerStateName(after.powerStateInferred)));
    }
    if (before.primaryMode != after.primaryMode) {
        changedFields->push_back("primaryMode");
        transitions->push_back("主输出分辨率: " + W2U8(before.primaryMode) +
                               " -> " + W2U8(after.primaryMode));
    }
}

}  // namespace resprobe
