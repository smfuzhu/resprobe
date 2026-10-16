// report.cpp - 控制台输出与 JSON 报告
#include "report.h"
#include "display.h"
#include "util.h"

#include <fstream>
#include <sstream>

namespace resprobe {

namespace {

// ---------------------------------------------------------------------------
// 极简 JSON 构造器
// ---------------------------------------------------------------------------
std::string JStr(const std::wstring& s) { return "\"" + JsonEscape(W2U8(s)) + "\""; }
std::string JStrU8(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }
std::string JBool(bool v) { return v ? "true" : "false"; }

std::string JInt(long long v) { return std::to_string(v); }
std::string JUInt(unsigned long long v) { return std::to_string(v); }

struct JObj {
    std::vector<std::string> items;
    void add(const std::string& k, const std::string& dumped) {
        items.push_back(JStrU8(k) + ": " + dumped);
    }
    void addStr(const std::string& k, const std::wstring& v) { add(k, JStr(v)); }
    void addBool(const std::string& k, bool v) { add(k, JBool(v)); }
    void addInt(const std::string& k, long long v) { add(k, JInt(v)); }
    std::string dump(int indent) const {
        if (items.empty()) return "{}";
        std::string pad((size_t)indent * 2, ' ');
        std::string pad2((size_t)(indent + 1) * 2, ' ');
        std::string s = "{\r\n";
        for (size_t i = 0; i < items.size(); ++i) {
            s += pad2 + items[i];
            if (i + 1 < items.size()) s += ",";
            s += "\r\n";
        }
        s += pad + "}";
        return s;
    }
};

struct JArr {
    std::vector<std::string> items;
    void add(const std::string& dumped) { items.push_back(dumped); }
    std::string dump(int indent) const {
        if (items.empty()) return "[]";
        std::string pad((size_t)indent * 2, ' ');
        std::string pad2((size_t)(indent + 1) * 2, ' ');
        std::string s = "[\r\n";
        for (size_t i = 0; i < items.size(); ++i) {
            s += pad2 + items[i];
            if (i + 1 < items.size()) s += ",";
            s += "\r\n";
        }
        s += pad + "]";
        return s;
    }
};

std::string DumpStringArray(const std::vector<std::wstring>& v, int indent) {
    JArr a;
    for (const std::wstring& s : v) a.add(JStr(s));
    return a.dump(indent);
}

std::string DumpUtf8Array(const std::vector<std::string>& v, int indent) {
    JArr a;
    for (const std::string& s : v) a.add(JStrU8(s));
    return a.dump(indent);
}

// ---------------------------------------------------------------------------
std::string DumpEdid(const EdidInfo& e, int indent, bool keepRawHex) {
    JObj o;
    o.addBool("present", e.present);
    o.addBool("valid", e.valid);
    o.addBool("checksumOk", e.checksumOk);
    o.addStr("deviceInstanceId", e.registryPath);
    o.addStr("manufacturer", e.manufacturer);
    o.add("pnpIdRaw", JUInt(e.pnpIdRaw));
    o.add("productCode", JUInt(e.productCodeRaw));
    o.add("serialNumber", JUInt(e.serialNumber));
    o.addStr("monitorName", e.monitorName);
    o.addStr("serialText", e.serialText);
    o.addInt("extensionCount", e.extensionCount);
    o.add("byteCount", JUInt(e.byteCount));

    JObj pref;
    pref.add("width", JInt(e.preferredWidth));
    pref.add("height", JInt(e.preferredHeight));
    pref.add("pixelClock10kHz", JUInt(e.preferredPixelClock10kHz));
    o.add("preferredTiming", pref.dump(indent + 1));

    // 原始字节只在活动输出上保留，避免同一台显示器的 EDID 在报告里重复多份
    if (keepRawHex) o.addStr("rawHex", e.rawHex);
    return o.dump(indent);
}

std::string DumpTarget(const DisplayTarget& t, int indent) {
    JObj o;
    o.addBool("active", t.active);
    o.addBool("targetAvailable", t.targetAvailable);
    o.add("targetStatusFlags", JUInt(t.targetStatusFlags));
    o.addStr("gdiDeviceName", t.gdiDeviceName);
    o.addStr("adapterLuid", U82W(FormatLuid(t.adapterLuid)));
    o.add("targetId", JUInt(t.targetId));
    o.add("sourceId", JUInt(t.sourceId));
    o.addStr("outputTechnology", t.outputTechnologyName);
    o.add("outputTechnologyCode", JUInt(t.outputTechnology));
    o.addStr("adapterDevicePath", t.adapterDevicePath);
    o.addBool("isVirtualDisplay", IsVirtualDisplayTarget(t));
    o.addStr("monitorFriendlyName", t.monitorFriendlyName);
    o.addStr("monitorDevicePath", t.monitorDevicePath);
    o.addStr("monitorInstanceId", t.monitorInstanceId);
    o.add("edidManufactureId", JUInt(t.edidManufactureId));
    o.add("edidProductCodeId", JUInt(t.edidProductCodeId));
    o.addBool("friendlyNameFromEdid", t.friendlyNameFromEdid);
    o.add("connectorInstance", JUInt(t.connectorInstance));

    {
        JObj cur;
        cur.addBool("available", t.hasSourceMode);
        cur.add("width", JInt(t.width));
        cur.add("height", JInt(t.height));
        cur.addStr("pixelFormat", PixelFormatName(t.pixelFormat));
        if (t.hasTargetMode && t.refreshDenominator != 0) {
            cur.add("refreshHz", JInt((long long)(t.refreshNumerator / t.refreshDenominator)));
        }
        o.add("currentMode", cur.dump(indent + 1));
    }

    {
        JObj pref;
        pref.addBool("available", t.hasPreferred);
        pref.add("width", JInt(t.preferredWidth));
        pref.add("height", JInt(t.preferredHeight));
        if (t.preferredRefreshDenominator != 0) {
            pref.add("refreshHz",
                     JInt((long long)(t.preferredRefreshNumerator / t.preferredRefreshDenominator)));
        }
        o.add("preferredMode", pref.dump(indent + 1));
    }

    {
        JObj g;
        g.addBool("found", t.gdiFound);
        g.addBool("attachedToDesktop", t.gdiAttached);
        g.addBool("primary", t.gdiPrimary);
        g.addBool("mirroringDriver", t.gdiMirroringDriver);
        g.addBool("remoteSessionDevice", t.gdiRemote);
        g.addStr("deviceString", t.gdiDeviceString);
        g.addStr("deviceId", t.gdiDeviceId);
        g.addStr("currentMode", t.gdiCurrentMode);
        g.addStr("registryMode", t.gdiRegistryMode);
        g.add("currentRefreshHz", JUInt(t.gdiCurrentRefreshHz));
        g.add("currentBitsPerPel", JUInt(t.gdiCurrentBitsPerPel));
        if (t.gdiDesktopRectValid) g.addStr("desktopRect", FormatRect(t.gdiDesktopRect));
        o.add("gdi", g.dump(indent + 1));
    }

    {
        JObj m;
        m.addInt("count", t.modeCount);
        m.addBool("truncated", t.modeListTruncated);
        m.addBool("referenceModeAvailable", t.referenceModeAvailable);
        m.addBool("fallbackModeAvailable", t.fallbackModeAvailable);
        m.addBool("isGenericFallbackList", t.modeListIsGenericFallback);
        m.add("resolutions", DumpStringArray(t.modeResolutions, indent + 2));
        o.add("modeList", m.dump(indent + 1));
    }

    o.addBool("monitorDeviceNodePresent", t.monitorDeviceNodePresent);
    o.add("edid", DumpEdid(t.edid, indent + 1, t.active));
    return o.dump(indent);
}

std::string DumpSnapshot(const DisplaySnapshot& s, int indent) {
    JObj o;
    o.addStr("timeUtc", U82W(s.timeUtc));
    o.add("uptimeMs", JUInt(s.uptimeMs));
    o.addStr("trigger", U82W(s.trigger));
    o.addStr("displayPowerState", DisplayPowerStateName(s.powerState));
    o.addStr("displayPowerStateInferred", DisplayPowerStateName(s.powerStateInferred));
    o.addBool("remoteSession", s.remoteSession);
    o.add("sessionId", JUInt(s.sessionId));
    o.addStr("primaryGdiDevice", s.primaryGdiDevice);
    o.addStr("primaryMode", s.primaryMode);
    o.addInt("totalPathCount", s.totalPathCount);
    o.addInt("reportedTargetCount", (long long)s.targets.size());

    JArr arr;
    for (const DisplayTarget& t : s.targets) arr.add(DumpTarget(t, indent + 2));
    o.add("targets", arr.dump(indent + 1));
    return o.dump(indent);
}

std::string DumpSession(const SessionInfo& si, int indent) {
    JObj o;
    o.addBool("remoteSession", si.remoteSession);
    o.add("sessionId", JUInt(si.sessionId));
    o.add("activeConsoleSessionId", JUInt(si.activeConsoleSessionId));
    o.addBool("isConsoleSession", si.isConsoleSession);
    o.addStr("protocolType", si.protocolName);
    o.add("protocolTypeCode", JUInt(si.protocolType));
    o.addStr("clientName", si.clientName);
    o.addStr("clientAddress", si.clientAddress);
    o.addStr("connectState", si.connectStateName);
    o.addStr("userName", si.userName);
    o.addStr("winStationName", si.winStationName);
    o.addStr("domainName", si.domainName);
    o.addBool("elevated", si.elevated);
    return o.dump(indent);
}

}  // namespace

// ---------------------------------------------------------------------------
std::wstring ExeDirectory() {
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (n == 0) return L".";
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return p.substr(0, slash);
}

std::wstring DefaultOutDir() {
    return ExeDirectory() + L"\\OUT";
}

bool EnsureDirectoryExists(const std::wstring& dir) {
    if (dir.empty()) return false;

    // 逐级创建，容忍已存在。
    //
    // 边界：UNC（\\server\share\...）与以分隔符开头的路径，不能从第一个字符就
    // 开始建目录 —— 在首个 "\" / "/" 处调用 CreateDirectoryW 会去创建当前驱动器
    // 根（如 "\"），那是完全错误的目标。做法是跳过开头连续的分隔符，保留
    // UNC / 根前缀原样后再从第一个真实分量开始逐级创建。
    size_t start = 0;
    while (start < dir.size() && (dir[start] == L'\\' || dir[start] == L'/')) ++start;

    std::wstring cur = dir.substr(0, start);
    for (size_t i = start; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == L'\\' || dir[i] == L'/') {
            bool isDriveRoot = (cur.size() == 3 && cur[1] == L':');
            // cur.size() > start + 1：跳过"只有前缀和分隔符"的情形，
            // 避免对根/空分量调用 CreateDirectoryW。
            if (!isDriveRoot && cur.size() > start + 1) {
                CreateDirectoryW(cur.c_str(), nullptr);
            }
        }
    }
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;

    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;

