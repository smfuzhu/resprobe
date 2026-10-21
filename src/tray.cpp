// tray.cpp - 系统托盘图标与右键菜单实现
#include "tray.h"

#include "report.h"
#include "util.h"
#include "watcher.h"   // RequestStop：菜单里"退出"复用 Ctrl+C 的停机路径

#include <wtsapi32.h>  // WM_WTSSESSION_CHANGE / WTS_SESSION_LOCK（隐私屏生命周期兜底）

#include <cstdio>

namespace resprobe {

namespace {

const wchar_t kTrayClassName[] = L"ResProbeTrayHost";

// 自定义回调消息：Shell_NotifyIcon 会把鼠标事件投递到这个消息上
const UINT kTrayCallbackMsg = WM_APP + 101;
// 连点切换请求：由 mouse hook 回调 PostMessage 过来（钩子里不能直接开关窗口）
const UINT kTrayPrivacyTapMsg = WM_APP + 102;

// 资源脚本里的图标 ID（见 res/resprobe.rc）
const int kIconResourceId      = 1;   // resprobe.ico        默认
const int kIconGreenResourceId = 2;   // resprobe-green.ico  显示器在线
const int kIconBlueResourceId  = 3;   // resprobe-blue.ico   显示输出已关闭
const int kIconRedResourceId   = 4;   // resprobe-red.ico    无兜底
const int kIconYellowResourceId= 5;   // resprobe-yellow.ico 兜底中
const int kIconPrivacyResourceId = 6; // resprobe-privacy.ico 隐私屏（紫底 + 闭眼横条）

// 六种形态在托盘提示里的后缀（与图标一一对应，鼠标悬停时给一层文字确认）
const wchar_t* TrayStateSuffix(TrayIconState s) {
    switch (s) {
        case TrayIconState::MonitorOnline:  return L" · 显示器在线";
        case TrayIconState::DisplayOff:     return L" · 显示输出已关闭";
        case TrayIconState::FallbackActive: return L" · 兜底中";
        case TrayIconState::NoFallback:     return L" · 无兜底";
        case TrayIconState::PrivacyScreen:  return L" · 隐私屏中";
        default:                            return L"";
    }
}

// 托盘自身的状态刷新定时器。
//
// 为什么必须加：图标要反映"显示器是否还在线"，而这条信息只存在于周期性探测里
// （EDID 走 SetupAPI，没有事件通知）。电源状态是事件驱动的、到得很快，但物理
// 显示器的插拔/掉电只有 WM_DEVICECHANGE 这种粗粒度信号，靠它不准。
// 5 秒一次是最折中的选择：用户能感觉到变化，单次 QueryTrayStatus 又是毫秒级。
const UINT_PTR kTrayRefreshTimerId = 2;
const UINT kTrayRefreshIntervalMs = 5000;

// 菜单命令 ID
enum MenuId : int {
    kIdStatus        = 1001,
    kIdPaths         = 1002,
    kIdOpenOut       = 1003,
    kIdViewLog       = 1004,
    kIdAbout         = 1005,
    kIdExit          = 1006,

    kIdResBase       = 1100,   // + 索引
    kIdResLast       = 1199,

    kIdAutoGuard     = 1301,
    kIdAutoOnce      = 1302,
    kIdAutoOff       = 1303,
    kIdAutoStatus    = 1304,
    kIdPrivacy       = 1305,

