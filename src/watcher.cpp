// watcher.cpp - 显示相关事件的持续监听
#include "watcher.h"
#include "display.h"
#include "report.h"
#include "util.h"
#include "verify.h"

#include <wtsapi32.h>

namespace resprobe {

namespace {

volatile LONG g_stopRequested = 0;
const wchar_t* kWindowClassName = L"ResProbeWatcherWindow";
const int kHeartbeatTimerId = 1;

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            InterlockedExchange(&g_stopRequested, 1);
            return TRUE;
        default:
            return FALSE;
    }
}

LRESULT CALLBACK WatcherWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Watcher* self = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<Watcher*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Watcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        self->OnWindowMessage(hwnd, msg, wp, lp);
    }

    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void RequestStop() {
    InterlockedExchange(&g_stopRequested, 1);
}

bool StopRequested() {
    return InterlockedCompareExchange(&g_stopRequested, 0, 0) != 0;
}

// ---------------------------------------------------------------------------
Watcher::Watcher(Report& report) : report_(report) {}

Watcher::~Watcher() {
    DestroyHostWindow();
}

// ---------------------------------------------------------------------------
bool Watcher::CreateHostWindow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WatcherWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    // 必须是顶层窗口：广播消息（WM_DISPLAYCHANGE / WM_DEVICECHANGE /
    // WM_SETTINGCHANGE）不会投递给 message-only 窗口。
    hwnd_ = CreateWindowExW(
        0, kWindowClassName, L"ResProbeWatcher",
        WS_OVERLAPPED, 0, 0, 0, 0,
        nullptr, nullptr, hInst, this);

    if (!hwnd_) return false;

    // 保持窗口隐藏（不调用 ShowWindow 即可）
    return true;
}

void Watcher::DestroyHostWindow() {
    if (powerNotify_) {
        UnregisterPowerSettingNotification(powerNotify_);
        powerNotify_ = nullptr;
    }
    if (devNotify_) {
        UnregisterDeviceNotification(devNotify_);
        devNotify_ = nullptr;
    }
    if (wtsRegistered_) {
        WTSUnRegisterSessionNotification(hwnd_);
        wtsRegistered_ = false;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
void Watcher::RegisterNotifications() {
    // 显示器电源状态（开/关/低功耗）
    powerNotify_ = RegisterPowerSettingNotification(
        hwnd_, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!powerNotify_) {
        LogLine("  [提示] 显示器电源状态通知注册失败（将依赖拓扑推断电源状态）");
    }

    // 监视器设备接口到达/移除
    DEV_BROADCAST_DEVICEINTERFACE_W filter;
    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kGuidDevInterfaceMonitor;
    devNotify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!devNotify_) {
        LogLine("  [提示] 监视器设备通知注册失败（将依赖 WM_DISPLAYCHANGE 事件）");
    }

    // 会话切换（远程连接/断开）
    wtsRegistered_ = (WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE);
    if (!wtsRegistered_) {
        LogLine("  [提示] 会话通知注册失败（将不跟踪会话切换事件）");
    }
}

// ---------------------------------------------------------------------------
void Watcher::OnPowerSetting(WPARAM, LPARAM lp) {
    auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
    if (!setting) return;

    if (IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState) &&
        setting->DataLength >= sizeof(DWORD)) {
        DWORD state = *(DWORD*)setting->Data;
        switch (state) {
            case 0: powerState_ = DisplayPowerState::Off; break;
            case 1: powerState_ = DisplayPowerState::On; break;
            case 2: powerState_ = DisplayPowerState::Dimmed; break;
            default: powerState_ = DisplayPowerState::Unknown; break;
        }

        ++report_.powerNotificationCount;

        std::string t = "电源通知(显示器状态)=" + W2U8(DisplayPowerStateName(powerState_));
        LogLine("[电源] " + LocalTimeText() + " " + t);

        if (report_.powerTransitions.size() < 300) {
            report_.powerTransitions.push_back(
                NowIso8601Utc() + " " + W2U8(DisplayPowerStateName(powerState_)) +
                " (uptimeMs=" + std::to_string(MonotonicMs() - startMs_) + ")");
        }
        ScheduleSample(t);
    }
}