    DWORD attr = GetFileAttributesW(dir.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DefaultOutPath(const std::wstring& mode) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[128];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 L"resprobe-%s-%04d%02d%02d-%02d%02d%02d.json",
                 mode.c_str(), st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring ResolveOutPath(const Options& opt) {
    // 未显式指定 --out -> 不写报告（默认行为，避免污染目录）
    if (opt.outPath.empty()) return std::wstring();

    const std::wstring& p = opt.outPath;

    // 含目录部分则按原样使用
    if (p.find(L'\\') != std::wstring::npos ||
        p.find(L'/') != std::wstring::npos ||
        p.find(L':') != std::wstring::npos) {
        return p;
    }

    // 纯文件名 -> 统一落到输出目录
    std::wstring dir = opt.outDir.empty() ? DefaultOutDir() : opt.outDir;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    return dir + L"\\" + p;
}

std::wstring DerivePeerPath(const std::wstring& outPath) {
    std::wstring p = outPath;
    size_t dot = p.find_last_of(L'.');
    size_t slash = p.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        p = p.substr(0, dot);
    }
    return p + L".peer.json";
}

// ---------------------------------------------------------------------------
void PrintBanner() {
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "== %s %s == 显示器分辨率与远程桌面诊断工具",
                RESPROBE_NAME, RESPROBE_VERSION);
    LogLine(buf);
}