    kIdInstall       = 1401,
    kIdUninstall     = 1402,
    kIdUninstallFull = 1403,
};

// 状态缓存有效期：右键菜单不应每次都做完整探测（EDID 走 SetupAPI，有毫秒级成本）。
// 代价是菜单首行最多滞后这么久；任何动作之后都会强制刷新。
const UINT64 kStatusTtlMs = 8000;
const UINT64 kModesTtlMs  = 8000;

std::wstring FormatCount(int n) {
    return std::to_wstring(n);
}

}  // namespace

// ===========================================================================
TrayIcon::TrayIcon() {}

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create(UINT32 refW, UINT32 refH) {
    refW_ = refW;
    refH_ = refH;
    inst_ = GetModuleHandleW(nullptr);

    if (!CreateHostWindow()) return false;

    icon_ = LoadIconW(inst_, MAKEINTRESOURCEW(kIconResourceId));
    if (!icon_) {
        // 资源缺失时退化到系统图标，保证托盘仍可用
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }
    iconGreen_ = LoadIconW(inst_, MAKEINTRESOURCEW(kIconGreenResourceId));
    iconBlue_  = LoadIconW(inst_, MAKEINTRESOURCEW(kIconBlueResourceId));
    iconRed_   = LoadIconW(inst_, MAKEINTRESOURCEW(kIconRedResourceId));
    iconYellow_= LoadIconW(inst_, MAKEINTRESOURCEW(kIconYellowResourceId));
    iconPrivacy_ = LoadIconW(inst_, MAKEINTRESOURCEW(kIconPrivacyResourceId));

    LogLine("[托盘] 图标资源: 默认=" + std::string(icon_ ? "OK" : "缺失") +
            " 绿(在线)=" + std::string(iconGreen_ ? "OK" : "缺失") +
            " 蓝(显示输出已关闭)=" + std::string(iconBlue_ ? "OK" : "缺失") +
            " 红(无兜底)=" + std::string(iconRed_ ? "OK" : "缺失") +
            " 黄(兜底中)=" + std::string(iconYellow_ ? "OK" : "缺失") +
            " 紫(隐私屏)=" + std::string(iconPrivacy_ ? "OK" : "缺失"));

    // 显示器电源状态（开/关/低功耗）。
    //
    // 托盘不能复用 Watcher 的注册：那是注册在 Watcher 自己的窗口上的，消息只会
    // 投递到那个窗口。托盘窗口是同一线程上的另一个顶层窗口，必须自己注册一份。
    // 注册失败不致命 —— 只是拿不到"Windows 关了显示输出"，图标退回默认形态。
    powerNotify_ = RegisterPowerSettingNotification(
        hwnd_, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    LogLine("[托盘] 显示器电源状态通知注册" +
            std::string(powerNotify_ ? "成功" : "失败（图标不区分显示输出关闭）"));

    // 会话通知（锁屏/解锁/注销）：同样必须由托盘自己注册一份，理由与上面相同 ——
    // Watcher 的 WTSRegisterSessionNotification 只把消息投递给它自己的窗口。
    // 用途是隐私屏的生命周期兜底（见 StopPrivacyForLifecycle）。
    // 注册失败不致命：只是失去"锁屏时自动撤罩"，隐私屏本身仍可用。
    wtsRegistered_ = (WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE);
    LogLine("[托盘] 会话通知注册" +
            std::string(wtsRegistered_ ? "成功" : "失败（锁屏时不会自动撤除隐私屏）"));

    AddNotificationIcon();

    // 图标注册失败（例如 explorer.exe 未运行）时必须让调用方知道。
    // 否则进程在跑、控制台被隐藏、托盘却没有图标 —— 用户只会认为程序没启动。
    if (!iconInstalled_) {
        return false;
    }

    // 周期刷新：让图标在显示器插拔/掉电后能自己变回去，不必等用户右键。
    refreshTimerId_ = SetTimer(hwnd_, kTrayRefreshTimerId, kTrayRefreshIntervalMs, nullptr);
    if (!refreshTimerId_) {
        LogLine("[托盘] 状态刷新定时器创建失败（图标只在右键/电源事件时更新）");
    }

    // 先做一次探测，让第一次右键就有内容
    RefreshStatusCache(true);
    RefreshModeCache(true);
    RefreshTip();

    // 连点解除的通知目标。钩子本身只在隐私屏开启期间安装（见 privacy.cpp），
    // 这里只是告诉它"数满了往哪个窗口发消息"。
    privacy_.SetNotifyTarget(hwnd_, kTrayPrivacyTapMsg);

    return true;
}

void TrayIcon::Destroy() {
    if (powerNotify_) {
        UnregisterPowerSettingNotification(powerNotify_);
        powerNotify_ = nullptr;
    }
    if (hwnd_ && wtsRegistered_) {
        WTSUnRegisterSessionNotification(hwnd_);
        wtsRegistered_ = false;
    }
    if (hwnd_ && refreshTimerId_) {
        KillTimer(hwnd_, refreshTimerId_);
        refreshTimerId_ = 0;
    }
    // 退出前若隐私屏还开着必须先撤掉遮罩与钩子。否则用户回来面对的是一片黑，
    // 而且本地键鼠还被吞着，完全没有线索（这些状态只在本进程内存里）。
    if (privacy_.Active()) {
        LogLine("[托盘] 隐私屏 -> 进程退出，撤除遮罩并恢复输入");
        privacy_.Stop();
    }
    RemoveNotificationIcon();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
bool TrayIcon::CreateHostWindow() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kTrayClassName;

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    // 必须是顶层窗口（不能是 HWND_MESSAGE）：TrackPopupMenu 要求 owner 能被置为前台
    hwnd_ = CreateWindowExW(0, kTrayClassName, L"ResProbeTray", WS_OVERLAPPED,
                            0, 0, 0, 0, nullptr, nullptr, inst_, this);
    if (!hwnd_) return false;

    // 不调用 ShowWindow，窗口保持隐藏
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    return true;
}

void TrayIcon::AddNotificationIcon() {
    if (!hwnd_ || iconInstalled_) return;

    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kTrayCallbackMsg;
    nid_.hIcon = icon_;
    wcsncpy_s(nid_.szTip, L"ResProbe", _TRUNCATE);

    // 刻意不调用 NIM_SETVERSION：保持旧版回调语义
    // （lParam 直接就是鼠标消息），处理起来最直观。
    iconInstalled_ = (Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE);
}

void TrayIcon::RemoveNotificationIcon() {
    if (!iconInstalled_) return;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    iconInstalled_ = false;
}

void TrayIcon::SetTip(const std::wstring& tip) {
    if (!iconInstalled_) return;

    std::wstring t = tip;
    if (t.size() > 120) t.resize(120);
    wcsncpy_s(nid_.szTip, t.c_str(), _TRUNCATE);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

// ---------------------------------------------------------------------------
void TrayIcon::RefreshStatusCache(bool force) {
    const UINT64 now = MonotonicMs();
    if (!force && statusStampMs_ != 0 && (now - statusStampMs_) < kStatusTtlMs) {
        return;
    }
    status_ = QueryTrayStatus(refW_, refH_);
    statusStampMs_ = now;

    // 状态刚刷新完，同步一次图标与提示（显示器在线与否只存在于这份探测结果里）
    UpdateTrayIcon();
    RefreshTip();
}

// ---------------------------------------------------------------------------
// 五种形态的判定（纯函数）
// ---------------------------------------------------------------------------
TrayIconState DecideTrayIconState(const TrayStatus& st, DisplayPowerState power) {
    // primaryMode 为空 = 这次探测什么都没采到（枚举失败，或当前没有任何活动输出）。
    // 此时不能判"无兜底"—— 那会把一次瞬时失败报成红色告警。
    if (st.primaryMode.empty()) return TrayIconState::Default;

    if (st.virtualAvailable <= 0) {
        // 没有虚拟显示输出可用：物理屏一旦断电就会掉分辨率且无人接
        return TrayIconState::NoFallback;
    }
    if (st.healthyPhysical <= 0 && st.totalActive > 0) {
        // 没有健康的物理屏，但仍有输出在活动 -> 虚拟屏正扛着桌面
        return TrayIconState::FallbackActive;
    }
    if (power == DisplayPowerState::Off) {
        // Windows 关闭了显示输出（空闲 / 锁屏）；此时物理屏通常还在线
        return TrayIconState::DisplayOff;
    }
    if (st.healthyPhysical > 0) {
        // 活动输出带 EDID 且监视器设备节点存在
        return TrayIconState::MonitorOnline;
    }
    return TrayIconState::Default;
}

// ---------------------------------------------------------------------------
void TrayIcon::UpdateTrayIcon() {
    if (!iconInstalled_) return;

    // 隐私屏是用户主动发起的覆盖项，遮罩存在就显示紫色，压过其余全部状态。
    // 现在它不再依赖"显示输出是否已关闭"—— 遮罩是否显示完全由我们自己掌控，
    // 所以图标与实际情况天然一致，不会出现"屏幕亮着却显示隐私屏中"。
    const TrayIconState want =
        privacy_.Active() ? TrayIconState::PrivacyScreen
                          : DecideTrayIconState(status_, powerState_);

    if (want == iconState_) return;

    HICON h = icon_;
    if (want == TrayIconState::MonitorOnline) {
        h = iconGreen_ ? iconGreen_ : icon_;
    } else if (want == TrayIconState::DisplayOff) {
        h = iconBlue_ ? iconBlue_ : icon_;
    } else if (want == TrayIconState::FallbackActive) {
        h = iconYellow_ ? iconYellow_ : icon_;
    } else if (want == TrayIconState::NoFallback) {
        h = iconRed_ ? iconRed_ : icon_;
    } else if (want == TrayIconState::PrivacyScreen) {
        h = iconPrivacy_ ? iconPrivacy_ : icon_;
    }
    if (!h) return;

    iconState_ = want;
    nid_.uFlags = NIF_ICON;
    nid_.hIcon = h;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);

    const char* name = (want == TrayIconState::MonitorOnline)  ? "绿(显示器在线)"
                     : (want == TrayIconState::DisplayOff)     ? "蓝(显示输出已关闭)"
                     : (want == TrayIconState::FallbackActive) ? "黄(兜底中)"
                     : (want == TrayIconState::NoFallback)     ? "红(无兜底)"
                     : (want == TrayIconState::PrivacyScreen)  ? "紫(隐私屏中)"
                                                               : "默认";
    LogLine(std::string("[托盘] 图标 -> ") + name);

    // 图标换了，提示必须跟着换，否则悬停看到的还是旧状态
    RefreshTip();
}

// ---------------------------------------------------------------------------
// 隐私屏
// ---------------------------------------------------------------------------
void TrayIcon::TogglePrivacyScreen() {
    if (!iconInstalled_) return;

    if (privacy_.Active()) {
        privacy_.Stop();
        LogLine("[托盘] 隐私屏 -> 已关闭（遮罩撤除，本地键鼠恢复）");
        UpdateTrayIcon();
        RefreshTip();
        // 关闭确认同样用气球。此时遮罩已撤、输入已恢复，模态框本身不是死结，
        // 但"托盘线程零阻塞"是隐私屏在位期间的硬规则，不分开关方向 ——
        // 而且气球本地/远端都看得见，满足"解除时双向可见"的需求。
        ShowBalloon(L"隐私屏已关闭",
                    L"本地屏幕已恢复，本地键鼠已恢复正常操作。");
        return;
    }

    if (!privacy_.Start()) {
        // Start 失败时不会留下黑窗口（捕获排除校验不过就直接放弃），
        // 所以这里可以安全地弹窗告诉用户原因 —— 屏幕还是亮的。
        LogLine("[托盘] 隐私屏 -> 启动失败: " + W2U8(privacy_.LastError()));
        Info(L"无法开启隐私屏", privacy_.LastError());
        return;
    }

    UpdateTrayIcon();
    RefreshTip();

    // 开启确认用托盘气球，**不能用模态弹窗**（2026-10-21 真机踩坑，两条都中过）：
    //
    //   1) 模态弹窗无期限扣押托盘线程：消息泵困在 MessageBox 里，本地发起的一切
    //      同步动作全部挂起，且没有超时 —— 没人点掉就一直卡（实证：卡住数小时，
    //      最后靠 20 连点逃生，OUT\tray.log 里"已开启"与"连点解除触发"隔着整段）。
    //   2) 开启确认框"本地看得见却点不动"：它在 Start() 之后弹，钩子已经在吞
    //      物理点击；而 Info() 用 owner=nullptr + MB_SETFOREGROUND 追求的正是
    //      "不被其它窗口挡住"的顶层对话框，它会出现在 WS_EX_TOPMOST 遮罩**之上**
    //      —— 并非早先注释假设的"被遮罩盖住、本地看不到"。于是本地看到一个
    //      点不掉的框，与"开启时本地看不到"的设计意图直接冲突，只有远控注入
    //      点击或 20 连点能解，这正是要避免的死结。
    //
    // 气球两个问题都没有：不阻塞线程、不需要点击、自动消失；远端照样可见
    // （explorer 的 UI 正常被捕获）。被系统策略静默时也有图标变紫 + 提示变更
    // + 日志三重兜底，确认信息不丢。
    // 文案里只列托盘菜单一条关闭途径：连点解除是**彩蛋**，不能在界面里提示 ——
    // 否则等于把"本地任何人都能解除隐私屏"的方法直接告诉使用者。
    ShowBalloon(L"隐私屏已开启",
                L"本地屏幕已变黑，本地键鼠已屏蔽；\n"
                L"远程端仍可正常看到并操作桌面。\n\n"
                L"如需关闭，请使用托盘菜单「关闭隐私屏」。");
}

// ---------------------------------------------------------------------------
// 隐私屏的生命周期兜底：锁屏 / 休眠 / 注销时主动撤罩
// ---------------------------------------------------------------------------
// 这里的每一条都是真机场景，不是防御性编程：
//
//   * 空闲休眠：开着隐私屏走开，系统按电源计划休眠；唤醒后遮罩还在、键鼠还在被吞，
//     本地看起来就是"黑屏死机"。而连点 20 下这条逃生路径**刻意没写进界面文案**
//     （见 TogglePrivacyScreen 末尾注释），所以不能指望用户记得。
//   * Win+L：登录界面属于安全桌面，钩子拦不住也管不到；用户输完密码回到用户桌面，
//     迎接他的是同一块黑屏 —— 能登录、不能用，比进不去更让人以为机器坏了。
//   * 注销 / 关机：进程会被强杀，遮罩本身随进程消失，但仍先主动撤一次，
//     保证"进程还活着的这段时间"里不会再吞输入。
//
// 三条语义约束：
//   1) **只撤除，不恢复**。隐私屏是用户的显式意图，系统替用户重开比不重开更糟。
//   2) **不弹窗**。这三条路径上用户多半看不到窗口（休眠中 / 锁屏界面 / 另一会话），
//      而且 PBT_APMSUSPEND 只给约 2 秒，弹模态框会拖住挂起流程。
//   3) 撤除后立刻重判图标：第 6 态（紫）的唯一依据是 privacy_.Active()，
//      不重判会留下"屏幕亮着却显示隐私屏中"的谎报。
void TrayIcon::StopPrivacyForLifecycle(const char* reason) {
    if (!privacy_.Active()) return;
    privacy_.Stop();
    LogLine(std::string("[隐私屏] 已自动撤除（") + reason +
            "）：遮罩移除，本地键鼠恢复；不会自动重新开启");
    UpdateTrayIcon();
    RefreshTip();
}

// ---------------------------------------------------------------------------
void TrayIcon::RefreshTip() {
    if (!iconInstalled_) return;

    std::wstring mode = status_.primaryMode.empty() ? L"(状态未知)" : status_.primaryMode;
    const std::wstring tip = L"ResProbe " + mode + TrayStateSuffix(iconState_);
    if (tip == lastTip_) return;   // 没变就不发 NIM_MODIFY
    lastTip_ = tip;
    SetTip(tip);
    // 提示文本无法从外部读取，落一条日志才能核对"图标与提示是否同步"
    LogLine("[托盘] 提示 -> " + W2U8(tip));
}

// ---------------------------------------------------------------------------
void TrayIcon::OnPowerSetting(WPARAM, LPARAM lp) {
    auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
    if (!setting) return;
    if (!IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState)) return;
    if (setting->DataLength < sizeof(DWORD)) return;

    const DWORD state = *(DWORD*)setting->Data;
    powerState_ = (state == 0) ? DisplayPowerState::Off
                : (state == 1) ? DisplayPowerState::On
                : (state == 2) ? DisplayPowerState::Dimmed
                               : DisplayPowerState::Unknown;

    LogLine("[托盘] 电源通知(显示器状态)=" + W2U8(DisplayPowerStateName(powerState_)));

    // 注意：隐私屏不再依赖这个通知。
    // 早先的实现用 SC_MONITORPOWER 关闭显示输出来做隐私屏，必须靠 =OFF/=ON 确认
    // 与收尾；但 RustDesk 持有 DISPLAY 电源请求会立刻把输出要回去，那条路已废弃。
    // 现在隐私屏是全屏遮罩 + 捕获排除，遮罩在不在完全由我们自己掌控。

    // 事件驱动，无需等定时器：这里直接重判一次图标
    UpdateTrayIcon();
}

void TrayIcon::RefreshModeCache(bool force) {
    const UINT64 now = MonotonicMs();
    if (!force && modesStampMs_ != 0 && (now - modesStampMs_) < kModesTtlMs) {
        return;
    }
    modes_ = GetPrimaryModeList(&currentMode_, nullptr);
    modesStampMs_ = now;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK TrayIcon::TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayIcon* self = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<TrayIcon*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->OnMessage(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayIcon::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // explorer.exe 重启后任务栏会重建，需要重新注册图标，否则图标消失
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        iconInstalled_ = false;
        AddNotificationIcon();
        RefreshTip();
        // NIM_ADD 用的是默认图标，这里必须重判一次，否则 explorer 重启后
        // "在线/已关闭"的形态会丢失。
        iconState_ = TrayIconState::Default;
        lastTip_.clear();   // NIM_ADD 会把提示重置成 "ResProbe"，必须重发
        UpdateTrayIcon();
        RefreshTip();
        return 0;
    }

    switch (msg) {
        case kTrayCallbackMsg: {
            switch (LOWORD(lp)) {
                // 左键与右键都弹出菜单：左键点开更顺手，也是很多托盘程序的习惯。
                // 状态报告仍然可以从菜单里的「完整状态报告...」打开，功能没有丢失。
                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowContextMenu();
                    return 0;

                case WM_LBUTTONDBLCLK:
                    // 第一次点击已经弹出菜单，双击的第二次点击由菜单的模态循环吃掉。
                    // 这里如果再做别的动作，会与菜单叠在一起。
                    return 0;

                default:
                    return 0;
            }
        }

        case WM_POWERBROADCAST:
            if (wp == PBT_POWERSETTINGCHANGE) {
                OnPowerSetting(wp, lp);
                return TRUE;
            }
            if (wp == PBT_APMSUSPEND) {
                // 挂起前把遮罩撤掉：否则唤醒后是"黑屏 + 键鼠无响应"。
                // Stop() 是同步的 DestroyWindow + UnhookWindowsHookEx，这里来得及。
                StopPrivacyForLifecycle("系统进入睡眠");
                return TRUE;   // 必须回 TRUE，系统才会在恢复时发 PBT_APMRESUMESUSPEND
            }
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                // 恢复后不重开隐私屏（见 StopPrivacyForLifecycle 的语义约束 1），
                // 但拓扑与电源状态大概率已经变了，强制重判一次图标。
                RefreshStatusCache(true);
                return TRUE;
            }
            break;

        case WM_WTSSESSION_CHANGE:
            // 通知投递给"注册它的窗口"，所以这条分支只有在托盘自己注册成功时才会到
            // （见 Create 里的 WTSRegisterSessionNotification）。
            switch (wp) {
                case WTS_SESSION_LOCK:
                    // Win+L / 空闲锁屏：解锁后能登录却不能用，必须撤罩。
                    StopPrivacyForLifecycle("会话锁定");
                    return 0;
                case WTS_SESSION_LOGOFF:
                    StopPrivacyForLifecycle("会话注销");
                    return 0;
                case WTS_SESSION_UNLOCK:
                    // 解锁后不重开，但屏幕刚恢复，缓存的状态多半已过期
                    RefreshStatusCache(true);
                    return 0;
                default:
                    // 远程连接/断开等：隐私屏不关心，交给 watcher 记录
                    return 0;
            }

        case kTrayPrivacyTapMsg:
            // 连点 20 下（10 秒内）：钩子只负责投递，这里在主线程真正执行。
            // **单向 —— 只解除，不开启**。开启方向要靠常驻鼠标钩子，代价不值得。
            // 消息可能在别处已经关掉隐私屏之后才被处理，所以再确认一次当前状态。
            if (privacy_.Active()) {
                LogLine("[隐私屏] 连点解除触发（10 秒内 20 次鼠标按下）");
                TogglePrivacyScreen();
            }
            return 0;

        case WM_TIMER:
            if (wp == kTrayRefreshTimerId) {
                // 显示器在线与否没有事件通知，只能靠周期探测
                RefreshStatusCache(true);
                return 0;
            }
            break;

        case WM_CLOSE:
            // 菜单里的"退出"走这里，避免在 WndProc 内部同步 DestroyWindow 自己
            RequestStop();
            DestroyWindow(hwnd_);
            return 0;

        case WM_ENDSESSION:
            // 注销/关机：遮罩窗口本来会随进程消失，但进程被强杀前可能还有几秒，
            // 期间不该继续吞本地输入。顺手撤一次，代价为零。
            StopPrivacyForLifecycle("注销或关机");
            // 立刻摘掉图标，避免留下"幽灵图标"
            RemoveNotificationIcon();
            return 0;

        case WM_DESTROY:
            RemoveNotificationIcon();
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            // 窗口已进入销毁流程：必须把句柄清掉。
            // 否则 Run() 返回后调用的 Destroy() 会对一个已经销毁的 HWND 再调
            // DestroyWindow / Shell_NotifyIcon，属悬垂句柄使用。
            RemoveNotificationIcon();
            hwnd_ = nullptr;
            return DefWindowProcW(hwnd, msg, wp, lp);

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
void TrayIcon::ShowContextMenu() {
    if (!hwnd_ || busy_) return;

    RefreshStatusCache(false);
    RefreshModeCache(false);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // ---- 状态首行（不可点，仅展示）-----------------------------------------
    {
        std::wstring line = status_.primaryMode.empty() ? L"(状态未知)" : status_.primaryMode;
        line += L"   虚拟屏 " + FormatCount(status_.virtualAvailable);
        if (status_.deadPhysical > 0) {
            line += L"   断链 " + FormatCount(status_.deadPhysical);
        }
        line += L"   ";
        if (!guardEnabled_) {
            line += L"守卫:关";
        } else if (status_.guardOwnedByOther) {
            line += L"守卫:其它实例";
        } else {
            line += L"守卫:本实例";
        }
        if (status_.degraded) line += L"   [异常]";
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdStatus, L"完整状态报告...");
    AppendMenuW(menu, MF_STRING, kIdPaths, L"列出显示输出路径...");

    // ---- 隐私屏 -------------------------------------------------------------
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdPrivacy,
                privacy_.Active() ? L"关闭隐私屏" : L"开启隐私屏");

    // ---- 分辨率 -------------------------------------------------------------
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        HMENU res = CreatePopupMenu();
        if (modes_.empty()) {
            AppendMenuW(res, MF_STRING | MF_GRAYED, 0, L"(未取到模式列表)");
        }
        const size_t kMaxItems = (size_t)(kIdResLast - kIdResBase + 1);
        const size_t shown = (modes_.size() < kMaxItems) ? modes_.size() : kMaxItems;
        for (size_t i = 0; i < shown; ++i) {
            UINT flags = MF_STRING;
            if (modes_[i].text == currentMode_) flags |= MF_CHECKED;
            AppendMenuW(res, flags, (UINT_PTR)(kIdResBase + (int)i),
                        modes_[i].text.c_str());
        }
        // 超过菜单容量时必须**明说**，否则用户会以为列表就这么多，无从察觉被截断
        if (modes_.size() > shown) {
            AppendMenuW(res, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(res, MF_STRING | MF_GRAYED, 0,
                        (L"… 另有 " + std::to_wstring(modes_.size() - shown) +
                         L" 项未显示（菜单上限 " + std::to_wstring(kMaxItems) +
                         L" 项）").c_str());
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)res,
                    (L"调整分辨率" + (currentMode_.empty() ? std::wstring()
                                                            : L"（当前 " + currentMode_ + L"）"))
                        .c_str());
    }