// ---------------------------------------------------------------------------
void Watcher::OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DISPLAYCHANGE: {
            char buf[160];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "WM_DISPLAYCHANGE(%ux%u %ubpp)",
                        (unsigned)LOWORD(lp), (unsigned)HIWORD(lp), (unsigned)wp);
            ScheduleSample(buf);
            break;
        }

        case WM_SETTINGCHANGE: {
            if (wp == SPI_SETWORKAREA || wp == SPI_SETDESKWALLPAPER) {
                // 工作区变化往往伴随分辨率变化，但并非充分条件
            }
            char buf[96];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "WM_SETTINGCHANGE(0x%04X)", (unsigned)wp);
            ScheduleSample(buf);
            break;
        }

        case WM_DEVICECHANGE: {
            const char* what = nullptr;
            switch (wp) {
                case DBT_DEVICEARRIVAL:         what = "设备到达"; break;
                case DBT_DEVICEREMOVECOMPLETE:  what = "设备移除"; break;
                case DBT_DEVNODES_CHANGED:      what = "设备节点变化"; break;
                case DBT_CONFIGCHANGED:         what = "配置变化"; break;
                default:                        what = nullptr; break;
            }
            if (what) {
                std::string t = std::string("WM_DEVICECHANGE(") + what + ")";
                ScheduleSample(t);
            }
            break;
        }

        case WM_POWERBROADCAST: {
            if (wp == PBT_POWERSETTINGCHANGE) {
                OnPowerSetting(wp, lp);
            } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                LogLine("[电源] 系统从睡眠恢复");
                ScheduleSample("PBT_APMRESUME");
            } else if (wp == PBT_APMSUSPEND) {
                LogLine("[电源] 系统即将进入睡眠");
            }
            break;
        }

        case WM_WTSSESSION_CHANGE: {
            const char* what = "未知";
            switch (wp) {
                case WTS_CONSOLE_CONNECT:    what = "控制台连接"; break;
                case WTS_CONSOLE_DISCONNECT: what = "控制台断开"; break;
                case WTS_REMOTE_CONNECT:     what = "远程连接"; break;
                case WTS_REMOTE_DISCONNECT:  what = "远程断开"; break;
                case WTS_SESSION_LOGON:      what = "会话登录"; break;
                case WTS_SESSION_LOGOFF:     what = "会话注销"; break;
                case WTS_SESSION_LOCK:       what = "会话锁定"; break;
                case WTS_SESSION_UNLOCK:     what = "会话解锁"; break;
                default: break;
            }
            char buf[160];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "WM_WTSSESSION_CHANGE(%s, 会话=%lu)", what, (unsigned long)lp);
            ScheduleSample(buf);
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
void Watcher::ScheduleSample(const std::string& trigger) {
    UINT64 now = MonotonicMs();
    UINT64 settle = (UINT64)(report_.opt.settleMs > 0 ? report_.opt.settleMs : 1);

    if (pending_) {
        // 合并窗口内连续事件，避免同一轮变化产生多条记录
        if (pendingTrigger_.find(trigger) == std::string::npos) {
            pendingTrigger_ += " + " + trigger;
        }
        // 关键：合并窗口最多延长到首事件之后的 kMaxDeferFactor 倍 settleMs。
        // 否则在持续不断的通知风暴中（例如显示器断电时 HDMI 链路反复协商），
        // 采样会被无限推迟，整个变化过程会被完全漏掉。
        const UINT64 kMaxDeferFactor = 3;
        UINT64 deadline = pendingFirstMs_ + settle * kMaxDeferFactor;
        UINT64 proposed = now + settle;
        pendingAt_ = (proposed < deadline) ? proposed : deadline;
    } else {
        pending_ = true;
        pendingTrigger_ = trigger;
        pendingFirstMs_ = now;
        pendingAt_ = now + settle;
    }
}