void PrintSession(const SessionInfo& si) {
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[环境] 会话ID=%lu  工作站=%s  远程会话=%s  协议=%s\n"
                "       客户端=%s  地址=%s  状态=%s  用户=%s  管理员=%s",
                (unsigned long)si.sessionId,
                W2U8(si.winStationName).c_str(),
                si.remoteSession ? "是" : "否",
                W2U8(si.protocolName).c_str(),
                W2U8(si.clientName).empty() ? "(无)" : W2U8(si.clientName).c_str(),
                W2U8(si.clientAddress).empty() ? "(无)" : W2U8(si.clientAddress).c_str(),
                W2U8(si.connectStateName).c_str(),
                W2U8(si.userName).empty() ? "(未知)" : W2U8(si.userName).c_str(),
                si.elevated ? "是" : "否");
    LogLine(buf);
}

void PrintRemoteSoftware(const std::vector<RemoteSoftwareInfo>& list) {
    if (list.empty()) {
        LogLine("[远程软件] 未发现远程控制或虚拟显示相关痕迹");
        return;
    }
    LogLine("[远程软件] 发现以下相关痕迹:");
    for (const RemoteSoftwareInfo& s : list) {
        char buf[768];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "           - [%s] %s : %s%s",
                    W2U8(s.kind).c_str(), W2U8(s.name).c_str(),
                    W2U8(s.detail).c_str(),
                    s.isVirtualDisplay ? "  (虚拟显示特征)" : "");
        LogLine(buf);
    }
}