    // ---- 开机自启动 ---------------------------------------------------------
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        HMENU autoMenu = CreatePopupMenu();
        AppendMenuW(autoMenu, MF_STRING, kIdAutoGuard,
                    L"启用（虚拟屏初始化 + 常驻守卫）");
        AppendMenuW(autoMenu, MF_STRING, kIdAutoOnce, L"启用（仅虚拟屏初始化）");
        AppendMenuW(autoMenu, MF_STRING, kIdAutoOff, L"停用（移除全部任务）");
        AppendMenuW(autoMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(autoMenu, MF_STRING, kIdAutoStatus, L"查看当前状态...");

        AppendMenuW(menu, MF_POPUP, (UINT_PTR)autoMenu, L"开机自启动");
    }

    // ---- 虚拟显示器 ---------------------------------------------------------
    // 隐私屏在位时置灰脚本类动作：它们同步等待脚本进程（超时 5 分钟），会停掉
    // 遮罩的 100ms 重抢并触发 LL 钩子超时丢点，等于在隐私屏上开窗（privacy.h）。
    // 置灰只是体验层，真正的拦截在 ActRunScriptAction 里（防其它入口）。
    {
        const UINT scriptFlags = privacy_.Active() ? (MF_STRING | MF_GRAYED) : MF_STRING;
        HMENU vd = CreatePopupMenu();
        AppendMenuW(vd, scriptFlags, kIdInstall, L"安装 / 修复");
        AppendMenuW(vd, scriptFlags, kIdUninstall, L"卸载（保留驱动）");
        AppendMenuW(vd, scriptFlags, kIdUninstallFull, L"彻底卸载（移除驱动）");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)vd, L"虚拟显示器");
    }

    // ---- 文件与退出 ---------------------------------------------------------
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdViewLog, L"查看最近的日志...");
    AppendMenuW(menu, MF_STRING, kIdOpenOut, L"打开输出目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdAbout, L"关于 ResProbe");
    AppendMenuW(menu, MF_STRING, kIdExit, L"退出");

    POINT pt;
    GetCursorPos(&pt);

    // TrackPopupMenu 的经典要求：先把自己置前台，否则点击菜单外部不会关闭
    SetForegroundWindow(hwnd_);
    const int cmd = (int)TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    // 同样来自经典做法：置前台后需补一条消息，否则菜单可能"粘住"
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (cmd != 0) {
        HandleCommand(cmd);
    }
}