// ---------------------------------------------------------------------------
void Watcher::TakeSample(const std::string& trigger) {
    DisplaySnapshot snap = CaptureSnapshot(trigger, report_.opt.referenceWidth,
                                           report_.opt.referenceHeight);
    snap.powerState = (powerState_ != DisplayPowerState::Unknown)
                          ? powerState_
                          : snap.powerState;

    EventRecord ev;
    ev.timeUtc = snap.timeUtc;
    ev.uptimeMs = snap.uptimeMs;
    ev.trigger = trigger;
    ev.snapshot = snap;

    if (hasLast_) {
        DiffSnapshots(last_, snap, &ev.changedFields, &ev.transitions);
        ev.changed = !ev.changedFields.empty();
    } else {
        ev.changed = true;
        ev.transitions.push_back("首次采样");
    }

    ev.index = (int)report_.events.size() + 1;

    // 只保留有实际变化的事件；同时对超长监听设置上限，避免报告无限增长。
    // 达到上限后不再追加，而是持续用最新状态覆盖最后一条“尾事件”，
    // 这样既有界，又保证最终状态准确。
    const size_t kMaxEvents = 1000;
    if (ev.changed) {
        if (report_.events.size() < kMaxEvents) {
            report_.events.push_back(ev);
            PrintEventConsole(ev);
            PrintSnapshotConsole(snap, false);
        } else {
            ev.index = (int)report_.events.size();
            ev.transitions.insert(ev.transitions.begin(),
                                  "事件数量已达上限，本条目持续覆盖为最新状态");
            report_.events.back() = ev;
            if (!limitWarned_) {
                limitWarned_ = true;
                LogLine("[警告] 事件数量已达上限，后续变化将覆盖最后一条记录");
            }
        }
    } else if (report_.opt.verbose) {
        LogLine("[事件] " + trigger + "（拓扑未发生实际变化，已忽略）");
    }

    last_ = snap;
    hasLast_ = true;

    // ---- 守卫：物理链路断开且虚拟屏可用时，自动把虚拟屏提升为主屏 ----
    // 使用冷却窗口而不是一次性标志：显示器可能被反复关开，每次都应当接管。
    if (report_.opt.guard) {
        const UINT64 kGuardCooldownMs = 8000;
        UINT64 now = MonotonicMs();

        bool hasVirtual = false;
        bool deadPhysical = false;
        std::wstring primaryMode;
        for (const DisplayTarget& t : snap.targets) {
            if (!t.active) continue;
            if (IsVirtualDisplayTarget(t)) {
                hasVirtual = true;
            } else if (!IsLinkHealthy(t)) {
                // M-2：与 ClassifyOutputNature / EvaluateTopology 共用同一判据。
                // 只看 edid.present 会让三处结论漂移（一处健康、一处断链）。
                deadPhysical = true;
            }
            if (t.gdiPrimary) primaryMode = FormatModeText(t.width, t.height);
        }

        std::wstring want = FormatModeText(report_.opt.referenceWidth,
                                           report_.opt.referenceHeight);
        bool alreadyOk = (primaryMode == want);

        bool cooldownPassed =
            (lastGuardMs_ == 0) || (now - lastGuardMs_ >= kGuardCooldownMs);

        // 需要动作的两种情况：
        //   1) 主屏没达到参考分辨率；
        //   2) 桌面上还挂着链路已断的死输出（它会把整体桌面尺寸拖坏）
        bool needAction = (!alreadyOk) || deadPhysical;

        if (hasVirtual && needAction && cooldownPassed) {
            lastGuardMs_ = now;
            ++guardActionCount_;

            LogLine("[守卫] 检测到主屏未达到参考分辨率（当前 " + W2U8(primaryMode) +
                    (deadPhysical ? "，且存在链路断开的物理输出" : "") +
                    "）且虚拟屏可用，自动接管 ...");

            PromoteOutcome po = PromoteVirtualDisplay(report_.opt.referenceWidth,
                                                      report_.opt.referenceHeight, true);
            for (const std::string& s : po.log) LogLine("[守卫] " + s);
            LogLine("[守卫] " + W2U8(po.message));
            report_.actionLog.push_back("guard#" + std::to_string(guardActionCount_) +
                                        ": " + W2U8(po.message));

            DisplaySnapshot fixed = CaptureSnapshot("guard-promoted",
                                                    report_.opt.referenceWidth,
                                                    report_.opt.referenceHeight);
            PrintSnapshotConsole(fixed, false);
            last_ = fixed;
            hasLast_ = true;
        }
    }
}

// ---------------------------------------------------------------------------
int Watcher::Run() {
    if (!CreateHostWindow()) {
        LogLine("错误: 无法创建监听窗口，事件监听不可用");
        return 3;
    }

    RegisterNotifications();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    startMs_ = MonotonicMs();
    lastHeartbeatMs_ = startMs_;

    // 首次采样
    {
        DisplaySnapshot snap = CaptureSnapshot("initial", report_.opt.referenceWidth,
                                               report_.opt.referenceHeight);
        report_.initial = snap;
        last_ = snap;
        hasLast_ = true;
        EventRecord ev;
        ev.index = 1;
        ev.timeUtc = snap.timeUtc;
        ev.uptimeMs = snap.uptimeMs;
        ev.trigger = "initial";
        ev.changed = true;
        ev.transitions.push_back("基线快照");
        ev.changedFields.push_back("initial");
        ev.snapshot = snap;
        report_.events.push_back(ev);
    }

    LogLine("监听已启动。请在监听期间复现问题（关闭显示器 / 连接远程软件）。按 Ctrl+C 结束。");
    if (report_.opt.durationSec > 0) {
        LogLine("本次监听时长上限: " + std::to_string(report_.opt.durationSec) + " 秒");
    }
    LogLine("------------------------------------------------------------------");

    while (!StopRequested()) {
        DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);

        if (wait == WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    RequestStop();
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        UINT64 now = MonotonicMs();

        if (pending_ && now >= pendingAt_) {
            std::string trigger = pendingTrigger_;
            pending_ = false;
            pendingTrigger_.clear();
            TakeSample(trigger);
            lastHeartbeatMs_ = now;
        }

        if (!pending_ && report_.opt.intervalSec > 0) {
            UINT64 intervalMs = (UINT64)report_.opt.intervalSec * 1000ULL;
            if (now - lastHeartbeatMs_ >= intervalMs) {
                lastHeartbeatMs_ = now;
                TakeSample("heartbeat");
            }
        }

        if (report_.opt.durationSec > 0 &&
            now - startMs_ >= (UINT64)report_.opt.durationSec * 1000ULL) {
            LogLine("已达到监听时长上限，停止监听。");
            pending_ = false;
            break;
        }
    }

    // 收尾：强制采样一次，确保最终状态被记录
    TakeSample(pending_ ? pendingTrigger_ : std::string("final-state"));

    LogLine("[电源] 监听期间共收到 " + std::to_string(report_.powerNotificationCount) +
            " 条显示器电源状态通知");

    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    DestroyHostWindow();
    return 0;
}

}  // namespace resprobe