void PrintSnapshotConsole(const DisplaySnapshot& snap, bool verbose) {
    char buf[512];
    unsigned activeCount =
        (unsigned)std::count_if(snap.targets.begin(), snap.targets.end(),
                                [](const DisplayTarget& t) { return t.active; });

    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[快照] %s 触发=%s 电源=%s(推断=%s) 主输出=%s (%s) 活动输出=%u 已连接输出=%u 路径总数=%d",
                snap.timeUtc.c_str(), snap.trigger.c_str(),
                W2U8(DisplayPowerStateName(snap.powerState)).c_str(),
                W2U8(DisplayPowerStateName(snap.powerStateInferred)).c_str(),
                W2U8(snap.primaryMode).c_str(),
                W2U8(snap.primaryGdiDevice).c_str(),
                activeCount, (unsigned)snap.targets.size(), snap.totalPathCount);
    LogLine(buf);

    for (const DisplayTarget& t : snap.targets) {
        if (!t.active && !verbose) continue;

        std::string curMode = W2U8(FormatModeText(t.width, t.height));
        std::string prefMode = t.hasPreferred
                                   ? W2U8(FormatModeText(t.preferredWidth, t.preferredHeight))
                                   : std::string("无");
        // 校验和单独可见：头部合法但校验和错误时不能笼统报"有效"
        std::string edidText;
        if (!t.edid.present)         edidText = "缺失";
        else if (!t.edid.valid)      edidText = "存在但无效";
        else if (!t.edid.checksumOk) edidText = "存在但校验和错误";
        else                         edidText = "有效";

        std::string line;
        line += "        " + W2U8(t.gdiDeviceName);
        line += t.active ? " ACTIVE" : " inactive";
        line += " | " + W2U8(t.outputTechnologyName);
        line += " | 当前=" + curMode;
        line += " | 首选=" + prefMode;
        line += " | 模式数=" + std::to_string(t.modeCount);
        line += " | 参考分辨率可用=" + std::string(t.referenceModeAvailable ? "是" : "否");
        line += " | EDID=" + edidText;
        LogLine(line);

        if (!t.gdiDeviceName.empty()) {
            std::string l2;
            l2 += "            适配器=" + W2U8(t.gdiDeviceString);
            l2 += " | 监视器=" + W2U8(t.monitorFriendlyName);
            l2 += " | 镜像驱动=" + std::string(t.gdiMirroringDriver ? "是" : "否");
            l2 += " | 桌面=" + std::string(t.gdiDesktopRectValid
                                              ? W2U8(FormatRect(t.gdiDesktopRect))
                                              : "(未知)");
            l2 += " | GDI当前=" + W2U8(t.gdiCurrentMode);
            LogLine(l2);
        }

        if (t.edid.present) {
            char l3[768];
            _snprintf_s(l3, sizeof(l3), _TRUNCATE,
                        "            EDID: 厂商=%s 产品=0x%04X 名称=%s 首选时序=%ux%u",
                        W2U8(t.edid.manufacturer).c_str(),
                        (unsigned)t.edid.productCodeRaw,
                        W2U8(t.edid.monitorName).c_str(),
                        t.edid.preferredWidth, t.edid.preferredHeight);
            LogLine(l3);
        }
    }
}

void PrintEventConsole(const EventRecord& ev) {
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[事件 #%d] %s 原因=%s 变化=%s",
                ev.index, ev.timeUtc.c_str(), ev.trigger.c_str(),
                ev.changed ? "是" : "否（拓扑未变）");
    LogLine(buf);

    for (const std::string& t : ev.transitions) {
        LogLine("        ! " + t);
    }
}