// ---------------------------------------------------------------------------
bool TrayIcon::HandleCommand(int id) {
    if (id == 0) return true;
    if (busy_) {
        // 上一个操作尚未结束：模态弹窗的嵌套消息循环里用户可能又点了托盘
        return true;
    }

    switch (id) {
        case kIdStatus:
            ActShowStatus();
            return true;
        case kIdPaths:
            ActShowPaths();
            return true;
        case kIdPrivacy:
            TogglePrivacyScreen();
            return true;
        case kIdOpenOut:
            // ShellExecute 会失败（资源管理器被替换、路径不存在、被策略拦截），
            // 静默失败会让人以为菜单点了没反应
            if (!OpenFolderInExplorer(GetOutDirPath())) {
                Info(L"打开输出目录失败",
                     L"无法调用资源管理器打开：\r\n" + GetOutDirPath() +
                         L"\r\n\r\nShellExecute 返回失败，可能是资源管理器未运行或被策略限制。");
            }
            return true;
        case kIdViewLog:
            ActShowRecentLog();
            return true;
        case kIdAbout:
            ActShowAbout();
            return true;
        case kIdExit:
            // 投递 WM_CLOSE 而不是直接 DestroyWindow：这里是本窗口的 WndProc
            // 调用栈内部，异步关闭更稳。WM_CLOSE 里会置停止标志，
            // Watcher 的循环随即退出，进程正常收尾。
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return true;

        case kIdAutoGuard:
            ActAutostart(true, true);
            return true;
        case kIdAutoOnce:
            ActAutostart(true, false);
            return true;
        case kIdAutoOff:
            ActAutostart(false, false);
            return true;
        case kIdAutoStatus: {
            AutostartState as = QueryAutostart();
            std::wstring t;
            t += L"虚拟屏初始化任务 : ";
            t += as.initRegistered ? L"已注册" : L"未注册";
            t += L"\r\n    任务名 " + std::wstring(L"ResProbe VDisplay Init") + L"\r\n\r\n";
            t += L"常驻守卫任务     : ";
            t += as.guardRegistered ? L"已注册" : L"未注册";
            t += L"\r\n    任务名 " + std::wstring(L"ResProbe Guard") + L"\r\n\r\n";
            t += L"其它 resprobe 进程 : " + FormatCount(as.resprobeProcessCount) +
                 L" 个（通常即常驻守卫）\r\n";
            Info(L"开机自启动状态", t);
            return true;
        }

        case kIdInstall:
            ActRunScriptAction(
                L"install", L"", L"tray-install.log", L"安装 / 修复虚拟显示器",
                L"安装完成。虚拟显示器已就绪，物理屏断电时桌面不会再回退。");
            return true;
        case kIdUninstall:
            ActRunScriptAction(
                L"uninstall", L"", L"tray-uninstall.log", L"卸载（保留驱动）",
                L"已卸载虚拟显示器与计划任务。驱动包保留，可随时重新安装。");
            return true;
        case kIdUninstallFull:
            ActRunScriptAction(
                L"uninstall", L"/full", L"tray-uninstall-full.log", L"彻底卸载（移除驱动）",
                L"已移除驱动。需要重新安装时会重新创建驱动与虚拟显示器。");
            return true;

        default:
            break;
    }

    // 分辨率项
    if (id >= kIdResBase && id <= kIdResLast) {
        const size_t idx = (size_t)(id - kIdResBase);
        if (idx < modes_.size()) {
            ActApplyResolution(modes_[idx].width, modes_[idx].height);
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
void TrayIcon::ActShowStatus() {
    if (busy_) return;

    busy_ = true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    RefreshStatusCache(true);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    const std::wstring text = BuildStatusReport(status_, refW_, refH_);
    RefreshTip();

    // busy_ 保持到弹窗之后才清：MessageBox 有自己的嵌套消息循环，
    // 期间用户若又点了托盘，busy_ 能拦住叠菜单
    Info(L"ResProbe 状态报告", text);
    busy_ = false;
}

void TrayIcon::ActShowRecentLog() {
    if (busy_) return;

    busy_ = true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    const std::wstring dir = GetOutDirPath();
    const std::wstring newest = FindNewestLogFile(dir);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (newest.empty()) {
        Info(L"最近的日志",
             L"输出目录里还没有 .log 文件：\r\n" + dir +
                 L"\r\n\r\n先执行一次监听或托盘菜单里的安装动作即可生成。");
        busy_ = false;
        return;
    }

    std::wstring tail;
    const bool ok = ReadLogTail(newest, 40, &tail);

    std::wstring text = L"文件：" + newest + L"\r\n\r\n";
    if (ok && !tail.empty()) {
        // 日志里 UTF-8 与 GBK 并存（resprobe 输出 UTF-8，cmd 回显中文路径、
        // schtasks 等系统工具输出 OEM 936），因此这里由程序解码后展示，
        // 直接用记事本打开会有一半是乱码。
        text += tail;
    } else {
        text += L"(读取失败或日志为空)";
    }

    Info(L"最近的日志（尾部 40 行）", text);
    busy_ = false;
}

void TrayIcon::ActShowPaths() {
    if (busy_) return;

    busy_ = true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::wstring text = BuildPathsReport(refW_, refH_);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    Info(L"显示输出路径", text);
    busy_ = false;
}

void TrayIcon::ActApplyResolution(UINT32 w, UINT32 h) {
    if (busy_) return;

    const std::wstring want = FormatModeText(w, h);
    if (want == currentMode_) {
        Info(L"调整分辨率", L"当前已经是 " + want + L"，无需调整。");
        return;
    }

    busy_ = true;
    SetTip(L"ResProbe 正在切换到 " + want + L" ...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    ResolutionResult rr = ApplyResolution(w, h);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    // 分辨率变了，缓存全部作废
    RefreshModeCache(true);
    RefreshStatusCache(true);
    RefreshTip();

    const bool ok = (rr.applied && rr.after == want);
    std::wstring text = rr.message;
    if (ok) {
        text += L"\r\n\r\n回写前：" + rr.before + L"\r\n回写后：" + rr.after;
    }

    Info(ok ? L"调整分辨率成功" : L"调整分辨率失败", text);
    busy_ = false;
}

void TrayIcon::ActAutostart(bool enable, bool guardMode) {
    if (busy_) return;

    if (!enable) {
        // 停用会移除兜底保护，先确认
        const int ans = MessageBoxW(
            nullptr,
            L"停用开机自启动会移除全部 ResProbe 计划任务。\n\n"
            L"之后物理关闭显示器仍可能让桌面回退到 1024x768，需要手动运行程序恢复。\n\n"
            L"确定要停用吗？",
            L"停用开机自启动", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (ans != IDYES) return;
    }

    busy_ = true;
    SetTip(L"ResProbe 正在更新开机自启动 ...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring message;
    const bool ok = SetAutostart(enable, guardMode, &message);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    AutostartState as = QueryAutostart();
    RefreshTip();

    std::wstring text = message;
    text += L"\r\n\r\n当前状态：\r\n";
    text += L"  虚拟屏初始化 : ";
    text += as.initRegistered ? L"已注册" : L"未注册";
    text += L"\r\n  常驻守卫     : ";
    text += as.guardRegistered ? L"已注册" : L"未注册";

    Info(ok ? L"开机自启动" : L"开机自启动设置失败", text);
    busy_ = false;
}

void TrayIcon::ActRunScriptAction(const std::wstring& role, const std::wstring& args,
                                  const std::wstring& logName,
                                  const std::wstring& title,
                                  const std::wstring& successText) {
    if (busy_) return;

    // 隐私屏在位期间拒绝执行脚本类动作：RunProjectScript 同步等待最长 5 分钟，
    // 期间托盘线程不泵消息 —— 遮罩的 100ms 重抢会停（任务栏等可趁虚覆盖遮罩，
    // 隐私出现窗口期），LL 钩子事件也会因 LowLevelHooksTimeout(约 300ms) 被
    // 系统跳过（privacy.h「连点解除」段落的 ⚠ 注释）。菜单里已置灰，这里再拦
    // 一道防其它入口，并用气球说清原因（同样不能弹模态框）。
    if (privacy_.Active()) {
        LogLine("[托盘] 已拒绝执行脚本动作（隐私屏开启中）：" + W2U8(title));
        ShowBalloon(L"隐私屏开启中，已拒绝执行",
                    L"「" + title + L"」需要长时间同步等待，会影响隐私屏遮罩。\n"
                    L"请先关闭隐私屏，再执行该操作。");
        return;
    }

    const int ans = MessageBoxW(
        nullptr,
        (L"即将执行：" + title +
         L"\n\n这会修改系统状态（显示驱动 / 设备节点 / 显示拓扑 / 计划任务），"
         L"可能需要 10~60 秒。\n执行期间请勿重复点击托盘菜单。\n\n确定继续吗？")
            .c_str(),
        title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (ans != IDYES) return;

    busy_ = true;
    SetTip(L"ResProbe 正在执行：" + title + L" ...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    ScriptRunResult sr = RunProjectScript(GetHomeDir(), role, args, logName, 300000);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    RefreshStatusCache(true);
    RefreshModeCache(true);
    RefreshTip();

    std::wstring text;

    if (!sr.found) {
        text = L"未找到所需的脚本。\n\n"
               L"该脚本需要在独立一行声明角色标记：\n"
               L"    @rem RESPROBE-ROLE:" + role;
        Info(L"找不到脚本", text);
        busy_ = false;
        return;
    }

    if (sr.ok) {
        text = successText;
    } else {
        text = L"执行未成功。";
        wchar_t codeBuf[32];
        _snwprintf_s(codeBuf, _countof(codeBuf), _TRUNCATE, L"0x%08lX",
                     (unsigned long)sr.exitCode);
        text += L"\r\n退出码：" + std::to_wstring(sr.exitCode) + L"（" + codeBuf + L"）";
        if (sr.timedOut) {
            text += sr.killed
                        ? L"\r\n注意：等待超时（5 分钟），脚本及其子进程已被强制终止，"
                          L"系统可能停在中间状态，请重跑一次。"
                        : L"\r\n注意：等待超时（5 分钟）且未能终止脚本，"
                          L"它可能仍在后台继续修改系统。";
        }
    }

    text += L"\r\n耗时：" + std::to_wstring(sr.elapsedMs / 1000) + L" 秒";
    text += L"\r\n日志：" + sr.logPath;
    if (!sr.tail.empty()) {
        text += L"\r\n\r\n--- 日志尾部 ---\r\n" + sr.tail;
    }

    Info(sr.ok ? title : (title + L"（失败）"), text);
    busy_ = false;
}

void TrayIcon::ActShowAbout() {
    std::wstring t;
    t += std::wstring(L"ResProbe ") + U82W(RESPROBE_VERSION) + L"\r\n\r\n";
    t += L"Windows 显示器分辨率变化探测与远程桌面诊断工具。\r\n\r\n";
    t += L"本工具用于定位「关闭显示器后分辨率回退到 1024x768」这类问题：\r\n"
         L"  - 区分是主机显示链路（EDID/HPD）问题，还是远程软件问题\r\n"
         L"  - 通过 usbmmidd 虚拟显示器提供兜底，物理屏断电时保持 1920x1080\r\n"
         L"  - 常驻监听显示事件，链路断开时自动接管\r\n\r\n";
    t += L"程序目录：" + GetHomeDir() + L"\r\n";
    t += L"输出目录：" + GetOutDirPath() + L"\r\n\r\n";
    t += L"命令行子命令：snapshot / watch / verify / paths / ensure / vdisplay / restore / tray / selftest";

    Info(L"关于 ResProbe", t);
}

void TrayIcon::Info(const std::wstring& title, const std::wstring& text) {
    // 宿主窗口是隐藏的，用 nullptr 作为 owner 让对话框成为顶层窗口，
    // 并显式要求置前台，否则可能被其它窗口挡住。
    MessageBoxW(nullptr, text.c_str(), title.c_str(),
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    if (!iconInstalled_) return;

    // 刻意不调用 NIM_SETVERSION（见 AddNotificationIcon）：保持旧版回调语义。
    // 旧版气球按 uTimeout 显示后自动消失，不需要用户点击 —— 这正是选它而不选
    // 模态弹窗的原因：托盘线程不会被扣押，也不存在"看得见点不掉"的死结。
    // 旧版语义下 uTimeout 会被钳制在 [10s, 30s]。
    nid_.uFlags = NIF_INFO;
    nid_.dwInfoFlags = NIIF_INFO;
    nid_.uTimeout = 10000;
    wcsncpy_s(nid_.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid_.szInfo, text.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid_)) {
        // 气球被系统策略禁用时不算失败：图标与提示已经变化，确认信息不丢
        LogLine("[托盘] 气球通知未能显示（可能被系统策略禁用），仅图标与提示变化");
    }
}

}  // namespace resprobe
