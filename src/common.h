// common.h - 全局基础头文件
// 注意: 必须作为每个翻译单元的第一个头文件被包含。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <dbt.h>   // DEV_BROADCAST_* / DBT_* (WIN32_LEAN_AND_MEAN 下不会被自动包含)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// ---------------------------------------------------------------------------
// 工具版本
// ---------------------------------------------------------------------------
#define RESPROBE_NAME    "ResProbe"
#define RESPROBE_VERSION "1.0.0"

// ---------------------------------------------------------------------------
// 本文件中手工声明的 GUID，避免依赖 initguid/devguid 链接细节
// ---------------------------------------------------------------------------
// {4d36e96e-e325-11ce-bfc1-08002be10318} GUID_DEVCLASS_MONITOR
// {4d36e968-e325-11ce-bfc1-08002be10318} GUID_DEVCLASS_DISPLAY
// {e6f07b5f-ee97-4a90-b076-33f57bf4eaa7} GUID_DEVINTERFACE_MONITOR
// {6fe69556-704a-47a0-8f24-c28d936fda47} GUID_CONSOLE_DISPLAY_STATE
// ---------------------------------------------------------------------------
namespace resprobe {

const GUID kGuidDevClassMonitor = {
    0x4d36e96e, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

const GUID kGuidDevClassDisplay = {
    0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

const GUID kGuidDevInterfaceMonitor = {
    0xe6f07b5f, 0xee97, 0x4a90, { 0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7 }
};

const GUID kGuidConsoleDisplayState = {
    0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 }
};

// DISPLAYCONFIG_PATH_MODE_IDX_INVALID 在不同 SDK 中可能缺失，这里兜底
#ifndef DISPLAYCONFIG_PATH_MODE_IDX_INVALID
#define DISPLAYCONFIG_PATH_MODE_IDX_INVALID 0xFFFFFFFF
#endif

// ---------------------------------------------------------------------------
// 显示器电源状态（来源于 GUID_CONSOLE_DISPLAY_STATE 电源通知）
// ---------------------------------------------------------------------------
enum class DisplayPowerState {
    Unknown = 0,
    On = 1,
    Off = 2,
    Dimmed = 3,
};

// ---------------------------------------------------------------------------
// EDID 信息
// ---------------------------------------------------------------------------
struct EdidInfo {
    bool present = false;              // 是否读取到 EDID 数据
    bool valid = false;                // EDID 头部/校验是否有效
    // 校验和（第一个 128 字节之和为 0）。曾经算出来后就被丢弃，导致"头部合法但
    // 校验和错误"的 EDID 在报告里仍显示为"有效"，与"保留证据"的定位冲突。
    bool checksumOk = false;
    std::wstring registryPath;         // 读取来源（设备实例 ID）
    std::wstring manufacturer;         // EDID 中的三字母厂商代码
    UINT16 pnpIdRaw = 0;               // EDID[8..9]，与 DisplayConfig 的 edidManufactureId 同源
    UINT16 productCodeRaw = 0;         // EDID[10..11]，与 DisplayConfig 的 edidProductCodeId 同源
    UINT32 serialNumber = 0;           // EDID[12..15]
    std::wstring monitorName;          // 描述符 0xFC
    std::wstring serialText;           // 描述符 0xFF
    UINT16 preferredWidth = 0;         // 首选时序（描述符 0）
    UINT16 preferredHeight = 0;
    UINT32 preferredPixelClock10kHz = 0;
    int extensionCount = 0;            // EDID[126] 扩展块数量
    SIZE_T byteCount = 0;
    std::wstring rawHex;               // 原始字节（十六进制），作为证据保留
};

// ---------------------------------------------------------------------------
// 单个显示输出（一个 DisplayConfig path = 一个 source + 一个 target 的组合）
// ---------------------------------------------------------------------------
struct DisplayTarget {
    // --- DisplayConfig path 基本信息 ---
    bool active = false;
    // targetAvailable: 该输出当前是否有可用的显示接收端（显示器）。
    // 这是判断“能否把这块屏扩展进桌面”的直接依据。
    bool targetAvailable = false;
    UINT32 targetStatusFlags = 0;
    UINT32 outputTechnology = 0;
    std::wstring outputTechnologyName;
    // 适配器设备路径（来自 DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME），
    // 例如 \\?\PCI#VEN_8086&DEV_9BC8...#{guid} 或 \\?\ROOT#DISPLAY#0001#{guid}。
    // 关键用途：镜像拓扑下多个输出会共用同一个 GDI 源名（gdiDeviceString 会串味），
    // 只有适配器设备路径能可靠区分物理适配器与虚拟/间接显示适配器。
    std::wstring adapterDevicePath;
    LUID adapterLuid{};
    UINT32 targetId = 0;
    UINT32 sourceId = 0;

    // --- target 设备信息 ---
    std::wstring monitorFriendlyName;
    std::wstring monitorDevicePath;    // \\?\DISPLAY#XXX#...，原始路径
    std::wstring monitorInstanceId;    // 规范化后的设备实例 ID
    UINT16 edidManufactureId = 0;      // 来自 DisplayConfig
    UINT16 edidProductCodeId = 0;
    UINT32 connectorInstance = 0;
    bool friendlyNameFromEdid = false;

    // --- source 设备信息 ---
    std::wstring gdiDeviceName;        // 例如 \\.\DISPLAY1

    // --- 当前模式 ---
    bool hasSourceMode = false;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 pixelFormat = 0;            // DISPLAYCONFIG_PIXELFORMAT
    bool hasTargetMode = false;
    UINT32 refreshNumerator = 0;
    UINT32 refreshDenominator = 0;

    // --- 首选模式（驱动认为该输出应当使用的模式） ---
    bool hasPreferred = false;
    UINT32 preferredWidth = 0;
    UINT32 preferredHeight = 0;
    UINT32 preferredRefreshNumerator = 0;
    UINT32 preferredRefreshDenominator = 0;

    // --- GDI 侧适配器状态 ---
    bool gdiFound = false;
    bool gdiAttached = false;
    bool gdiPrimary = false;
    bool gdiMirroringDriver = false;   // 镜像/间接显示驱动（虚拟显示器的强特征）
    bool gdiRemote = false;
    std::wstring gdiDeviceString;      // 显卡/驱动描述，例如 "NVIDIA GeForce ..."
    std::wstring gdiDeviceId;          // 硬件 ID
    bool gdiDesktopRectValid = false;
    RECT gdiDesktopRect{};

    // --- GDI 模式表 ---
    int modeCount = 0;
    bool modeListTruncated = false;
    bool referenceModeAvailable = false;   // 参考分辨率（默认 1920x1080）是否仍在模式表中
    bool fallbackModeAvailable = false;    // 是否存在 1024x768 / 800x600 / 640x480
    std::wstring gdiCurrentMode;           // EnumDisplaySettingsEx(ENUM_CURRENT_SETTINGS)
    std::wstring gdiRegistryMode;          // EnumDisplaySettingsEx(ENUM_REGISTRY_SETTINGS)
    UINT32 gdiCurrentRefreshHz = 0;
    UINT32 gdiCurrentBitsPerPel = 0;
    std::vector<std::wstring> modeResolutions;

    // --- EDID ---
    EdidInfo edid;
    // 监视器设备节点是否仍存在于系统中（显示器断电导致链路断开时通常会消失）
    bool monitorDeviceNodePresent = false;
    // 模式表是否为“无 EDID 时的通用回退列表”。
    // 该列表通常仍包含 1920x1080，因此不能只用“模式表里有没有参考分辨率”来判断链路是否退化。
    bool modeListIsGenericFallback = false;
};

// ---------------------------------------------------------------------------
// 一次完整的快照
// ---------------------------------------------------------------------------
struct DisplaySnapshot {
    std::string timeUtc;
    UINT64 uptimeMs = 0;
    std::string trigger;
    DisplayPowerState powerState = DisplayPowerState::Unknown;          // 电源通知给出的状态
    DisplayPowerState powerStateInferred = DisplayPowerState::Unknown;  // 由拓扑（EDID 可见性）推断
    bool remoteSession = false;
    DWORD sessionId = 0;
    std::wstring primaryGdiDevice;
    std::wstring primaryMode;              // 例如 1920x1080
    int totalPathCount = 0;                // 系统报告的输出路径总数
    std::vector<DisplayTarget> targets;    // 有意义的输出（活动 / 已连接监视器）
};

// ---------------------------------------------------------------------------
// 会话信息
// ---------------------------------------------------------------------------
struct SessionInfo {
    bool remoteSession = false;            // GetSystemMetrics(SM_REMOTESESSION)
    DWORD sessionId = 0;
    DWORD activeConsoleSessionId = 0;
    bool isConsoleSession = true;
    DWORD protocolType = 0;                // 0=console, 1=ICA, 2=RDP
    std::wstring protocolName;
    std::wstring clientName;
    std::wstring clientAddress;
    DWORD connectState = 0;
    std::wstring connectStateName;
    std::wstring userName;
    std::wstring winStationName;
    std::wstring domainName;
    bool consoleDisplayConnected = false;  // WTSGetActiveConsoleSessionId 是否有效
    bool elevated = false;
};

// ---------------------------------------------------------------------------
// 远程/虚拟显示相关软件痕迹
// ---------------------------------------------------------------------------
struct RemoteSoftwareInfo {
    std::wstring name;      // 厂商友好名
    std::wstring kind;      // display-adapter | monitor-device | process | service
    std::wstring detail;    // 具体证据字符串
    bool isVirtualDisplay = false;
};

// ---------------------------------------------------------------------------
// 模式可用性探测结果
// ---------------------------------------------------------------------------
struct ModeProbe {
    std::wstring gdiDeviceName;
    UINT32 width = 0;
    UINT32 height = 0;
    bool deviceActive = false;
    bool listedInModeList = false;
    bool cdsTestPerformed = false;
    LONG cdsTestResult = 0;
    std::wstring cdsTestResultName;
    bool cdsTestAccepted = false;
    std::wstring currentMode;
    std::wstring registryMode;
    std::vector<std::wstring> degradedModeResolutions;  // 模式表中所有 <=1024x768 的分辨率
};

// ---------------------------------------------------------------------------
// 根因判定
// ---------------------------------------------------------------------------
struct Verdict {
    std::string code = "UNDETERMINED";
    std::string summary;
    std::string confidence = "low";
    std::string blame = "unknown";     // link | remote-software | none | unknown
    std::vector<std::string> reasons;
    std::vector<std::string> recommendations;
};

// ---------------------------------------------------------------------------
// 对端（远程客户端机器）摘要
// ---------------------------------------------------------------------------
struct PeerSummary {
    bool present = false;
    std::string timeUtc;
    std::string primaryMode;
    bool remoteSession = false;
    bool degraded = false;
    std::wstring sourcePath;
    std::string rawText;
};

// ---------------------------------------------------------------------------
// 事件记录
// ---------------------------------------------------------------------------
struct EventRecord {
    int index = 0;
    std::string timeUtc;
    std::string trigger;
    UINT64 uptimeMs = 0;
    bool changed = false;
    std::vector<std::string> changedFields;
    std::vector<std::string> transitions;
    DisplaySnapshot snapshot;
};

// ---------------------------------------------------------------------------
// 命令行选项
// ---------------------------------------------------------------------------
struct Options {
    std::wstring mode;                 // snapshot | watch | verify | paths | vdisplay | restore | ensure
    std::wstring outPath;              // 空 = 不写 JSON（默认行为）
    std::wstring outDir;               // 空 = <程序目录>\OUT
    std::wstring peerPath;
    std::wstring ensureMode = L"auto"; // ensure 专有：auto | mirror | promote
    UINT32 referenceWidth = 1920;
    UINT32 referenceHeight = 1080;
    int durationSec = 0;               // 0 = 一直运行直到 Ctrl+C
    int intervalSec = 30;              // 心跳采样间隔，0 = 关闭
    int settleMs = 800;                // 事件后等待驱动稳定的时间
    bool cdsTest = true;               // 是否执行 CDS_TEST 只读模式探测
    bool confirmed = false;            // --yes，允许回写
    bool promote = false;              // --promote，把虚拟显示输出提升为主屏并重排桌面
    bool mirror = false;               // --mirror，把虚拟显示输出与主输出复制（镜像）
    bool guard = false;                // --guard，监听中检测到物理链路断链时自动提升虚拟屏
    bool noGuard = false;              // --no-guard，tray 专有：只提供托盘界面，不做任何拓扑动作
    bool quiet = false;
    bool verbose = false;
};

// ---------------------------------------------------------------------------
// 汇总报告
// ---------------------------------------------------------------------------
struct Report {
    Options opt;
    SessionInfo session;
    std::vector<RemoteSoftwareInfo> remoteSoftware;
    DisplaySnapshot initial;
    std::vector<EventRecord> events;
    std::vector<ModeProbe> probes;
    Verdict verdict;
    PeerSummary peer;
    std::string startedUtc;
    std::string finishedUtc;
    std::wstring diagnostics;

    // 显示器电源状态通知的原始证据（时间戳 + 状态），用于和分辨率变化做时间关联
    int powerNotificationCount = 0;
    std::vector<std::string> powerTransitions;

    // 回写（restore）执行记录
    bool restoreAttempted = false;
    bool restoreSucceeded = false;
    std::string restoreResultName;
    std::string restoreMessage;
    std::vector<std::string> restoreAttempts;   // "尝试方式 -> 结果码名称"

    // 显示配置动作记录（vdisplay / guard 等）
    std::vector<std::string> actionLog;
};

}  // namespace resprobe