void PrintProbes(const std::vector<ModeProbe>& probes) {
    if (probes.empty()) return;
    LogLine("[验证] 模式可用性探测:");
    for (const ModeProbe& p : probes) {
        char buf[768];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "        %s 目标=%ux%u 模式表内=%s CDS_TEST=%s 当前=%s 注册表=%s",
                    W2U8(p.gdiDeviceName).c_str(), p.width, p.height,
                    p.listedInModeList ? "是" : "否",
                    p.cdsTestPerformed ? (p.cdsTestAccepted ? "预检通过" : "被拒绝") : "未执行",
                    W2U8(p.currentMode).c_str(),
                    W2U8(p.registryMode).c_str());
        LogLine(buf);

        if (!p.degradedModeResolutions.empty()) {
            std::string joined;
            for (size_t i = 0; i < p.degradedModeResolutions.size() && i < 12; ++i) {
                if (i) joined += ", ";
                joined += W2U8(p.degradedModeResolutions[i]);
            }
            LogLine("            可用的降级分辨率: " + joined);
        }
        if (p.cdsTestPerformed && !p.cdsTestAccepted) {
            LogLine("            CDS_TEST 结果码: " + W2U8(p.cdsTestResultName));
        }
    }
}

void PrintVerdict(const Verdict& v) {
    LogLine("------------------------------------------------------------------");
    LogLine("[判定] code=" + v.code);
    LogLine("[判定] 置信度=" + v.confidence + "  责任方=" + v.blame);
    LogLine("[判定] 结论: " + v.summary);
    if (!v.reasons.empty()) {
        LogLine("[判定] 依据:");
        for (const std::string& r : v.reasons) LogLine("        - " + r);
    }
    if (!v.recommendations.empty()) {
        LogLine("[判定] 建议:");
        for (const std::string& r : v.recommendations) LogLine("        - " + r);
    }
    LogLine("------------------------------------------------------------------");
}

// ---------------------------------------------------------------------------
bool WriteJsonReport(const Report& r, const std::wstring& path) {
    JObj root;

    root.addStr("tool", U82W(RESPROBE_NAME));
    root.addStr("version", U82W(RESPROBE_VERSION));
    root.addStr("mode", r.opt.mode);
    root.addStr("startedUtc", U82W(r.startedUtc));
    root.addStr("finishedUtc", U82W(r.finishedUtc));
    root.addStr("generatedUtc", U82W(NowIso8601Utc()));
    root.addStr("diagnostics", r.diagnostics);

    {
        JObj o;
        o.add("referenceWidth", JInt(r.opt.referenceWidth));
        o.add("referenceHeight", JInt(r.opt.referenceHeight));
        o.addInt("durationSec", r.opt.durationSec);
        o.addInt("intervalSec", r.opt.intervalSec);
        o.addInt("settleMs", r.opt.settleMs);
        o.addBool("cdsTest", r.opt.cdsTest);
        o.addBool("writeConfirmed", r.opt.confirmed);
        root.add("options", o.dump(1));
    }

    root.add("session", DumpSession(r.session, 1));

    {
        JArr arr;
        for (const RemoteSoftwareInfo& s : r.remoteSoftware) {
            JObj o;
            o.addStr("name", s.name);
            o.addStr("kind", s.kind);
            o.addStr("detail", s.detail);
            o.addBool("isVirtualDisplay", s.isVirtualDisplay);
            arr.add(o.dump(2));
        }
        root.add("remoteSoftware", arr.dump(1));
    }

    root.add("initialSnapshot", DumpSnapshot(r.initial, 1));

    {
        JObj o;
        o.add("notificationCount", JInt(r.powerNotificationCount));
        o.add("transitions", DumpUtf8Array(r.powerTransitions, 2));
        root.add("displayPowerNotifications", o.dump(1));
    }

    {
        JArr arr;
        for (const EventRecord& ev : r.events) {
            JObj o;
            o.add("index", JInt(ev.index));
            o.addStr("timeUtc", U82W(ev.timeUtc));
            o.add("uptimeMs", JUInt(ev.uptimeMs));
            o.addStr("trigger", U82W(ev.trigger));
            o.addBool("changed", ev.changed);
            o.add("changedFields", DumpUtf8Array(ev.changedFields, 3));
            o.add("transitions", DumpUtf8Array(ev.transitions, 3));
            o.add("snapshot", DumpSnapshot(ev.snapshot, 2));
            arr.add(o.dump(2));
        }
        root.add("events", arr.dump(1));
    }

    {
        JArr arr;
        for (const ModeProbe& p : r.probes) {
            JObj o;
            o.addStr("gdiDeviceName", p.gdiDeviceName);
            o.add("targetWidth", JInt(p.width));
            o.add("targetHeight", JInt(p.height));
            o.addBool("deviceActive", p.deviceActive);
            o.addBool("listedInModeList", p.listedInModeList);
            o.addBool("cdsTestPerformed", p.cdsTestPerformed);
            o.add("cdsTestResultCode", JInt(p.cdsTestResult));
            o.addStr("cdsTestResultName", p.cdsTestResultName);
            o.addBool("cdsTestAccepted", p.cdsTestAccepted);
            o.addStr("currentMode", p.currentMode);
            o.addStr("registryMode", p.registryMode);
            o.add("degradedModeResolutions", DumpStringArray(p.degradedModeResolutions, 3));
            arr.add(o.dump(2));
        }
        root.add("verification", arr.dump(1));
    }

    {
        JObj o;
        o.addBool("present", r.peer.present);
        o.addStr("sourcePath", r.peer.sourcePath);
        o.addStr("timeUtc", U82W(r.peer.timeUtc));
        o.addStr("primaryMode", U82W(r.peer.primaryMode));
        o.addBool("remoteSession", r.peer.remoteSession);
        o.addBool("degraded", r.peer.degraded);
        root.add("peer", o.dump(1));
    }

    {
        JObj o;
        o.addBool("attempted", r.restoreAttempted);
        o.addBool("succeeded", r.restoreSucceeded);
        o.addStr("resultName", U82W(r.restoreResultName));
        o.addStr("message", U82W(r.restoreMessage));
        o.add("attempts", DumpUtf8Array(r.restoreAttempts, 2));
        root.add("restore", o.dump(1));
    }

    root.add("actionLog", DumpUtf8Array(r.actionLog, 1));

    {
        JObj o;
        o.addStr("code", U82W(r.verdict.code));
        o.addStr("blame", U82W(r.verdict.blame));
        o.addStr("confidence", U82W(r.verdict.confidence));
        o.addStr("summary", U82W(r.verdict.summary));
        o.add("reasons", DumpUtf8Array(r.verdict.reasons, 2));
        o.add("recommendations", DumpUtf8Array(r.verdict.recommendations, 2));
        root.add("verdict", o.dump(1));
    }

    std::string text = root.dump(0);
    text += "\r\n";

    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), (std::streamsize)text.size());
    out.close();
    return out.good() || out.eof();
}

// ---------------------------------------------------------------------------
bool WritePeerSummary(const Report& r, const std::wstring& path) {
    JObj root;
    root.addStr("tool", U82W(RESPROBE_NAME));
    root.addStr("version", U82W(RESPROBE_VERSION));
    root.addStr("generatedUtc", U82W(NowIso8601Utc()));
    root.addStr("mode", r.opt.mode);

    root.addStr("hostName", r.session.winStationName);
    root.addStr("userName", r.session.userName);
    root.addBool("remoteSession", r.session.remoteSession);
    root.add("sessionId", JUInt(r.session.sessionId));
    root.addStr("protocolType", r.session.protocolName);

    root.addStr("timeUtc", U82W(NowIso8601Utc()));
    root.addStr("primaryMode", r.initial.primaryMode);
    root.addStr("primaryGdiDevice", r.initial.primaryGdiDevice);

    UINT32 w = 0, h = 0;
    bool degraded = false;
    if (swscanf_s(r.initial.primaryMode.c_str(), L"%ux%u", &w, &h) == 2) {
        degraded = (w <= 1024 && h <= 768);
    }
    root.addBool("degraded", degraded);

    JArr targets;
    for (const DisplayTarget& t : r.initial.targets) {
        if (!t.active) continue;
        JObj o;
        o.addStr("gdiDeviceName", t.gdiDeviceName);
        o.addStr("mode", FormatModeText(t.width, t.height));
        o.addStr("outputTechnology", t.outputTechnologyName);
        o.addBool("active", t.active);
        o.addBool("primary", t.gdiPrimary);
        o.addBool("edidPresent", t.edid.present);
        o.addStr("monitorName", t.monitorFriendlyName);
        targets.add(o.dump(2));
    }
    root.add("activeTargets", targets.dump(1));

    std::string text = root.dump(0);
    text += "\r\n";

    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), (std::streamsize)text.size());
    out.close();
    return true;
}

}  // namespace resprobe
